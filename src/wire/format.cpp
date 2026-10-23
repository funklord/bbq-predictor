#include "wire/format.h"

#include <QDateTime>
#include <QTimeZone>

extern "C" {
#include "monocypher.h"
#include "monocypher-ed25519.h"
}

uint8_t bbq_wire_band_code(bbq_band band) {
	switch (band) {
	case bbq_band::observed:
		return 1;
	case bbq_band::current:
		return 2;
	case bbq_band::nowcast_fine:
		return 3;
	case bbq_band::nowcast:
		return 4;
	case bbq_band::extended:
		return 5;
	case bbq_band::hourly:
		return 6;
	case bbq_band::corrected:
		/* This program's opinion, not a provider's report (sec 12.5). */
		return 0;
	}
	return 0;
}

bool bbq_wire_station_valid(const QString &station) {
	if (station.isEmpty() || station.size() > 32) {
		return false;
	}
	for (QChar c : station) {
		const ushort u = c.unicode();
		if (!((u >= 'A' && u <= 'Z') || (u >= '0' && u <= '9'))) {
			return false;
		}
	}
	return true;
}

uint16_t bbq_wire_month_of(qint64 utc) {
	const QDate date = QDateTime::fromSecsSinceEpoch(utc, bbq_utc()).date();
	return uint16_t(date.year() * 12 + date.month() - 1);
}

qint64 bbq_wire_month_begin(uint16_t month) {
	const QDate first(month / 12, month % 12 + 1, 1);
	return QDateTime(first, QTime(0, 0), bbq_utc()).toSecsSinceEpoch();
}

qint64 bbq_wire_month_end(uint16_t month) {
	return bbq_wire_month_begin(uint16_t(month + 1));
}

QString bbq_wire_month_name(uint16_t month) {
	return QStringLiteral("%1-%2")
	        .arg(month / 12, 4, 10, QLatin1Char('0'))
	        .arg(month % 12 + 1, 2, 10, QLatin1Char('0'));
}

bool bbq_wire_band_from_code(uint8_t code, bbq_band *band) {
	for (bbq_band candidate : {bbq_band::observed, bbq_band::current, bbq_band::nowcast_fine,
	                           bbq_band::nowcast, bbq_band::extended, bbq_band::hourly}) {
		if (bbq_wire_band_code(candidate) == code) {
			*band = candidate;
			return true;
		}
	}
	return false;
}

QByteArray bbq_wire_digest(const QByteArray &file) {
	QByteArray digest(16, '\0');
	crypto_blake2b(reinterpret_cast<uint8_t *>(digest.data()), 16,
	               reinterpret_cast<const uint8_t *>(file.constData()),
	               size_t(file.size()));
	return digest;
}

bool bbq_signature_valid(const QByteArray &signature, const QByteArray &public_key,
                         const QByteArray &message) {
	if (signature.size() != 64 || public_key.size() != 32) {
		return false;
	}

	return crypto_ed25519_check(
	               reinterpret_cast<const uint8_t *>(signature.constData()),
	               reinterpret_cast<const uint8_t *>(public_key.constData()),
	               reinterpret_cast<const uint8_t *>(message.constData()),
	               size_t(message.size())) == 0;
}
