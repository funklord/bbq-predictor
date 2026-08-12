#ifndef BBQ_NET_PROBE_H
#define BBQ_NET_PROBE_H

/*
 * Ask the machine what it can actually reach (project.md sec 11.6).
 *
 * Written for the Android build, where TLS failed and three rounds of
 * inference about WHY produced three wrong answers. Guessing at a
 * remote failure from a one-line warning is how that happens; this
 * makes the device answer instead.
 *
 * It reports what Qt's TLS stack resolved to -- whether SSL is
 * supported at all, and which library version was found at build time
 * and at run time -- and then fetches a list of URLs, saying for each
 * one exactly how it ended. The two halves matter together: "no TLS"
 * and "TLS fine, host unreachable" produce the same blank graph and
 * want completely different fixes.
 *
 * Output goes through the logging category rather than stdout, because
 * on Android there is no stdout to read and logcat is the only way the
 * answer gets off the device.
 */
int bbq_net_probe(int timeout_s);

#endif
