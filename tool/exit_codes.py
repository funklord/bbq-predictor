#!/usr/bin/env python3
"""The fetch exit codes are a contract, and four files state them.

--fetch-once distinguishes a partial fetch from a failed one so the
systemd timer can forgive a station that has gone quiet without
forgiving an outage. That distinction is only worth having while every
copy of it agrees:

    src/wu/fetch_once.cpp    the codes the program actually returns
    src/wu/fetch_once.h      what callers are told they mean
    packaging/systemd/...    SuccessExitStatus, which forgives one
    packaging/bbq-predictor.1  the EXIT STATUS a reader is given

Drift here fails silently in the worst direction. If the partial code
changes and the unit is not updated, the timer marks itself failed every
time a station is briefly quiet -- noise that buries a real fault. If
the unit is widened instead, it forgives the outage it exists to report.
Neither shows up in a test run or a package build.

Exits 1 when the copies disagree, so `make style` fails.
"""

import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import cxx_text  # noqa: E402  -- needs the path line above

CODE = Path("src/wu/fetch_once.cpp")
HEADER = Path("src/wu/fetch_once.h")
UNIT = Path("packaging/systemd/bbq-predictor-fetch.service")
MANUAL = Path("packaging/bbq-predictor.1")

# The partial case: some band failed, the composite still covers now.
# This is the one the unit forgives, and the only one it may forgive.
PARTIAL = 3


def returns_of(text, function):
	"""Integer literals returned by `function`, which must exist.

	Bounded to the function rather than to the end of the file. Reading
	to EOF worked only because this function happens to be last, and
	would have attributed a later helper's `return 0` to it -- a gate
	that is right by the accident of where somebody put a function is
	one commit from being wrong without anybody touching it.

	It sees integer LITERALS. A return of a variable is invisible here,
	which is a real limit and is the reason the unit and the manual are
	checked against this set rather than against each other: two
	documents agreeing while the program does something else is what
	sec 15.10.2 is about.
	"""
	start = text.index(function)

	# The closing brace of a function whose body starts in column 0 is a
	# brace in column 0. Crude, and exact for this file's style, which
	# the project's own indentation gate enforces.
	end = text.find('\n}\n', start)
	body = text[start:end if end != -1 else len(text)]

	return {int(n) for n in re.findall(r'\breturn\s+(\d+)\s*;', body)}


def control_passes():
	"""Can this comparison speak?

	A stand-in body returning a code the checker is not looking for must
	be seen, or the parse could be returning an empty set and reporting
	agreement with everything.
	"""
	sample = "int f() { return 0; return 7; }"
	return returns_of(sample, "int f()") == {0, 7}


def main():
	if not control_passes():
		print("exit-codes: the control failed, so no result below means "
		      "anything", file=sys.stderr)
		return 2

	for path in (CODE, HEADER, UNIT, MANUAL):
		if not path.is_file():
			print(f"exit-codes: {path} is missing", file=sys.stderr)
			return 2

	if not cxx_text.self_check():
		print("exit-codes: the comment stripper is broken, so a "
		      "commented-out return would count as one the program "
		      "makes", file=sys.stderr)
		return 2

	code = cxx_text.without_comments(CODE.read_text(encoding="utf-8"))
	unit = UNIT.read_text(encoding="utf-8")
	manual = MANUAL.read_text(encoding="utf-8")
	header = HEADER.read_text(encoding="utf-8")

	returned = returns_of(code, "int bbq_wu_fetch_once(")
	if not returned:
		print("exit-codes: no return values found in bbq_wu_fetch_once -- "
		      "the pattern has stopped matching, which is not the same as "
		      "a function that returns nothing", file=sys.stderr)
		return 2

	bad = 0

	if PARTIAL not in returned:
		print(f"exit-codes: the program no longer returns {PARTIAL}, which "
		      f"is the code the unit forgives", file=sys.stderr)
		bad += 1

	forgiven = re.findall(r'^SuccessExitStatus=(.+)$', unit, re.M)
	if forgiven != [str(PARTIAL)]:
		print(f"exit-codes: the unit forgives {forgiven!r}, not "
		      f"[{str(PARTIAL)!r}]", file=sys.stderr)
		bad += 1

	# Every code the program can return must be explained to a reader.
	section = manual[manual.index(".SH EXIT STATUS"):]
	section = section[:section.index(".SH ", 1)]
	for value in sorted(returned):
		if not re.search(rf'^\.B {value}$', section, re.M):
			print(f"exit-codes: {value} is returned but not in the manual "
			      f"page's EXIT STATUS", file=sys.stderr)
			bad += 1

	# And the header, which is what a caller reads, must name them all.
	# Deliberately the weaker question -- does the number appear -- since
	# the alternative is parsing prose, and a checker that parses prose
	# breaks when somebody rewords a sentence correctly.
	explained = {int(n) for n in re.findall(r'\b(\d+)\b', header)}
	for value in sorted(returned - explained):
		print(f"exit-codes: {value} is returned but not named in "
		      f"{HEADER}", file=sys.stderr)
		bad += 1

	if bad:
		return 1

	print(f"exit-codes: {sorted(returned)} returned, {PARTIAL} forgiven by "
	      f"the unit and explained in the manual")
	return 0


if __name__ == "__main__":
	sys.exit(main())
