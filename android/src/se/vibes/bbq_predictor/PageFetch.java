package se.vibes.bbq_predictor;

import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.net.HttpURLConnection;
import java.net.URL;

/*
 * Fetch one page with Android's own HTTP client (project.md sec 2.6.1.2).
 *
 * This exists for a single reason: Weather Underground answers this
 * program's Qt requests with 404 while the phone's own curl gets 200 from
 * the same wifi, seconds apart. What differs is the client, and on
 * Android the largest difference is that this project ships its own
 * OpenSSL rather than using the platform's. HttpURLConnection uses the
 * platform stack, so a request through it looks like every other app on
 * the device.
 *
 * Deliberately tiny. It is not a networking layer -- it is one GET, used
 * by one caller, for the one URL Qt is being refused on. Everything else
 * this program fetches still goes through Qt.
 *
 * Returns the body, or null on any failure. The caller cannot act on the
 * difference between a refusal and a timeout, and a null keeps the
 * retry decision in one place rather than two.
 */
public class PageFetch {
	public static String get(String address, String agent, int timeoutMs) {
		HttpURLConnection link = null;

		try {
			link = (HttpURLConnection) new URL(address).openConnection();
			link.setRequestProperty("User-Agent", agent);
			link.setConnectTimeout(timeoutMs);
			link.setReadTimeout(timeoutMs);
			link.setInstanceFollowRedirects(true);

			/*
			 * An error's body as well, when the server sent one: WU's 404
			 * page carries the site's API_KEY like every page it serves
			 * (project.md sec 2.6.1.5). No body is null, as a refusal is.
			 */
			InputStream body = link.getResponseCode() == HttpURLConnection.HTTP_OK
			                           ? link.getInputStream()
			                           : link.getErrorStream();
			if (body == null) {
				return null;
			}
			ByteArrayOutputStream held = new ByteArrayOutputStream();
			byte[] chunk = new byte[16384];
			int read;

			while ((read = body.read(chunk)) != -1) {
				held.write(chunk, 0, read);
			}

			body.close();
			return held.toString("UTF-8");
		} catch (Exception failed) {
			return null;
		} finally {
			if (link != null) {
				link.disconnect();
			}
		}
	}
}
