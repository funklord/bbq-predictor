#!/usr/bin/env python3
"""Every option the program accepts must appear in the manual page, and
every option the page documents must exist.

A manual page is almost entirely present-tense countable claims about the
tree, which is the shape most likely to rot: options accrete, nobody
re-reads the page, and it quietly stops describing the program. This
project has already met the milder version -- the built-in --help
documented eight options while the binary accepted twenty-one -- and the
page was written by comparing the two by hand, once. A comparison done
once is a comparison that goes stale the next time somebody adds a flag.

Both directions are checked because they catch different faults, and
neither implies the other:

  in the program, not in the page   an option nobody can discover
  in the page, not in the program   an option that does not exist,
                                    which is worse: it sends a reader
                                    to type something that fails

Exits 1 when the two disagree, so `make style` fails.
"""

import re
import sys
from pathlib import Path

SOURCE = Path("src/main.cpp")
PAGE = Path("packaging/bbq-predictor.1")

# An option deliberately left out of the page goes here, with the reason.
# An empty mapping is the honest state today: everything the program
# accepts is documented.
ALLOWED_UNDOCUMENTED = {}

# Only literals actually TESTED as arguments count. Matching every
# "--x" string in the file would make a gate out of anything that
# happens to look like a flag in a message, and a gate with an ignore
# list for its own false findings has been switched off by instalments.
IN_SOURCE = re.compile(
		r'(?:option_value\s*\(\s*arguments\s*,\s*QStringLiteral\(\s*'
		r'|arguments\.contains\s*\(\s*QStringLiteral\(\s*)'
		r'"(--[a-z][a-z-]*)"')

# roff spells a literal hyphen \- so it is not confused with a line break.
IN_PAGE = re.compile(r'\\-\\-([a-z][a-z\\-]*)')


def options_in_source(text):
	return set(IN_SOURCE.findall(text))


def options_in_page(text):
	found = set()
	for raw in IN_PAGE.findall(text):
		found.add("--" + raw.replace("\\-", "-"))
	return found


def compare(source_text, page_text):
	"""Returns (undocumented, invented)."""
	in_source = options_in_source(source_text)
	in_page = options_in_page(page_text)
	undocumented = in_source - in_page - set(ALLOWED_UNDOCUMENTED)
	invented = in_page - in_source
	return sorted(undocumented), sorted(invented)


def control_passes():
	"""Can this comparison speak at all?

    A gate whose failure mode is silence has to demonstrate on every run
    that it can fail, or its silence means only that it ran. Both
    directions are provoked deliberately, because a check that catches
    one and not the other reads exactly like one that catches both.
    """
	source = 'option_value(arguments, QStringLiteral("--real"));'
	page = r'.B \-\-invented'

	undocumented, invented = compare(source, page)
	return undocumented == ["--real"] and invented == ["--invented"]


def main():
	if not control_passes():
		print("man-options: the control failed, so no result below means "
			  "anything", file=sys.stderr)
		return 2

	for path in (SOURCE, PAGE):
		if not path.is_file():
			print(f"man-options: {path} is missing", file=sys.stderr)
			return 2

	source_text = SOURCE.read_text(encoding="utf-8")
	page_text = PAGE.read_text(encoding="utf-8")

	in_source = options_in_source(source_text)
	if not in_source:
		print("man-options: found no options in " + str(SOURCE) +
			  " -- the pattern has stopped matching, which is not the "
			  "same as a program with no options", file=sys.stderr)
		return 2

	undocumented, invented = compare(source_text, page_text)

	for option in undocumented:
		print(f"man-options: {option} is accepted but not in {PAGE}",
			  file=sys.stderr)
	for option in invented:
		print(f"man-options: {option} is documented but not accepted",
			  file=sys.stderr)

	if undocumented or invented:
		return 1

	print(f"man-options: {len(in_source)} option(s), documented both ways")
	return 0


if __name__ == "__main__":
	sys.exit(main())
