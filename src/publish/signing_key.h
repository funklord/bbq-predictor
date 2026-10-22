#ifndef BBQ_SIGNING_KEY_H
#define BBQ_SIGNING_KEY_H

#include <QByteArray>
#include <QString>

#include <cstdint>

/*
 * The publisher's Ed25519 key (project.md sec 15.12.2).
 *
 * Every published file is signed with it and the applet carries the
 * public half, so the transport -- the daemon, or HTTPS on 443 -- is
 * never what is trusted. The daemon does not hold this key, so taking
 * the daemon over cannot forge a forecast.
 *
 * On disk the key is its 32-byte seed and nothing else. A key file any
 * other user can read is refused rather than used, the way ssh refuses
 * one: a readable key is one somebody else may already have.
 */
class bbq_signing_key {
public:
	bbq_signing_key() = default;
	~bbq_signing_key();

	bbq_signing_key(const bbq_signing_key &) = delete;
	bbq_signing_key &operator=(const bbq_signing_key &) = delete;

	/* Returns false and says why in `error`. */
	bool load(const QString &path, QString *error);

	/*
	 * Write a new seed to `path`, which must not exist, readable by its
	 * owner alone. Leaves this key holding it.
	 */
	bool generate(const QString &path, QString *error);

	/* From a seed already in memory; for tests. */
	void set_seed(const uint8_t seed[32]);

	bool is_valid() const { return m_valid; }

	/* 64 bytes of Ed25519 signature over `message`. */
	QByteArray sign(const QByteArray &message) const;

	/* The 32-byte public key the applet carries. */
	QByteArray public_key() const;

private:
	uint8_t m_secret[64] = {};
	uint8_t m_public[32] = {};
	bool m_valid = false;
};

#endif
