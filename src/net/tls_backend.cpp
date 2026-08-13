#include "net/tls_backend.h"

#include <QCoreApplication>
#include <QDir>
#include <QLibraryInfo>
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
	/*
	 * Where to look, and libraryPaths() alone is NOT enough.
	 *
	 * On Android every plugin is flattened into the application's own
	 * library directory, and that directory is not on libraryPaths() --
	 * which lists a plugins/ path that does not exist in the package.
	 * Scanning only the library paths therefore found nothing, gave up
	 * silently, and left TLS off with the plugin sitting in plain sight
	 * one directory away. The probe found it because the probe looked
	 * in both places; this now looks in the same places the probe does,
	 * which is the only reason it is known to be the right set.
	 */
	QStringList places = QCoreApplication::libraryPaths();
	places << QCoreApplication::applicationDirPath();
	places << QLibraryInfo::path(QLibraryInfo::PluginsPath);
	places.removeDuplicates();

	for (const QString &path : places) {
		const QDir directory(path);
		const QStringList pattern(QStringLiteral("*tls*openssl*"));
		const QStringList names = directory.entryList(pattern, QDir::Files);

		for (const QString &name : names) {
			QPluginLoader loader(directory.absoluteFilePath(name));

			/*
			 * instance(), not load().
			 *
			 * load() maps the shared object and verifies its metadata,
			 * and that is ALL it does -- it does not construct the
			 * plugin's root object. A QTlsBackend registers itself when
			 * it is constructed, so a loaded-but-never-instantiated
			 * plugin leaves the backend list exactly as it was.
			 *
			 * That is what the first version of this did, and it is why
			 * it appeared to work: load() returned true, the function
			 * returned happy, and TLS stayed off. The probe reported
			 * the plugin "loaded" in the same breath as
			 * "available backends cert-only" and the contradiction sat
			 * there unread, because cert-only is built into QtNetwork
			 * and needs no plugin -- so its presence proved nothing
			 * about discovery.
			 */
			if (loader.instance() != nullptr && QSslSocket::supportsSsl()) {
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
