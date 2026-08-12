#ifndef BBQ_NET_TLS_BACKEND_H
#define BBQ_NET_TLS_BACKEND_H

/*
 * Make sure a TLS backend is actually loaded (project.md sec 11.6).
 *
 * On Android Qt's automatic plugin discovery does not load the OpenSSL
 * TLS backend, even with the plugin sitting in the same directory as
 * the cert-only backend that it does load. QSslSocket then reports
 * supportsSsl() false and every HTTPS request fails with "TLS
 * initialization failed" -- which, for this program, means no data at
 * all, because every provider it reads is HTTPS.
 *
 * Loading the plugin explicitly fixes it: the backend registers, finds
 * the bundled libssl and libcrypto, and every host answers. Measured
 * both ways on the device before this existed (sec 11.6.1).
 *
 * A no-op wherever TLS already works, which is every desktop, so this
 * is safe to call unconditionally and needs no platform test at the
 * call site.
 */
void bbq_ensure_tls_backend();

#endif
