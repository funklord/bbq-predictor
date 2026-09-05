package se.vibes.bbq_predictor;

import android.app.job.JobInfo;
import android.app.job.JobParameters;
import android.app.job.JobScheduler;
import android.app.job.JobService;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.util.Log;

/*
 * What is allowed to start the fetch in the background (sec 17.1).
 *
 * Android 8 forbids starting a background service from the background:
 * measured on the device as "Background start not allowed: service ...
 * startFg?=false". The two ways round it are a foreground service, which
 * costs a permanent notification for a weather applet nobody asked to be
 * notified by, and this one -- the system starts the job, and a job that
 * is running may start a service.
 *
 * The job does nothing itself. It exists to be the thing Android is
 * willing to start, and hands straight over to the Qt service, which is
 * where the fetch that the applet and the systemd timer both use lives.
 */
public class FetchJobService extends JobService {

	private static final int JOB_ID = 1;

	/*
	 * Fifteen minutes is the floor the platform enforces for periodic
	 * work; asking for less silently gets fifteen anyway. A station
	 * reports about every five, so this is three rounds rather than one
	 * -- worse than the applet in the foreground and far better than
	 * nothing, which is what the archive got before.
	 */
	private static final long PERIOD_MS = 15L * 60L * 1000L;

	@Override
	public boolean onStartJob(JobParameters parameters) {
		/*
		 * Logged because a job that dispatches and does nothing looks
		 * exactly like one that never ran, and the first attempt at
		 * this was diagnosed from an empty logcat that turned out to be
		 * telling the truth.
		 */
		Log.i("bbq-predictor", "fetch job: starting the service");

		try {
			startService(new Intent(this, FetchService.class));
			Log.i("bbq-predictor", "fetch job: service start requested");
		} catch (Throwable failed) {
			Log.e("bbq-predictor", "fetch job: could not start the service",
			      failed);
		}

		/*
		 * false: the fetch runs in the service's own process and is not
		 * this thread's to wait for. Saying otherwise would hold a
		 * wakelock for work that has already been handed on.
		 */
		jobFinished(parameters, false);
		return false;
	}

	@Override
	public boolean onStopJob(JobParameters parameters) {
		/* Reschedule: a fetch dropped for want of a moment is a round
		 * missed rather than a failure. */
		return true;
	}

	/*
	 * Asked for every time the applet starts. Scheduling the same id
	 * again replaces the old one rather than stacking, so this is
	 * idempotent -- and it is the only thing that puts the job back
	 * after a reboot, because the job is deliberately not persisted:
	 * persistence needs RECEIVE_BOOT_COMPLETED, and asking for a
	 * permission to save a single launch is a poor trade.
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
