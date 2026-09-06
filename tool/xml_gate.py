#!/usr/bin/env python3

"""Refuse XML this project cannot ship (project.md sec 16.24.1).

Three times now a comment written with a `--` in it has been committed,
and each time the failure arrived as an aapt2 parse error in the middle
of an Android build several minutes long, blaming a file in a generated
directory rather than the one in the tree. XML forbids `--` inside a
comment outright, which is a rule nothing about writing prose suggests.

A parser rather than a pattern, because `--` in a comment is only the
instance that keeps happening. An unclosed tag, a stray ampersand and a
mismatched quote all cost the same several minutes and are found by the
same call.
"""

import sys
import xml.etree.ElementTree as ET
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent

# Where XML this project owns lives. Generated and build trees are
# excluded by not being listed rather than by being filtered: a
# directory added later is invisible to this gate until somebody names
# it, which is the failure a wildcard would hide.
LOOKS_IN = ("android", "packaging")

# A comment carrying a double hyphen, which is exactly the mistake this
# was written for. Parsed BEFORE any real file: a gate whose failure
# mode is silence has to demonstrate it can speak, and an XML library
# that stopped rejecting this would let every real file through while
# reporting a clean sweep.
CONTROL_BAD = '<a><!-- a -- comment --></a>'
CONTROL_GOOD = '<a><!-- a plain comment --></a>'


def parses(text):
	try:
		ET.fromstring(text)
		return True
	except ET.ParseError:
		return False


def main():
	if parses(CONTROL_BAD) or not parses(CONTROL_GOOD):
		print("xml-gate: the control failed, so no result below means "
		      "anything", file=sys.stderr)
		return 2

	files = sorted(
	        path
	        for name in LOOKS_IN
	        for path in (ROOT / name).rglob("*.xml"))

	if not files:
		print("xml-gate: found no XML at all, which is not a pass",
		      file=sys.stderr)
		return 2

	bad = 0
	for path in files:
		try:
			ET.parse(path)
		except ET.ParseError as failed:
			print("%s: %s" % (path.relative_to(ROOT), failed),
			      file=sys.stderr)
			bad += 1

	if bad:
		print("xml-gate: %d of %d file(s) will not parse"
		      % (bad, len(files)), file=sys.stderr)
		return 1

	print("xml-gate: %d file(s) parse, comments included" % len(files))
	return 0


if __name__ == "__main__":
	sys.exit(main())
