package se.vibes.bbq_predictor;

import android.app.job.JobInfo;
import android.app.job.JobParameters;
import android.app.job.JobScheduler;
import android.app.job.JobService;
import android.content.ComponentName;
import android.content.Context;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;

import org.qtproject.qt.android.BbqJobLoader;

/*
 * The background fetch (project.md sec 17), run by the job itself.
 *
 * The job loads the same native library the activity does and runs the
 * same main() -- with --android-service in its arguments, from the
 * meta-data in the manifest, so main() fetches and draws the widget's
 * picture rather than opening a window. The fetch archives, verifies and
 * queues the correction because those live in the feed, so the job gets
 * all three without a second implementation of any of them.
 *
 * THE JOB DOES THE WORK, rather than starting a service that does. Both
 * kinds of service start were measured refused from a job with the app
 * closed, on the API 33 emulator 2026-11-02: Android 8's "Background
 * start not allowed ... startFg?=false" for a plain one, and Android
 * 12's ForegroundServiceStartNotAllowedException for a foreground one.
 * A job is given up to ten minutes and needs no notification, which a
 * foreground service did (sec 17.4).
 *
 * IN ITS OWN PROCESS, ":fetch" in the manifest. Qt permits one
 * application per process and the activity's already has one; the
 * process is ended once the job is done, because Qt cannot be started in
 * it a second time.
 */
public class FetchJobService extends JobService {

	private static final String TAG = "bbq-predictor";
	private static final int JOB_ID = 1;

	/*
	 * Fifteen minutes is the floor the platform enforces for periodic
	 * work; asking for less silently gets fifteen anyway. A station
	 * reports about every five, so this is three rounds rather than one
	 * -- worse than the applet in the foreground and far better than
	 * nothing, which is what the widget got before.
	 */
	private static final long PERIOD_MS = 15L * 60L * 1000L;

	/* The job in progress, for fetchDone(), which main() calls. */
	private static FetchJobService running;
	private static JobParameters running_job;

	private boolean qt_started;

	@Override
	public boolean onStartJob(JobParameters parameters) {
		/*
		 * Logged because a job that dispatches and does nothing looks
		 * exactly like one that never ran.
		 */
		Log.i(TAG, "fetch job: starting");

		running = this;
		running_job = parameters;

		/*
		 * true: the fetch runs on Qt's thread and ends in fetchDone().
		 * Saying false would let Android end the process mid-fetch.
		 */
		qt_started = BbqJobLoader.start(this);
		if (!qt_started) {
			Log.e(TAG, "fetch job: Qt did not start");
			running = null;
			running_job = null;
			return false;
		}

		return true;
	}

	@Override
	public boolean onStopJob(JobParameters parameters) {
		/*
		 * Stopped by the system, out of time or network. The next
		 * period is a quarter of an hour away, so nothing is retried
		 * sooner.
		 */
		Log.w(TAG, "fetch job: stopped by the system");
		return false;
	}

	@Override
	public void onDestroy() {
		super.onDestroy();

		/*
		 * Qt cannot be started twice in one process, so the process goes
		 * with it, as QtServiceBase's own onDestroy does. Only when Qt
		 * was started here.
		 */
		if (qt_started) {
			BbqJobLoader.stop();
			System.exit(0);
		}
	}

	/*
	 * Called by main() on Qt's thread once the fetch and the widget's
	 * picture are done. Finished on the main thread, where the job
	 * belongs.
	 */
	public static void fetchDone() {
		new Handler(Looper.getMainLooper()).post(() -> {
			if (running != null && running_job != null) {
				Log.i(TAG, "fetch job: done");
				running.jobFinished(running_job, false);
			}
			running = null;
			running_job = null;
		});
	}

	/*
	 * Asked for every time the applet starts, from the activity's
	 * process. Scheduling the same id again replaces the old one rather
	 * than stacking, so this is idempotent -- and it is the only thing
	 * that puts the job back after a reboot, because the job is
	 * deliberately not persisted: persistence needs
	 * RECEIVE_BOOT_COMPLETED, and asking for a permission to save a
	 * single launch is a poor trade.
	 */
	public static void schedule(Context context) {
		if (context == null) {
			return;
		}

		JobScheduler scheduler =
		        (JobScheduler) context.getSystemService(Context.JOB_SCHEDULER_SERVICE);
		if (scheduler == null) {
			return;
		}

		JobInfo job = new JobInfo.Builder(
		                      JOB_ID,
		                      new ComponentName(context, FetchJobService.class))
		                      .setRequiredNetworkType(JobInfo.NETWORK_TYPE_ANY)
		                      .setPeriodic(PERIOD_MS)
		                      .build();

		scheduler.schedule(job);
	}
}
