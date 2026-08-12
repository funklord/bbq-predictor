#include "net/tls_backend.h"

#include <QCoreApplication>
#include <QDir>
#include <QPluginLoader>
#include <QSslSocket>
#include <QStringList>

void bbq_ensure_tls_backend() {
	/*
	 * Nothing to do where TLS already works, which is the ordinary case
	 * everywhere except Android. Asking costs a plugin scan that Qt has
	 * done already, and answers the question this function exists for
	 * rather than a proxy for it -- supportsSsl() actually attempts the
	 * load rather than reporting a configuration.
	 */
	if (QSslSocket::supportsSsl()) {
		return;
	}

	/*
	 * Find the OpenSSL backend and load it by hand.
	 *
	 * The plugin is present and loadable -- QPluginLoader returns true
	 * for it -- Qt simply never gets round to it on its own. So this is
	 * not a repair of a broken plugin but a nudge for a discovery step
	 * that does not happen, and it is deliberately narrow: one pattern,
	 * one load, and it stops as soon as TLS reports itself working.
	 */
	for (const QString &path : QCoreApplication::libraryPaths()) {
		const QDir directory(path);
		const QStringList pattern(QStringLiteral("*tls*openssl*"));
		const QStringList names = directory.entryList(pattern, QDir::Files);

		for (const QString &name : names) {
			QPluginLoader loader(directory.absoluteFilePath(name));

			if (loader.load() && QSslSocket::supportsSsl()) {
				return;
			}
		}
	}

	/*
	 * Said out loud rather than left as a silent absence. Without TLS
	 * this program can fetch nothing, and a user staring at an empty
	 * graph deserves better than an error attached to each band in turn.
	 */
	qWarning("bbq-predictor: no TLS backend could be loaded; "
	         "every provider is HTTPS, so nothing can be fetched.");
}
