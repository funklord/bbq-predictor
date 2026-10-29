#!/usr/bin/env python3
"""Seed fuzz_providers.cpp: its starting corpus and its dictionary.

The corpus is every JSON response the reader tests embed as a raw string,
each written once per reader with the selector byte fuzz_providers.cpp
reads first -- which test feeds which reader is not worth tracking when
the fuzzer is about to mutate them all anyway. The dictionary is every
field name the readers ask for, so mutation speaks the providers' words.

Usage: fuzz_providers_seed.py CORPUS_DIR DICT_FILE
"""
import pathlib
import re
import sys

READERS = 9
TESTS = ["test/test_reader.cpp", "test/test_providers.cpp"]
SOURCES = ["src/wu/reader.cpp", "src/openmeteo/forecast.cpp", "src/met/nowcast.cpp"]


def main() -> int:
	corpus = pathlib.Path(sys.argv[1])
	corpus.mkdir(parents=True, exist_ok=True)

	seeds = 0
	for path in TESTS:
		text = pathlib.Path(path).read_text(encoding="utf-8")
		for index, body in enumerate(re.findall(r'R"\((.*?)\)"', text, re.S)):
			for reader in range(READERS):
				name = f"{pathlib.Path(path).stem}-{index}-{reader}"
				(corpus / name).write_bytes(bytes([reader]) + body.encode("utf-8"))
				seeds += 1

	words = set()
	for path in SOURCES:
		text = pathlib.Path(path).read_text(encoding="utf-8")
		words.update(re.findall(r'QStringLiteral\("([A-Za-z0-9_]+)"\)', text))
	with open(sys.argv[2], "w", encoding="utf-8") as out:
		for word in sorted(words):
			out.write(f'"\\"{word}\\""\n')

	print(f"fuzz-providers: {seeds} seed(s), {len(words)} dictionary word(s)")
	return 0 if seeds and words else 1


if __name__ == "__main__":
	sys.exit(main())
