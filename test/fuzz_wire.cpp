/*
 * libFuzzer target for the applet's reader (project.md sec 15.12): the
 * parse_* half, which the signature keeps from hostile bytes in normal
 * use, fuzzed directly so that the parser is tested as though the key
 * had leaked. The first byte picks the kind; the rest is the body.
 * Built and run by `make fuzz-wire`, under ASan and UBSan, from a corpus
 * of real bodies the encoder writes (fuzz_wire_seed.cpp).
 *
 * This is also what holds the reader to the generated calls that are
 * safe at situ 56dcad1: a framed() or required() reached from here
 * would read out of bounds and stop the run.
 */
#include <QByteArray>
#include <QtGlobal>
#include <QString>

#include "wire/read.h"

/*
 * Quiet: qUncompress warns on every corrupt stream, which is most of what
 * a fuzzer sends, and printing it was most of the run's time.
 */
static void quiet(QtMsgType, const QMessageLogContext &, const QString &) {}

extern "C" int LLVMFuzzerInitialize(int *, char ***) {
	qInstallMessageHandler(quiet);
	return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
	if (size < 1) {
		return 0;
	}

	const QByteArray body(reinterpret_cast<const char *>(data + 1), int(size - 1));
	const QString station = QStringLiteral("ISTOCK877");
	QString why;

	switch (data[0] % 4) {
	case 0: {
		QStringList stations;
		bbq_wire_parse_stations(body, &stations, &why);
		break;
	}
	case 1: {
		std::vector<bbq_wire_index_entry> entries;
		bbq_wire_parse_index(body, station, &entries, &why);
		break;
	}
	case 2: {
		bbq_wire_month_rows rows;
		if (bbq_wire_parse_month(body, station, uint16_t(2026 * 12 + 8), &rows, &why)) {
			/* What a caller relies on, held as an invariant. */
			if (rows.provider.size() != rows.samples.size()) {
				__builtin_trap();
			}
			for (uint8_t provider : rows.provider) {
				if (provider >= rows.providers.size()) {
					__builtin_trap();
				}
			}
		}
		break;
	}
	case 3: {
		bbq_wire_head head;
		bbq_wire_parse_head(body, station, &head, &why);
		break;
	}
	}
	return 0;
}
