package se.vibes.bbq_predictor;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.content.pm.ServiceInfo;
import android.os.Build;
import android.util.Log;

import org.qtproject.qt.android.bindings.QtService;

/*
 * The background fetch (project.md sec 17).
 *
 * A Qt service, which means this process loads the same native library
 * the activity does and runs the same main() -- with --android-service
 * in its arguments, so main() builds a QCoreApplication and fetches
 * rather than a window. The fetch archives, verifies and queues the
 * correction because those live in the feed, so a service that runs it
 * gets all three without a second implementation of any of them.
 *
 * A FOREGROUND service, and that is not a preference. A background one
 * is killed before it can finish: loading Qt Core, Network and Sql into
 * a cold process took some thirty seconds on an SM-N960F and Android
 * ended it with "bg anr" (sec 17.3). A foreground service is given far
 * longer, and the price is a notification, which the copyright holder
 * chose over not having the feature.
 */
public class FetchService extends QtService {

	private static final String CHANNEL = "bbq-predictor-fetch";
	private static final int NOTIFICATION_ID = 2;

	@Override
	public void onCreate() {
		/*
		 * BEFORE super.onCreate(), which is where Qt loads.
		 *
		 * Android wants the notification promptly and Qt's startup is
		 * the slow part, so claiming foreground first is what buys the
		 * time the load needs. Doing it afterwards would be the same
		 * race that produced the ANR.
		 */
		goForeground();
		super.onCreate();
	}

	private void goForeground() {
		try {
			NotificationManager manager =
			        getSystemService(NotificationManager.class);

			if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O && manager != null) {
				/*
				 * IMPORTANCE_LOW: shown, and silent. A fetch every
				 * quarter of an hour that made a sound would be
				 * intolerable, and the notification exists because
				 * Android requires one rather than because anybody
				 * wants telling.
				 */
				NotificationChannel channel = new NotificationChannel(
				        CHANNEL, getString(R.string.bbq_fetch_channel),
				        NotificationManager.IMPORTANCE_LOW);
				channel.setShowBadge(false);
				manager.createNotificationChannel(channel);
			}

			Notification.Builder builder =
			        Build.VERSION.SDK_INT >= Build.VERSION_CODES.O
			                ? new Notification.Builder(this, CHANNEL)
			                : new Notification.Builder(this);

			Notification notice =
			        builder.setContentTitle(getString(R.string.bbq_fetch_title))
			                .setContentText(getString(R.string.bbq_fetch_text))
			                .setSmallIcon(android.R.drawable.stat_notify_sync)
			                .setOngoing(true)
			                .build();

			if (Build.VERSION.SDK_INT >= 29) {
				startForeground(NOTIFICATION_ID, notice,
				                ServiceInfo.FOREGROUND_SERVICE_TYPE_DATA_SYNC);
			} else {
				startForeground(NOTIFICATION_ID, notice);
			}
		} catch (Throwable failed) {
			/*
			 * Said rather than swallowed. Without foreground status the
			 * service will be killed mid-fetch, and a silent failure
			 * here would present as the archive simply not advancing.
			 */
			Log.e("bbq-predictor", "fetch service: could not go foreground",
			      failed);
		}
	}
}
