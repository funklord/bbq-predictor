package se.vibes.bbq_predictor;

import android.util.Log;

import org.qtproject.qt.android.bindings.QtService;

/*
 * The background fetch (project.md sec 17).
 *
 * A Qt service, which means this process loads the same native library
 * the activity does and runs the same main() -- with --android-service
 * in its arguments, which makes main() build a QCoreApplication and
 * fetch rather than a window.
 *
 * That sameness is the point. The fetch archives, verifies and queues
 * the correction because those live in the feed rather than in the
 * window, so a service that runs it gets all three without a second
 * implementation of any of them. The alternative was a Java fetcher,
 * which is the thing GraphWidget deliberately refused for the drawing
 * and would be worse here: the parsing, the band precedence and the
 * store schema would all have to be kept in step.
 */
public class FetchService extends QtService {

	@Override
	public void onCreate() {
		Log.i("bbq-predictor", "fetch service: created, loading Qt");
		super.onCreate();
	}
}
