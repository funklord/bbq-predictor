#ifndef BBQ_SERVER_CLIENT_H
#define BBQ_SERVER_CLIENT_H

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QUrl>

#include <cstdint>
#include <vector>

#include "wire/format.h"

class QNetworkAccessManager;
class QNetworkReply;
class QTcpSocket;
class QTimer;

/*
 * Fetch published files from vibes.se (project.md sec 15.12.2).
 *
 * Three routes, tried in turn, starting from the one that last worked:
 *
 *   1. the daemon's protocol on 7227;
 *   2. the daemon's protocol on 443, for networks that block the rest;
 *   3. HTTPS GETs on 443, for when a web server holds that port.
 *
 * The transport is not trusted on any of them -- every file is signed
 * and checked by src/wire/read -- so a route is only a way to the bytes.
 * A route is abandoned for the next on anything but a well-formed
 * answer: a refused or silent connection, a malformed header, a body
 * over its cap. "Not served" is a well-formed answer, not a failure.
 *
 * Every exchange has a deadline, so a round always finishes.
 */

struct bbq_server_request {
	bbq_wire_kind kind = bbq_wire_kind::stations;
	QString station;          /* empty for the manifest */
	uint16_t month = 0;       /* month files only */
	QByteArray have;          /* the signature of the copy held, or empty */
};

enum class bbq_server_status {
	ok,           /* `file` holds the file */
	unchanged,    /* the copy held is current */
	not_served,   /* the server does not have it */
	failed,       /* no route answered */
};

struct bbq_server_result {
	bbq_server_status status = bbq_server_status::failed;
	QByteArray file;
};

struct bbq_server_route {
	enum class kind { daemon, https };
	kind type = kind::daemon;
	QString host;
	quint16 port = 0;
	QUrl base;                /* https only: where the tree is served */
};

/* vibes.se:7227, vibes.se:443, then https://vibes.se/bbq-predictor/. */
std::vector<bbq_server_route> bbq_server_default_routes();

/* The 48-byte request the daemon takes (src/serve/serve_core.h). */
QByteArray bbq_server_encode_request(const bbq_server_request &request);

/* The path a web server serves a request's file at, under the base. */
QString bbq_server_path(const bbq_server_request &request);

class bbq_server_client : public QObject {
	Q_OBJECT

public:
	explicit bbq_server_client(QNetworkAccessManager *network,
	                           QObject *parent = nullptr);
	~bbq_server_client() override;

	void set_routes(std::vector<bbq_server_route> routes);

	/* Which route to try first: the one that last answered. */
	int preferred_route() const { return m_preferred; }
	void set_preferred_route(int index);

	/* Per exchange, for the suite; 20 s otherwise. */
	void set_timeout_ms(int timeout_ms) { m_timeout_ms = timeout_ms; }

	/*
	 * Fetch every request, one result per request in the same order,
	 * delivered by finished(). One batch at a time; a second call while
	 * one runs is refused with every result failed.
	 */
	void fetch(std::vector<bbq_server_request> requests);
	bool is_busy() const { return m_busy; }

signals:
	void finished(const std::vector<bbq_server_result> &results);

private:
	void try_route(int attempt);
	void try_daemon(const bbq_server_route &route);
	void try_https(const bbq_server_route &route, size_t next);
	void route_failed();
	void route_answered();
	void drop_connections();
	void parse_daemon_replies();
	void finish();

	QNetworkAccessManager *m_network = nullptr;
	std::vector<bbq_server_route> m_routes;
	int m_preferred = 0;
	int m_timeout_ms = 20000;

	bool m_busy = false;
	int m_attempt = 0;
	std::vector<bbq_server_request> m_requests;
	std::vector<bbq_server_result> m_results;

	QTcpSocket *m_socket = nullptr;
	QNetworkReply *m_reply = nullptr;
	QByteArray m_received;
	size_t m_answered = 0;
	QTimer *m_deadline = nullptr;
	quint64 m_generation = 0;
};

#endif
