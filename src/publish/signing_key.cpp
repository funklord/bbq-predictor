#include "publish/signing_key.h"

#include <QFile>

#include <fcntl.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

extern "C" {
#include "monocypher.h"
#include "monocypher-ed25519.h"
}

bbq_signing_key::~bbq_signing_key() {
	crypto_wipe(m_secret, sizeof m_secret);
}

void bbq_signing_key::set_seed(const uint8_t seed[32]) {
	/* crypto_ed25519_key_pair wipes the seed it is given, so give it a copy. */
	uint8_t copy[32];
	std::memcpy(copy, seed, sizeof copy);
	crypto_ed25519_key_pair(m_secret, m_public, copy);
	crypto_wipe(copy, sizeof copy);
	m_valid = true;
}

bool bbq_signing_key::load(const QString &path, QString *error) {
	const QByteArray name = QFile::encodeName(path);
	const int fd = ::open(name.constData(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
	if (fd < 0) {
		*error = QStringLiteral("cannot open %1: %2")
		                 .arg(path, QString::fromLocal8Bit(std::strerror(errno)));
		return false;
	}

	struct stat info;
	if (::fstat(fd, &info) != 0 || !S_ISREG(info.st_mode)) {
		::close(fd);
		*error = QStringLiteral("%1 is not a regular file").arg(path);
		return false;
	}
	if (info.st_mode & (S_IRWXG | S_IRWXO)) {
		::close(fd);
		*error = QStringLiteral("%1 is readable by others (mode %2); "
		                        "chmod 600 it, and treat the key as exposed")
		                 .arg(path)
		                 .arg(info.st_mode & 07777, 4, 8, QLatin1Char('0'));
		return false;
	}
	if (info.st_size != 32) {
		::close(fd);
		*error = QStringLiteral("%1 holds %2 bytes, and a key is 32")
		                 .arg(path)
		                 .arg(qint64(info.st_size));
		return false;
	}

	uint8_t seed[32];
	const ssize_t got = ::read(fd, seed, sizeof seed);
	::close(fd);
	if (got != ssize_t(sizeof seed)) {
		crypto_wipe(seed, sizeof seed);
		*error = QStringLiteral("could not read %1").arg(path);
		return false;
	}

	set_seed(seed);
	crypto_wipe(seed, sizeof seed);
	return true;
}

bool bbq_signing_key::generate(const QString &path, QString *error) {
	uint8_t seed[32];
	if (::getrandom(seed, sizeof seed, 0) != ssize_t(sizeof seed)) {
		*error = QStringLiteral("no randomness from the kernel");
		return false;
	}

	/* O_EXCL: a key is never overwritten, since losing one strands every client. */
	const QByteArray name = QFile::encodeName(path);
	const int fd = ::open(name.constData(),
	                      O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
	if (fd < 0) {
		crypto_wipe(seed, sizeof seed);
		*error = QStringLiteral("cannot create %1: %2")
		                 .arg(path, QString::fromLocal8Bit(std::strerror(errno)));
		return false;
	}

	const bool written = ::write(fd, seed, sizeof seed) == ssize_t(sizeof seed) &&
	                     ::fsync(fd) == 0;
	::close(fd);
	if (!written) {
		crypto_wipe(seed, sizeof seed);
		::unlink(name.constData());
		*error = QStringLiteral("could not write %1").arg(path);
		return false;
	}

	set_seed(seed);
	crypto_wipe(seed, sizeof seed);
	return true;
}

QByteArray bbq_signing_key::sign(const QByteArray &message) const {
	QByteArray signature(64, '\0');
	crypto_ed25519_sign(reinterpret_cast<uint8_t *>(signature.data()), m_secret,
	                    reinterpret_cast<const uint8_t *>(message.constData()),
	                    size_t(message.size()));
	return signature;
}

QByteArray bbq_signing_key::public_key() const {
	return QByteArray(reinterpret_cast<const char *>(m_public), sizeof m_public);
}

bool bbq_signature_valid(const QByteArray &signature, const QByteArray &public_key,
                         const QByteArray &message) {
	if (signature.size() != 64 || public_key.size() != 32) {
		return false;
	}

	return crypto_ed25519_check(
	               reinterpret_cast<const uint8_t *>(signature.constData()),
	               reinterpret_cast<const uint8_t *>(public_key.constData()),
	               reinterpret_cast<const uint8_t *>(message.constData()),
	               size_t(message.size())) == 0;
}
