#include "net/probe.h"

#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QStandardPaths>
#include <QLibraryInfo>
#include <QLoggingCategory>
#include <QNetworkAccessManager>
#include <QPluginLoader>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSslSocket>
#include <QStringList>
#include <QTimer>
#include <QUrl>

namespace {

/*
 * What to try, and each one is here to distinguish a specific pair of
 * explanations rather than to be thorough.
 */
struct probe_target {
	const char *what;
	const char *url;
};

const probe_target targets[] = {
	/*
	 * Plain HTTP first. If this succeeds and every HTTPS fails, the
	 * network is fine and the problem is entirely TLS -- which is the
	 * single most useful thing to know, and the one a failing HTTPS
	 * request alone cannot tell you.
	 */
	{ "http (no TLS at all)", "http://example.com/" },

	/* A boring HTTPS host, to separate "TLS is broken" from "that
	 * provider is broken". */
	{ "https (a plain host)", "https://example.com/" },

	/* The three the applet actually depends on. */
	{ "wunderground page", "https://www.wunderground.com/forecast" },
	{ "met.no api", "https://api.met.no/weatherapi/nowcast/2.0/complete?lat=59.33&lon=18.07" },
	{ "open-meteo api", "https://api.open-meteo.com/v1/forecast?latitude=59.33&longitude=18.07&hourly=temperature_2m" },
	{ "weather.com api", "https://api.weather.com/v2/pws/observations/current?stationId=ISTOCK822&format=json&units=m&apiKey=0" },
};

/*
 * Where the answer is written, as well as logged.
 *
 * A file, because the log could not be relied on: on Android the early
 * part of startup happens before Qt installs its logcat handler, so
 * anything said there goes to a stderr nobody reads -- which is exactly
 * how this probe reported nothing at all on its first run, on the one
 * platform it was written for.
 */
QFile *report_file = nullptr;
QtMessageHandler previous_handler = nullptr;

/*
 * Qt's own diagnostics, captured into the same report.
 *
 * The reason the OpenSSL backend refuses to load is something only Qt
 * knows, and it says so under a debug category that is off by default
 * and goes to a log that early Android startup does not reach. Both
 * problems are fixed here: the category is turned on, and every message
 * Qt emits is written to the file beside our own lines.
 */
void capture(QtMsgType type, const QMessageLogContext &at, const QString &text) {
	const bool writable = report_file != nullptr && report_file->isOpen();
	const bool ours = text.startsWith(QStringLiteral("probe:"));

	if (writable && !ours) {
		report_file->write("qt: ");
		report_file->write(text.toUtf8());
		report_file->write("\n");
		report_file->flush();
	}

	if (previous_handler != nullptr) {
		previous_handler(type, at, text);
	}
}

void say(const QString &line) {
	/*
	 * qWarning rather than qInfo: info is filtered out by default in
	 * some configurations, and a diagnostic that can be silenced by a
	 * logging rule is one that reports nothing on the machine where it
	 * was needed.
	 */
	qWarning("%s", qUtf8Printable(line));

	if (report_file != nullptr && report_file->isOpen()) {
		report_file->write(line.toUtf8());
		report_file->write("\n");
		report_file->flush();
	}
}

} // namespace

int bbq_net_probe(int timeout_s) {
	const QString directory =
	        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
	QDir().mkpath(directory);

	QFile report(directory + QStringLiteral("/probe.txt"));
	if (report.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
		report_file = &report;
	}

	previous_handler = qInstallMessageHandler(capture);

	/*
	 * Turned on deliberately. This is the category that names the file
	 * Qt tried to load and what the loader said about it, which is the
	 * one fact three rounds of inference never established.
	 */
	QLoggingCategory::setFilterRules(QStringLiteral(
	        "qt.tlsbackend.ossl.debug=true\n"
	        "qt.network.ssl.debug=true"));

	say(QStringLiteral("probe: writing to %1").arg(report.fileName()));
	say(QStringLiteral("probe: --- what this build has ---"));

	/*
	 * The answer that three rounds of inference failed to establish.
	 * supportsSsl() actually attempts the load, so this is not a
	 * configuration reading -- it is the same question the applet asks
	 * when it tries to fetch, asked where the answer is visible.
	 */
	say(QStringLiteral("probe: supportsSsl        %1")
	            .arg(QSslSocket::supportsSsl() ? QStringLiteral("yes")
	                                           : QStringLiteral("NO")));
	say(QStringLiteral("probe: built against      %1")
	            .arg(QSslSocket::sslLibraryBuildVersionString()));
	say(QStringLiteral("probe: found at runtime   %1")
	            .arg(QSslSocket::sslLibraryVersionString()));
	say(QStringLiteral("probe: active backend     %1")
	            .arg(QSslSocket::activeBackend()));
	say(QStringLiteral("probe: available backends %1")
	            .arg(QSslSocket::availableBackends().join(QStringLiteral(", "))));

	/*
	 * Where Qt looked. On Android the TLS backend scans directories for
	 * files matching libcrypto.* and libssl.*, so knowing which
	 * directories are on that list -- and what is in them -- is the
	 * difference between "the library is missing" and "the library is
	 * present and Qt is looking somewhere else".
	 */
	say(QStringLiteral("probe: --- where a library could be found ---"));

	QStringList places = QCoreApplication::libraryPaths();
	places << QCoreApplication::applicationDirPath();
	places << QLibraryInfo::path(QLibraryInfo::PluginsPath);
	places.removeDuplicates();

	for (const QString &place : places) {
		const QDir directory(place);
		/*
		 * "libcrypto*", not "libcrypto.*".
		 *
		 * The dotted form was copied from the glob inside Qt's own
		 * fallback search, and it does not match libcrypto_3.so -- the
		 * name Qt actually dlopens on Android and the name this project
		 * now ships. So the probe reported "(no ssl libraries)" for the
		 * directory holding both of them, in the same run where TLS was
		 * working perfectly. A diagnostic that reports an absence next
		 * to a success is worse than one that says nothing: it invites
		 * the reader to explain a fault that is not there.
		 */
		QStringList wanted;
		wanted << QStringLiteral("libcrypto*") << QStringLiteral("libssl*");

		const QStringList found = directory.entryList(wanted, QDir::Files);

		say(QStringLiteral("probe:   %1  %2")
		            .arg(place,
		                 found.isEmpty() ? QStringLiteral("(no ssl libraries)")
		                                 : found.join(QStringLiteral(" "))));
	}

	/*
	 * Load the OpenSSL backend plugin by hand and report what the loader
	 * says.
	 *
	 * Qt reported availableBackends as cert-only with the plugin file
	 * sitting beside the cert-only one that DID load, and emitted no
	 * diagnostic at all -- so the failure happens before anything with a
	 * logging category runs. QPluginLoader::errorString() is the only
	 * place the reason is written down.
	 */
	say(QStringLiteral("probe: --- loading the tls plugins by hand ---"));

	for (const QString &place : places) {
		const QDir directory(place);
		const QStringList pattern(QStringLiteral("*tls*backend*"));
		const QStringList plugins = directory.entryList(pattern, QDir::Files);

		for (const QString &name : plugins) {
			QPluginLoader loader(directory.absoluteFilePath(name));
			const bool loaded = loader.load();

			/*
			 * Both, separately, because they are different questions and
			 * reporting only the first is what made this probe agree with
			 * a loader that did not work. load() maps the file;
			 * instance() constructs the plugin, which is what actually
			 * registers a TLS backend. A plugin can pass the first and
			 * fail the second, and that gap is the whole bug.
			 */
			const bool built = loaded && loader.instance() != nullptr;

			say(QStringLiteral("probe:   %1  load %2, instance %3%4")
			            .arg(name,
			                 loaded ? QStringLiteral("ok") : QStringLiteral("NO"),
			                 built ? QStringLiteral("ok") : QStringLiteral("NO"),
			                 loaded && !built
			                         ? QStringLiteral(" -- %1").arg(loader.errorString())
			                         : QString()));
		}
	}

	/*
	 * Asked again, after the plugins have been constructed rather than
	 * merely mapped. The difference between this line and the one above
	 * is the measurement that matters.
	 */
	say(QStringLiteral("probe: after instancing:"));
	say(QStringLiteral("probe:   supportsSsl        %1")
	            .arg(QSslSocket::supportsSsl() ? QStringLiteral("yes")
	                                           : QStringLiteral("NO")));
	say(QStringLiteral("probe:   available backends %1")
	            .arg(QSslSocket::availableBackends().join(QStringLiteral(", "))));
	say(QStringLiteral("probe:   found at runtime   %1")
	            .arg(QSslSocket::sslLibraryVersionString()));

	{
	}

	say(QStringLiteral("probe: --- what it can reach ---"));

	QNetworkAccessManager net;
	net.setTransferTimeout(timeout_s * 1000);

	int failures = 0;

	for (const probe_target &target : targets) {
		QNetworkRequest request(QUrl(QString::fromLatin1(target.url)));
		request.setHeader(QNetworkRequest::UserAgentHeader,
		                  QStringLiteral("bbq-predictor/probe"));
		request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
		                     QNetworkRequest::NoLessSafeRedirectPolicy);

		QNetworkReply *reply = net.get(request);

		/*
		 * Each request is waited for on its own loop, with the manager's
		 * transfer timeout as the bound. A probe that hangs is a probe
		 * nobody gets an answer from.
		 */
		QEventLoop loop;
		QObject::connect(reply, &QNetworkReply::finished, &loop,
		                 &QEventLoop::quit);
		QTimer::singleShot((timeout_s + 5) * 1000, &loop, &QEventLoop::quit);
		loop.exec();

		const int status =
		        reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

		if (reply->error() == QNetworkReply::NoError) {
			say(QStringLiteral("probe: OK   %1  HTTP %2, %3 bytes")
			            .arg(QString::fromLatin1(target.what))
			            .arg(status)
			            .arg(reply->readAll().size()));
		} else {
			++failures;

			/*
			 * The error NUMBER as well as the string. The strings are
			 * translated and paraphrased between Qt versions; the enum
			 * value is what can be looked up without ambiguity.
			 */
			say(QStringLiteral("probe: FAIL %1  error %2 (%3)%4")
			            .arg(QString::fromLatin1(target.what))
			            .arg(static_cast<int>(reply->error()))
			            .arg(reply->errorString())
			            .arg(status > 0 ? QStringLiteral(", HTTP %1").arg(status)
			                            : QString()));
		}

		reply->deleteLater();
	}

	say(QStringLiteral("probe: %1 of %2 failed")
	            .arg(failures)
	            .arg(static_cast<int>(sizeof(targets) / sizeof(targets[0]))));

	qInstallMessageHandler(previous_handler);
	report_file = nullptr;
	report.close();

	return failures == 0 ? 0 : 1;
}
