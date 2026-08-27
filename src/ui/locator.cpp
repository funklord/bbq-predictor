#include "ui/locator.h"

#include <QCoreApplication>
#include <QGeoPositionInfo>
#include <QGeoPositionInfoSource>
#include <QPermissions>
#include <QTimer>

bbq_locator::bbq_locator(QObject *parent) : QObject(parent) {}

void bbq_locator::answer_located(double latitude, double longitude) {
	if (m_answered) {
		return;
	}

	m_answered = true;
	emit located(latitude, longitude);
}

void bbq_locator::answer_unavailable(const QString &reason) {
	if (m_answered) {
		return;
	}

	m_answered = true;
	emit unavailable(reason);
}

void bbq_locator::locate_once(int timeout_ms) {
	m_answered = false;

	/*
	 * ASK FIRST (sec 13.3.2).
	 *
	 * A permission in the manifest only makes it requestable. Since
	 * Android 6 it must also be granted at run time, and nothing grants
	 * it but a dialog somebody answers. Without this the source was
	 * created, an update was requested, and the platform refused it
	 * without ever telling the user there was a question -- measured on
	 * an SM-N960F, where both location permissions read granted=false
	 * after a launch that had asked for a fix.
	 *
	 * Approximate rather than Precise, matching the manifest and the
	 * NonSatellitePositioningMethods below: three places that have to
	 * agree, and this is the one the reader sees.
	 */
	QLocationPermission wanted;
	wanted.setAccuracy(QLocationPermission::Approximate);

	/* Said once: refusing outright and refusing the dialog are the
	 * same answer to the reader, and should read the same. */
	const QString refused = tr("permission to use location was refused");

	/*
	 * Declared before the switch, not inside it: a lambda wrapped across
	 * lines inside a case is where the indentation rule and a case label
	 * disagree, and the argument is the same either way.
	 */
	const auto answered = [this, timeout_ms, refused](const QPermission &given) {
		if (given.status() != Qt::PermissionStatus::Granted) {
			answer_unavailable(refused);
			return;
		}

		start_source(timeout_ms);
	};

	switch (qApp->checkPermission(wanted)) {
	case Qt::PermissionStatus::Granted:
		break;

	case Qt::PermissionStatus::Denied:
		answer_unavailable(refused);
		return;

	case Qt::PermissionStatus::Undetermined:
		qApp->requestPermission(wanted, this, answered);
		return;
	}

	start_source(timeout_ms);
}

void bbq_locator::start_source(int timeout_ms) {
	if (m_source == nullptr) {
		m_source = QGeoPositionInfoSource::createDefaultSource(this);
	}

	/*
	 * No source at all. Ordinary on a desktop, and not an error: it
	 * means the machine cannot answer this question, which is exactly
	 * what the search box is for.
	 */
	if (m_source == nullptr) {
		answer_unavailable(tr("this machine has no positioning source"));
		return;
	}

	/*
	 * Coarse is asked for by name rather than taken as whatever the
	 * platform defaults to. Station distances are hundreds of metres at
	 * best, so a satellite fix would cost time and battery to answer a
	 * question a cell-tower fix answers.
	 */
	m_source->setPreferredPositioningMethods(
	        QGeoPositionInfoSource::NonSatellitePositioningMethods);

	connect(m_source, &QGeoPositionInfoSource::positionUpdated, this,
	        [this](const QGeoPositionInfo &info) {
		if (!info.isValid()) {
			return;
		}

		answer_located(info.coordinate().latitude(),
		               info.coordinate().longitude());
	});

	connect(m_source, &QGeoPositionInfoSource::errorOccurred, this,
	        [this](QGeoPositionInfoSource::Error problem) {
		/*
		 * Named rather than numbered. "Location is switched off" and
		 * "you said no" want different responses from the reader, and a
		 * code tells them neither.
		 */
		switch (problem) {
		case QGeoPositionInfoSource::AccessError:
			answer_unavailable(tr("permission to use location was refused"));
			return;
		case QGeoPositionInfoSource::ClosedError:
			answer_unavailable(tr("location services are switched off"));
			return;
		case QGeoPositionInfoSource::NoError:
			return;
		default:
			answer_unavailable(tr("the positioning source failed"));
			return;
		}
	});

	/*
	 * A deadline of our own, because a source that simply never answers
	 * is the ordinary indoor case and emits no error while it waits.
	 * Without this the station list would sit empty with nothing said,
	 * which reads as a broken program rather than as a phone that
	 * cannot see the sky.
	 */
	QTimer::singleShot(timeout_ms, this, [this]() {
		answer_unavailable(tr("no position within the time allowed"));
	});

	m_source->requestUpdate(timeout_ms);
}
