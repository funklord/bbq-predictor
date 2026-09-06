#!/usr/bin/env python3

"""Every header under src/ must be named in a project file (sec 16.39.1).

qmake tracks a header's dependencies only if something names it. A
header that no .pro or .pri lists appears nowhere in the generated
Makefile, so editing it rebuilds nothing -- and every test that includes
it goes on asserting against a stale object.

That is not a hypothetical. `src/wu/fetch_verdict.h` was added, a test
was written against it, the code was deliberately broken to watch the
test fail, and the test passed: the binary had never been rebuilt. The
gate exists because reading the .pro file is not something anybody does
when adding a header.
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# Where a header may be named. Nothing else in the tree affects what
# qmake tracks.
PROJECT_FILES = ("*.pro", "test/*.pro", "test/*.pri")

NAMES_A_HEADER = re.compile(r"[A-Za-z0-9_./$]*src/([A-Za-z0-9_/]+\.h)")


def named_headers():
	found = set()
	for pattern in PROJECT_FILES:
		for path in ROOT.glob(pattern):
			text = path.read_text(encoding="utf-8")
			for match in NAMES_A_HEADER.finditer(text):
				found.add("src/" + match.group(1))
	return found


def missing(headers, named):
	return sorted(h for h in headers if h not in named)


def control_passes():
	"""The comparison must be able to say no.

	A gate whose failure mode is silence has to demonstrate it can
	speak, and this one's whole job is to report an absence -- which is
	the shape that reads identically whether it looked or not.
	"""
	seen = missing(["src/a.h", "src/b.h"], {"src/a.h"})
	return seen == ["src/b.h"] and missing(["src/a.h"], {"src/a.h"}) == []


def main():
	if not control_passes():
		print("header-deps: the control failed, so no result below means "
		      "anything", file=sys.stderr)
		return 2

	headers = sorted(
	        p.relative_to(ROOT).as_posix() for p in (ROOT / "src").rglob("*.h"))

	if not headers:
		print("header-deps: found no headers at all, which is not a pass",
		      file=sys.stderr)
		return 2

	named = named_headers()
	if not named:
		print("header-deps: no project file names any header, so the "
		      "pattern has stopped matching", file=sys.stderr)
		return 2

	absent = missing(headers, named)
	if absent:
		for header in absent:
			print("%s: named in no .pro or .pri, so qmake tracks no "
			      "dependency on it and editing it rebuilds nothing"
			      % header, file=sys.stderr)
		print("header-deps: %d of %d header(s) untracked"
		      % (len(absent), len(headers)), file=sys.stderr)
		return 1

	print("header-deps: %d header(s) named in project files, so a change "
	      "to each rebuilds what includes it" % len(headers))
	return 0


if __name__ == "__main__":
	sys.exit(main())
