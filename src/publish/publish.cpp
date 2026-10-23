#include "publish/publish.h"

#include "model/composite.h"
#include "publish/encode.h"
#include "publish/signing_key.h"
#include "store/history.h"
#include "wire/read.h"

#include <QDir>
#include <QFile>
#include <QSaveFile>

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include <map>
#include <thread>

namespace {

/*
 * Write `bytes` to `path` unless it already holds exactly them. Aside and
 * renamed, never written through, so a reader sees the old file or the
 * new one and nothing between.
 */
bool write_if_changed(const QString &path, const QByteArray &bytes, bool *changed,
                      QString *why) {
	QFile existing(path);
	if (existing.size() == bytes.size() && existing.open(QIODevice::ReadOnly) &&
	    existing.readAll() == bytes) {
		*changed = false;
		return true;
	}

	QSaveFile out(path);
	out.setDirectWriteFallback(false);
	if (!out.open(QIODevice::WriteOnly) || out.write(bytes) != bytes.size()) {
		*why = QStringLiteral("cannot write %1: %2").arg(path, out.errorString());
		out.cancelWriting();
		return false;
	}
	out.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner |
	                   QFileDevice::ReadGroup | QFileDevice::ReadOther);
	if (!out.commit()) {
		*why = QStringLiteral("cannot replace %1: %2").arg(path, out.errorString());
		return false;
	}

	*changed = true;
	return true;
}

uint16_t read_u16(const QByteArray &b, int at) {
	return uint16_t(uint8_t(b[at]) | uint8_t(b[at + 1]) << 8);
}

uint32_t read_u32(const QByteArray &b, int at) {
	return uint32_t(uint8_t(b[at])) | uint32_t(uint8_t(b[at + 1])) << 8 |
	       uint32_t(uint8_t(b[at + 2])) << 16 | uint32_t(uint8_t(b[at + 3])) << 24;
}

qint64 read_i64(const QByteArray &b, int at) {
	uint64_t value = 0;
	for (int i = 7; i >= 0; --i) {
		value = value << 8 | uint8_t(b[at + i]);
	}
	return qint64(value);
}

/*
 * The index this publisher wrote last time, so unchanged months can be
 * skipped. Its own file, but checked as though it were not: the
 * signature against this key, then every length against the bytes.
 * Anything wrong reads as "no previous index", which costs a full
 * re-encode and nothing worse.
 */
std::map<uint16_t, bbq_wire_index_entry> previous_index(const QString &path,
                                                       const QString &station,
                                                       const bbq_signing_key &key) {
	std::map<uint16_t, bbq_wire_index_entry> entries;

	QFile in(path);
	if (in.size() > 1024 * 1024 || !in.open(QIODevice::ReadOnly)) {
		return entries;
	}
	const QByteArray file = in.readAll();
	if (file.size() < 10 + 64 || !file.startsWith("BBQ1") ||
	    uint8_t(file[4]) != uint8_t(bbq_wire_kind::index) || file[5] != 0) {
		return entries;
	}
	const qint64 body_length = read_u32(file, 6);
	if (10 + body_length + 64 != file.size() ||
	    !bbq_signature_valid(file.right(64), key.public_key(), file.left(file.size() - 64))) {
		return entries;
	}

	const QByteArray body = file.mid(10, int(body_length));
	const QByteArray id = station.toLatin1();
	if (body.size() < 1 + id.size() + 8 + 2 || uint8_t(body[0]) != id.size() ||
	    body.mid(1, id.size()) != id) {
		return entries;
	}

	int at = 1 + id.size() + 8;
	const int count = read_u16(body, at);
	at += 2;
	if (body.size() != at + count * 42) {
		return entries;
	}

	for (int i = 0; i < count; ++i, at += 42) {
		bbq_wire_index_entry entry;
		entry.month = read_u16(body, at);
		entry.rows = read_u32(body, at + 2);
		entry.bytes = read_u32(body, at + 6);
		entry.digest = body.mid(at + 10, 16);
		entry.first_utc = read_i64(body, at + 26);
		entry.last_utc = read_i64(body, at + 34);
		entries[entry.month] = entry;
	}
	return entries;
}

bool same_entries(const std::vector<bbq_wire_index_entry> &a,
                  const std::map<uint16_t, bbq_wire_index_entry> &b) {
	if (a.size() != b.size()) {
		return false;
	}
	for (const bbq_wire_index_entry &entry : a) {
		const auto found = b.find(entry.month);
		if (found == b.end() || found->second.rows != entry.rows ||
		    found->second.bytes != entry.bytes || found->second.digest != entry.digest ||
		    found->second.first_utc != entry.first_utc ||
		    found->second.last_utc != entry.last_utc) {
			return false;
		}
	}
	return true;
}

/*
 * One publisher at a time per tree: two cron entries for two stations
 * both rewrite the manifest. Bounded, because a lock that waits for ever
 * is a cron job that piles up for ever.
 */
class tree_lock {
public:
	explicit tree_lock(const QString &dir) {
		const QByteArray path = QFile::encodeName(dir + QStringLiteral("/.publish.lock"));
		m_fd = ::open(path.constData(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0644);
		if (m_fd < 0) {
			return;
		}
		for (int attempt = 0; attempt < 120; ++attempt) {
			if (::flock(m_fd, LOCK_EX | LOCK_NB) == 0) {
				m_held = true;
				return;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(500));
		}
	}

	~tree_lock() {
		if (m_fd >= 0) {
			::close(m_fd);
		}
	}

	bool held() const { return m_held; }

private:
	int m_fd = -1;
	bool m_held = false;
};

} // namespace

int bbq_publish(const QString &dir, const QString &station,
                const bbq_composite *composite, const bbq_history &history,
                const bbq_signing_key &key, qint64 now_utc, QTextStream &out,
                QTextStream &error) {
	if (!bbq_wire_station_valid(station)) {
		error << "publish: station id '" << station
		      << "' cannot be published: it must be 1 to 32 of A-Z and 0-9\n";
		return 1;
	}
	if (!key.is_valid()) {
		error << "publish: no signing key\n";
		return 1;
	}

	const QString station_dir = dir + QLatin1Char('/') + station;
	if (!QDir().mkpath(station_dir)) {
		error << "publish: cannot create " << station_dir << "\n";
		return 1;
	}

	tree_lock lock(dir);
	if (!lock.held()) {
		error << "publish: another publish held " << dir << " for a minute\n";
		return 1;
	}

	QString why;
	bool changed = false;
	int written = 0;

	/* The months, skipping every one the last index already describes. */
	const QString index_path = station_dir + QStringLiteral("/index");
	const auto previous = previous_index(index_path, station, key);
	std::vector<bbq_wire_index_entry> entries;

	for (const bbq_history::observation_month &summary : history.observation_months(station)) {
		const uint16_t month = bbq_wire_month_of(summary.begin_utc);
		const QString path = station_dir + QLatin1Char('/') + bbq_wire_month_name(month);

		const auto old = previous.find(month);
		if (old != previous.end() && old->second.rows == uint32_t(summary.rows) &&
		    old->second.first_utc == summary.first_utc &&
		    old->second.last_utc == summary.last_utc &&
		    QFile(path).size() == qint64(old->second.bytes)) {
			entries.push_back(old->second);
			continue;
		}

		bbq_wire_month_rows rows;
		for (const auto &row : history.observation_rows(
		             station, bbq_wire_month_begin(month), bbq_wire_month_end(month))) {
			int provider = rows.providers.indexOf(row.provider);
			if (provider < 0) {
				rows.providers.append(row.provider);
				provider = int(rows.providers.size()) - 1;
			}
			rows.samples.push_back(row.sample);
			rows.provider.push_back(uint8_t(provider));
		}
		if (rows.samples.empty()) {
			continue;
		}

		const QByteArray body = bbq_wire_month_body(station, month, rows, &why);
		if (body.isEmpty()) {
			error << "publish: " << bbq_wire_month_name(month) << ": " << why << "\n";
			return 1;
		}
		const QByteArray file = bbq_wire_file(bbq_wire_kind::month, body, key);
		if (!write_if_changed(path, file, &changed, &why)) {
			error << "publish: " << why << "\n";
			return 1;
		}
		written += changed ? 1 : 0;

		bbq_wire_index_entry entry;
		entry.month = month;
		entry.rows = uint32_t(rows.samples.size());
		entry.bytes = uint32_t(file.size());
		entry.digest = bbq_wire_digest(file);
		entry.first_utc = rows.samples.front().start_utc;
		entry.last_utc = rows.samples.back().start_utc;
		entries.push_back(entry);
	}

	/* The index, only when a month changed: a client compares it to decide what to fetch. */
	if (!entries.empty() && !same_entries(entries, previous)) {
		const QByteArray body = bbq_wire_index_body(station, now_utc, entries, &why);
		if (body.isEmpty() ||
		    !write_if_changed(index_path, bbq_wire_file(bbq_wire_kind::index, body, key),
		                      &changed, &why)) {
			error << "publish: index: " << why << "\n";
			return 1;
		}
		written += changed ? 1 : 0;
	}

	/*
	 * The head: what this round fetched, each band stamped with when.
	 * A round where nothing arrived leaves the last head alone rather
	 * than replacing good bands with none; the client already keeps a
	 * band a head omits, under its old fetch time.
	 */
	std::vector<const bbq_series *> bands;
	bool any_band = false;

	/*
	 * A BAND THIS ROUND DID NOT FETCH IS CARRIED FROM THE LAST HEAD
	 * (sec 15.16). The publisher's rounds fetch each band only when its
	 * interval is up, so most rounds bring the current reading alone, and
	 * a head of that alone would hand a new client no forecast for up to
	 * an hour. The last head is read back as a client reads it, signature
	 * against this key and all, so a damaged one is carried from not at
	 * all; and each carried band keeps its own fetch time, so a client
	 * still sees how old it is.
	 */
	bbq_wire_head previous_head;
	{
		QFile in(station_dir + QStringLiteral("/head"));
		QString unused;
		if (!(in.size() <= 1024 * 1024 && in.open(QIODevice::ReadOnly) &&
		      bbq_wire_read_head(in.readAll(), key.public_key(), station, &previous_head,
		                         &unused))) {
			previous_head.bands.clear();
		}
	}

	if (composite) {
		for (bbq_band band : {bbq_band::observed, bbq_band::current, bbq_band::nowcast_fine,
		                      bbq_band::nowcast, bbq_band::extended, bbq_band::hourly}) {
			const bbq_series *series = composite->band(band);
			if (series && !series->is_empty()) {
				any_band = true;
			} else {
				for (const bbq_series &carried : previous_head.bands) {
					if (carried.band() == band && !carried.is_empty()) {
						series = &carried;
					}
				}
			}
			bands.push_back(series);
		}
	}
	if (any_band) {
		/*
		 * Stamped with the newest band's FETCH time, not the publish
		 * time. The stamp exists so a client can refuse a head older
		 * than the one it holds, and the newest fetch serves that as
		 * well; the publish time would make two heads of identical bands
		 * differ, and a head that changes every run whatever it carries
		 * is one no client can ever be told is unchanged.
		 */
		qint64 newest = 0;
		for (const bbq_series *series : bands) {
			if (series && !series->is_empty()) {
				newest = std::max(newest, series->fetched_utc());
			}
		}
		const QByteArray body = bbq_wire_head_body(station, newest, bands, &why);
		if (body.isEmpty()) {
			error << "publish: head: " << why << "\n";
			return 1;
		}
		if (!write_if_changed(station_dir + QStringLiteral("/head"),
		                      bbq_wire_file(bbq_wire_kind::head, body, key), &changed, &why)) {
			error << "publish: " << why << "\n";
			return 1;
		}
		written += changed ? 1 : 0;
	}

	/*
	 * The manifest LAST: every station directory with an index, which is
	 * what makes it a station rather than a directory somebody made.
	 */
	QStringList stations;
	const QStringList names = QDir(dir).entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
	for (const QString &name : names) {
		if (bbq_wire_station_valid(name) && QFile::exists(dir + QLatin1Char('/') + name +
		                                                  QStringLiteral("/index"))) {
			stations.append(name);
		}
	}
	const QByteArray manifest = bbq_wire_stations_body(now_utc, stations, &why);
	if (manifest.isEmpty() ||
	    !write_if_changed(dir + QStringLiteral("/stations"),
	                      bbq_wire_file(bbq_wire_kind::stations, manifest, key), &changed,
	                      &why)) {
		error << "publish: stations: " << why << "\n";
		return 1;
	}

	out << "publish: " << station << ": " << entries.size() << " month(s), " << written
	    << " file(s) rewritten, " << stations.size() << " station(s) in " << dir << "\n";
	return 0;
}
