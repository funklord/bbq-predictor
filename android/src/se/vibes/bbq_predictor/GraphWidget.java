package se.vibes.bbq_predictor;

import android.app.PendingIntent;
import android.appwidget.AppWidgetManager;
import android.appwidget.AppWidgetProvider;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.widget.RemoteViews;

import java.io.File;

/*
 * The home-screen widget (project.md sec 16).
 *
 * It draws a picture the APPLICATION rendered, and does not render one
 * itself. The graph is Qt drawing a composite of four bands with the
 * project's own palette and interpolation; reimplementing any of that
 * in Java would be a second renderer to keep in step, and the two would
 * disagree the first time either changed.
 *
 * So the contract is a file. The application writes widget.png beside
 * its archive whenever a fetch settles, and tells this class it has.
 * The widget reads it.
 *
 * WHICH MEANS THE PICTURE CAN BE OLD, and that is the whole of what
 * this class has to get right. A stale forecast that looks current is
 * worse than no widget: somebody lights a fire on it. The file's own
 * modification time is the age -- not a timestamp drawn into the image,
 * which would freeze at render time and go on claiming to be fresh --
 * and past a threshold it is said, in words, over the picture.
 */
public class GraphWidget extends AppWidgetProvider {

	/* Where the application agrees to leave the picture. */
	private static final String PICTURE = "widget.png";

	/*
	 * Beyond this the age is stated on the widget.
	 *
	 * Twenty minutes because a station reports about every five and the
	 * application refreshes on that cadence, so three missed rounds is
	 * the first point at which the picture is telling somebody about a
	 * sky that has moved on.
	 */
	private static final long STALE_MS = 20L * 60L * 1000L;

	static File pictureFile(Context context) {
		/*
		 * Qt's AppDataLocation is files/ under the private data
		 * directory, which is what getFilesDir() returns. Named here
		 * once rather than at both ends: the C++ side derives the same
		 * path from QStandardPaths, and if those two ever disagree the
		 * widget shows the empty state rather than a wrong picture.
		 */
		return new File(context.getFilesDir(), PICTURE);
	}

	/*
	 * Called from C++ when a fresh picture has been written. Without
	 * this the widget would only change when Android felt like asking,
	 * which it does at most every thirty minutes and not at all while
	 * the device is idle.
	 */
	public static void refresh(Context context) {
		if (context == null) {
			return;
		}

		AppWidgetManager manager = AppWidgetManager.getInstance(context);
		ComponentName self = new ComponentName(context, GraphWidget.class);
		int[] ids = manager.getAppWidgetIds(self);

		if (ids == null || ids.length == 0) {
			return;
		}

		Intent intent = new Intent(context, GraphWidget.class);
		intent.setAction(AppWidgetManager.ACTION_APPWIDGET_UPDATE);
		intent.putExtra(AppWidgetManager.EXTRA_APPWIDGET_IDS, ids);
		context.sendBroadcast(intent);
	}

	@Override
	public void onUpdate(Context context, AppWidgetManager manager,
	                     int[] ids) {
		for (int id : ids) {
			draw(context, manager, id);
		}
	}

	private void draw(Context context, AppWidgetManager manager, int id) {
		RemoteViews views =
		        new RemoteViews(context.getPackageName(), R.layout.bbq_widget);

		/*
		 * Tapping opens the application. A widget that does nothing
		 * when touched reads as broken, and the thing somebody wants
		 * after glancing at a forecast is the forecast.
		 */
		Intent open = context.getPackageManager()
		        .getLaunchIntentForPackage(context.getPackageName());
		if (open != null) {
			views.setOnClickPendingIntent(
			        R.id.bbq_widget_graph,
			        PendingIntent.getActivity(context, 0, open,
			                PendingIntent.FLAG_UPDATE_CURRENT
			                        | PendingIntent.FLAG_IMMUTABLE));
		}

		File picture = pictureFile(context);
		Bitmap drawn = picture.isFile()
		        ? BitmapFactory.decodeFile(picture.getAbsolutePath())
		        : null;

		if (drawn == null) {
			/*
			 * No picture, or one that will not decode -- a half-written
			 * file being the obvious way to get the second. Say so
			 * rather than showing an empty frame, which reads as a
			 * broken widget instead of one waiting for its first run.
			 */
			views.setViewVisibility(R.id.bbq_widget_graph, android.view.View.GONE);
			views.setViewVisibility(R.id.bbq_widget_empty, android.view.View.VISIBLE);
			views.setViewVisibility(R.id.bbq_widget_age, android.view.View.GONE);
			manager.updateAppWidget(id, views);
			return;
		}

		views.setViewVisibility(R.id.bbq_widget_graph, android.view.View.VISIBLE);
		views.setViewVisibility(R.id.bbq_widget_empty, android.view.View.GONE);
		views.setImageViewBitmap(R.id.bbq_widget_graph, drawn);

		final long age = System.currentTimeMillis() - picture.lastModified();
		if (age > STALE_MS) {
			views.setViewVisibility(R.id.bbq_widget_age,
			                        android.view.View.VISIBLE);
			views.setTextViewText(R.id.bbq_widget_age, describe(age));
		} else {
			views.setViewVisibility(R.id.bbq_widget_age,
			                        android.view.View.GONE);
		}

		manager.updateAppWidget(id, views);
	}

	/*
	 * Said in the units somebody thinks in. "drawn 95 minutes ago" is a
	 * number to convert; "1 h 35 min old" is the answer.
	 */
	private static String describe(long age_ms) {
		final long minutes = age_ms / 60000L;

		if (minutes < 60) {
			return minutes + " min old";
		}

		final long hours = minutes / 60;
		if (hours < 48) {
			return hours + " h " + (minutes % 60) + " min old";
		}

		return (hours / 24) + " days old";
	}
}
