#include "server/transport.h"

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTcpSocket>
#include <QTimer>

#include <cstring>

namespace {

/* The daemon's response header, src/serve/serve_core.h. */
constexpr int response_header_size = 12;

uint32_t file_cap(bbq_wire_kind kind) {
	return kind == bbq_wire_kind::month ? 4u * 1024 * 1024 : 1024u * 1024;
}

/* The first eight bytes of the signature on a copy, as the daemon compares them. */
QByteArray have_of(const QByteArray &held) {
	return held.size() >= 64 ? held.right(64).left(8) : QByteArray();
}

} // namespace

QByteArray bbq_server_built_in_key() {
	/*
	 * The hex `bbq-predictor --generate-signing-key` prints as "public
	 * key". Empty until the publishing key exists; a wrong one is no
	 * worse than none, since nothing would verify and the providers are
	 * still asked once the policy gives up.
	 */
	static const char hex[] = "";
	const QByteArray key = QByteArray::fromHex(QByteArray(hex));
	return key.size() == 32 ? key : QByteArray();
}

std::vector<bbq_server_route> bbq_server_default_routes() {
	std::vector<bbq_server_route> routes(3);
	routes[0].type = bbq_server_route::kind::daemon;
	routes[0].host = QStringLiteral("vibes.se");
	routes[0].port = 7227;
	routes[1].type = bbq_server_route::kind::daemon;
	routes[1].host = QStringLiteral("vibes.se");
	routes[1].port = 443;
	routes[2].type = bbq_server_route::kind::https;
	routes[2].base = QUrl(QStringLiteral("https://vibes.se/bbq-predictor/"));
	return routes;
}

QByteArray bbq_server_encode_request(const bbq_server_request &request) {
	QByteArray out(48, '\0');
	std::memcpy(out.data(), "BBQR", 4);
	out[4] = 1;
	out[5] = char(request.kind);
	if (request.kind == bbq_wire_kind::month) {
		out[6] = char(request.month & 0xff);
		out[7] = char(request.month >> 8);
	}
	const QByteArray station = request.station.toLatin1().left(32);
	std::memcpy(out.data() + 8, station.constData(), size_t(station.size()));
	const QByteArray have = have_of(request.have);
	std::memcpy(out.data() + 40, have.constData(), size_t(have.size()));
	return out;
}

QString bbq_server_path(const bbq_server_request &request) {
	switch (request.kind) {
	case bbq_wire_kind::stations:
		return QStringLiteral("stations");
	case bbq_wire_kind::index:
		return request.station + QStringLiteral("/index");
	case bbq_wire_kind::head:
		return request.station + QStringLiteral("/head");
	case bbq_wire_kind::month:
		return request.station + QLatin1Char('/') + bbq_wire_month_name(request.month);
	}
	return QString();
}

bbq_server_client::bbq_server_client(QNetworkAccessManager *network, QObject *parent)
        : QObject(parent), m_network(network), m_routes(bbq_server_default_routes()),
          m_deadline(new QTimer(this)) {
	m_deadline->setSingleShot(true);
	connect(m_deadline, &QTimer::timeout, this, [this]() { route_failed(); });
}

bbq_server_client::~bbq_server_client() = default;

void bbq_server_client::set_routes(std::vector<bbq_server_route> routes) {
	m_routes = std::move(routes);
	m_preferred = 0;
}

void bbq_server_client::set_preferred_route(int index) {
	if (index >= 0 && index < int(m_routes.size())) {
		m_preferred = index;
	}
}

void bbq_server_client::fetch(std::vector<bbq_server_request> requests) {
	if (m_busy || m_routes.empty() || requests.empty()) {
		std::vector<bbq_server_result> failed(requests.size());
		QTimer::singleShot(0, this, [this, failed]() { emit finished(failed); });
		return;
	}

	m_busy = true;
	m_requests = std::move(requests);
	try_route(0);
}

void bbq_server_client::try_route(int attempt) {
	m_attempt = attempt;
	m_results.assign(m_requests.size(), bbq_server_result());
	m_received.clear();
	m_answered = 0;
	++m_generation;

	if (attempt >= int(m_routes.size())) {
		finish();
		return;
	}

	const bbq_server_route &route = m_routes[size_t((m_preferred + attempt) % int(m_routes.size()))];
	m_deadline->start(m_timeout_ms);

	if (route.type == bbq_server_route::kind::daemon) {
		try_daemon(route);
	} else {
		try_https(route, 0);
	}
}

void bbq_server_client::try_daemon(const bbq_server_route &route) {
	m_socket = new QTcpSocket(this);
	const quint64 generation = m_generation;

	connect(m_socket, &QTcpSocket::connected, this, [this, generation]() {
		if (generation != m_generation) {
			return;
		}
		/* Pipelined: every request at once, the answers in the same order. */
		QByteArray all;
		for (const bbq_server_request &request : m_requests) {
			all.append(bbq_server_encode_request(request));
		}
		m_socket->write(all);
	});
	connect(m_socket, &QTcpSocket::readyRead, this, [this, generation]() {
		if (generation != m_generation) {
			return;
		}
		m_received.append(m_socket->readAll());
		parse_daemon_replies();
	});
	connect(m_socket, &QTcpSocket::errorOccurred, this, [this, generation]() {
		if (generation == m_generation) {
			route_failed();
		}
	});
	connect(m_socket, &QTcpSocket::disconnected, this, [this, generation]() {
		if (generation == m_generation && m_answered < m_requests.size()) {
			route_failed();
		}
	});

	m_socket->connectToHost(route.host, route.port);
}

void bbq_server_client::parse_daemon_replies() {
	while (m_answered < m_requests.size()) {
		if (m_received.size() < response_header_size) {
			return;
		}

		const auto *header = reinterpret_cast<const uint8_t *>(m_received.constData());
		const uint32_t length = uint32_t(header[8]) | uint32_t(header[9]) << 8 |
		                        uint32_t(header[10]) << 16 | uint32_t(header[11]) << 24;
		const uint8_t status = header[4];
		const bbq_server_request &request = m_requests[m_answered];

		/* Anything but a well-formed answer abandons the route. */
		if (std::memcmp(header, "BBQA", 4) != 0 || status > 4 ||
		    length > file_cap(request.kind) || (status != 0 && length != 0)) {
			route_failed();
			return;
		}
		if (uint32_t(m_received.size()) < response_header_size + length) {
			return;
		}

		bbq_server_result &result = m_results[m_answered];
		switch (status) {
		case 0:
			result.status = bbq_server_status::ok;
			result.file = m_received.mid(response_header_size, int(length));
			break;
		case 1:
			result.status = bbq_server_status::unchanged;
			break;
		case 2:
			result.status = bbq_server_status::not_served;
			break;
		default:
			/* Bad request or busy: this route cannot answer it now. */
			route_failed();
			return;
		}

		m_received.remove(0, response_header_size + int(length));
		++m_answered;
	}

	route_answered();
}

void bbq_server_client::try_https(const bbq_server_route &route, size_t next) {
	if (next >= m_requests.size()) {
		route_answered();
		return;
	}

	const bbq_server_request &request = m_requests[next];
	QNetworkRequest get(route.base.resolved(QUrl(bbq_server_path(request))));
	get.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
	                 QNetworkRequest::NoLessSafeRedirectPolicy);
	get.setMaximumRedirectsAllowed(2);
	QNetworkReply *reply = m_network->get(get);
	m_reply = reply;
	const quint64 generation = m_generation;
	const uint32_t cap = file_cap(request.kind);

	/* Refused while it arrives, not after: a cap read in full is no cap. */
	connect(reply, &QNetworkReply::downloadProgress, this,
	        [reply, cap](qint64 received, qint64 total) {
		if (received > qint64(cap) || total > qint64(cap)) {
			reply->abort();
		}
	});
	connect(reply, &QNetworkReply::finished, this, [this, reply, route, next, generation, cap]() {
		reply->deleteLater();
		if (generation != m_generation) {
			return;
		}
		m_reply = nullptr;

		const int code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
		bbq_server_result &result = m_results[next];
		if (reply->error() == QNetworkReply::NoError && code == 200) {
			/*
			 * The cap again, on what arrived: the progress check above
			 * can lose the race to a fast link, and did on loopback --
			 * the whole oversized body landed before it could abort.
			 */
			const QByteArray file = reply->read(qint64(cap) + 1);
			if (uint32_t(file.size()) > cap) {
				route_failed();
				return;
			}
			const QByteArray &held = m_requests[next].have;
			if (!held.isEmpty() && file.size() >= 64 &&
			    file.right(64).left(8) == have_of(held)) {
				result.status = bbq_server_status::unchanged;
			} else {
				result.status = bbq_server_status::ok;
				result.file = file;
			}
		} else if (code == 404) {
			result.status = bbq_server_status::not_served;
		} else {
			route_failed();
			return;
		}
		try_https(route, next + 1);
	});
}

void bbq_server_client::route_answered() {
	m_preferred = (m_preferred + m_attempt) % int(m_routes.size());
	finish();
}

/* Whatever the abandoned route still has open, closed rather than left to finish. */
void bbq_server_client::drop_connections() {
	if (m_socket) {
		m_socket->disconnect(this);
		m_socket->abort();
		m_socket->deleteLater();
		m_socket = nullptr;
	}
	if (m_reply) {
		m_reply->disconnect(this);
		m_reply->abort();
		m_reply->deleteLater();
		m_reply = nullptr;
	}
}

void bbq_server_client::route_failed() {
	drop_connections();
	try_route(m_attempt + 1);
}

void bbq_server_client::finish() {
	m_deadline->stop();
	++m_generation;
	drop_connections();

	/* No route answered: every result stays failed. */
	if (m_attempt >= int(m_routes.size())) {
		m_results.assign(m_requests.size(), bbq_server_result());
	}

	m_busy = false;
	const std::vector<bbq_server_result> results = std::move(m_results);
	m_results.clear();
	m_requests.clear();
	emit finished(results);
}
