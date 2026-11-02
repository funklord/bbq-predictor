package org.qtproject.qt.android;

import android.app.Service;
import android.util.Log;

/*
 * Qt, started inside a job rather than a QtService (project.md sec 17.7).
 *
 * IN QT'S PACKAGE BECAUSE WHAT IT CALLS IS PACKAGE-PRIVATE. The service
 * loader, QtNative's start and QtLoader's result are not public, and
 * QtServiceBase -- the only thing that calls them -- extends Service, so
 * a JobService cannot inherit it. This is QtServiceBase's onCreate and
 * onDestroy, line for line, from Qt 6.12's own source. A Qt release that
 * renames any of it fails here at compile time, which is the loud end of
 * depending on another project's internals.
 *
 * Why a job has to host it at all: Android 12 refuses a foreground
 * service started from the background, and Android 8 a background one,
 * both measured from a periodic job with the app closed. The job itself
 * is the one thing the platform will run, for up to ten minutes and with
 * no notification.
 */
public final class BbqJobLoader {

	private BbqJobLoader() {
	}

	/*
	 * Loads Qt and starts main() on Qt's own thread. True when main() is
	 * on its way; false when Qt could not be loaded, or is already
	 * running in this process, and the caller has no fetch to wait for.
	 */
	public static boolean start(Service service) {
		if (QtNative.getStateDetails().isStarted) {
			Log.w(QtNative.QtTAG, "BbqJobLoader: Qt is already running here");
			return false;
		}

		QtNative.setService(service);

		try {
			QtServiceLoader loader = QtServiceLoader.getServiceLoader(service);
			QtLoader.LoadingResult result = loader.loadQtLibraries();

			if (result == QtLoader.LoadingResult.Failed) {
				Log.w(QtNative.QtTAG, "BbqJobLoader: failed to load Qt libraries");
				return false;
			}

			if (result == QtLoader.LoadingResult.Succeeded) {
				QtNative.startApplication(loader.getApplicationParameters(),
				                          loader.getMainLibraryPath());
				QtNative.setApplicationState(
				        QtNative.ApplicationState.ApplicationHidden);
				return true;
			}
		} catch (IllegalArgumentException failed) {
			Log.w(QtNative.QtTAG, "BbqJobLoader: " + failed.getMessage());
		}

		return false;
	}

	/*
	 * Qt's teardown. The process cannot start Qt a second time, so the
	 * caller exits it afterwards, as QtServiceBase does.
	 */
	public static void stop() {
		QtNative.terminateQtNativeApplication();
		QtNative.setService(null);
	}
}
