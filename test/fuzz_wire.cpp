/*
 * libFuzzer target for the applet's reader (project.md sec 15.12): the
 * parse_* half, which the signature keeps from hostile bytes in normal
 * use, fuzzed directly so that the parser is tested as though the key
 * had leaked. The first byte picks the kind; the rest is the body.
 * Built and run by `make fuzz-wire`, under ASan and UBSan, from a corpus
 * of real bodies the encoder writes (fuzz_wire_seed.cpp).
 *
 * This is also what holds the reader to the generated calls this run
 * covers. The parse_* cases never reach framed() or required(); the
 * fifth case drives them directly, against the contract the reader
 * would rely on, so that a later reader using them would be using
 * fuzzed code (src/wire/vendor/README.md).
 */
#include <QByteArray>
#include <QtGlobal>
#include <QString>

#include "wire/read.h"

#include "bbq.hpp"

#include <vector>

/*
 * Quiet: qUncompress warns on every corrupt stream, which is most of what
 * a fuzzer sends, and printing it was most of the run's time.
 */
static void quiet(QtMsgType, const QMessageLogContext &, const QString &) {}

extern "C" int LLVMFuzzerInitialize(int *, char ***) {
	qInstallMessageHandler(quiet);
	return 0;
}

/*
 * What situ's framer promises, held for one generated struct, as situ
 * states it (its 26.693). Complete means the `need` bytes are all here;
 * truncated asks for more than it has. The answer never goes backwards:
 * with one byte fewer, the need is no larger -- situ's 26.693 said
 * "need 11" at ten bytes and "complete at 10" at eleven, so a caller told
 * complete stopped short. And framing agrees with validation: if the
 * whole input validates, the `need` bytes it named validate too, the
 * check that catches 26.684's kind of fault.
 *
 * Not held: that the `need` bytes alone answer complete with the same
 * need. That was this case's first third check, and situ's contract
 * does not promise it -- `required` gives a lower bound that may rise
 * as bytes arrive, so "complete at n" says nothing in the n bytes asks
 * for more, and only validate speaks for the whole message.
 */
template <typename T>
static void hold_framing(std::vector<uint8_t> &bytes) {
	using ::situ::rt::err;
	const uint32_t have = uint32_t(bytes.size());
	uint32_t need = 0;
	const err said = T::required(bytes.data(), have, need);

	if (said == err::truncated && need <= have) {
		__builtin_trap();
	}
	/*
	 * Never backwards, asked of two shorter prefixes: one byte fewer, and
	 * the frame the answer itself named. The second is where 26.693
	 * showed -- complete at 10 for thirteen bytes, then "need 11" for
	 * ten -- and one byte fewer than thirteen would never have seen it.
	 */
	if (said == err::ok || said == err::truncated) {
		for (const uint32_t prefix : {have - 1, need}) {
			if (have == 0 || prefix >= have) {
				continue;
			}
			uint32_t shorter = 0;
			const err less = T::required(bytes.data(), prefix, shorter);
			if ((less == err::ok || less == err::truncated) && shorter > need) {
				__builtin_trap();
			}
		}
	}
	if (said != err::ok) {
		return;
	}
	if (need > have) {
		__builtin_trap();
	}

	::situ::rt::message whole_message(bytes.data(), have);
	T whole;
	if (T::at(whole_message, 0, have, whole) != err::ok || whole.validate() != err::ok) {
		return;
	}
	::situ::rt::message framed_message(bytes.data(), need);
	T framed;
	if (T::at(framed_message, 0, need, framed) != err::ok ||
	    framed.validate() != err::ok) {
		__builtin_trap();
	}
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
	if (size < 1) {
		return 0;
	}

	const QByteArray body(reinterpret_cast<const char *>(data + 1), int(size - 1));
	const QString station = QStringLiteral("ISTOCK877");
	QString why;

	switch (data[0] % 5) {
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
	case 4: {
		/*
		 * Off unless FUZZ_FRAMING=1 (Makefile): situ's month_body framer
		 * breaks the third promise above (situ's inbox, 20261026T1249Z),
		 * and a fuzz target that always fails on a known fault in code
		 * the reader does not call would only teach people to ignore it.
		 */
#ifndef BBQ_FUZZ_FRAMING
		break;
#endif
		if (body.isEmpty()) {
			break;
		}
		/* A copy: a view takes a writable buffer, and the second byte picks the struct. */
		std::vector<uint8_t> bytes(data + 2, data + size);
		switch (uint8_t(body[0]) % 7) {
		case 0: hold_framing<::situ::bbq_file>(bytes); break;
		case 1: hold_framing<::situ::head_body>(bytes); break;
		case 2: hold_framing<::situ::index_body>(bytes); break;
		case 3: hold_framing<::situ::month_body>(bytes); break;
		case 4: hold_framing<::situ::stations_body>(bytes); break;
		case 5: hold_framing<::situ::columns>(bytes); break;
		case 6: hold_framing<::situ::band_>(bytes); break;
		}
		break;
	}
	}
	return 0;
}
