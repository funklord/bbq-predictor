#!/usr/bin/env python3

"""Files the build has to be told about, and silently ignores otherwise.

Two checks of one shape: a file exists, nothing names it, and the build
carries on reporting success while doing less than it appears to.

  a header not named in a .pro   qmake tracks no dependency on it, so
                                 editing it rebuilds nothing and every
                                 test including it asserts against a
                                 stale object (sec 16.39.1)

  a test .pro not in tests.pro   the binary is never built and never
                                 run, and `make test` reports the
                                 remaining ones passing

Neither announces itself. Both are caught by asking what is on disk and
what is referenced, which is the only question that separates them.

The first was found the hard way (sec 16.39.1).

 `src/wu/fetch_verdict.h` was added, a
test was written against it, the code was deliberately broken to watch
the test fail, and the test passed -- the binary had never been rebuilt.
The second has not happened here and is the same mistake one directory
along, which is reason enough: reading tests.pro is not something
anybody does when adding a test.
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


def registered_tests():
	"""The test projects tests.pro actually builds.

	COMMENTS ARE CUT FIRST, and that is not fussiness. Matching the
	whole file passes a tests.pro whose every entry has been commented
	out -- the suite would build nothing and the gate would report
	twelve projects registered. Found by commenting them out to watch
	this fail, and watching it pass.
	"""
	suite = (ROOT / "test" / "tests.pro").read_text(encoding="utf-8")

	live = []
	for line in suite.splitlines():
		live.append(line.split("#", 1)[0])

	return set(re.findall(r"(test_[A-Za-z0-9_]+\.pro)", "\n".join(live)))


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
		print("build-wiring: the control failed, so no result below means "
		      "anything", file=sys.stderr)
		return 2

	headers = sorted(
	        p.relative_to(ROOT).as_posix() for p in (ROOT / "src").rglob("*.h"))

	if not headers:
		print("build-wiring: found no headers at all, which is not a pass",
		      file=sys.stderr)
		return 2

	named = named_headers()
	if not named:
		print("build-wiring: no project file names any header, so the "
		      "pattern has stopped matching", file=sys.stderr)
		return 2

	absent = missing(headers, named)
	if absent:
		for header in absent:
			print("%s: named in no .pro or .pri, so qmake tracks no "
			      "dependency on it and editing it rebuilds nothing"
			      % header, file=sys.stderr)
		print("build-wiring: %d of %d header(s) untracked"
		      % (len(absent), len(headers)), file=sys.stderr)
		return 1

	projects = sorted(
	        p.name for p in (ROOT / "test").glob("test_*.pro"))
	built = registered_tests()

	if not projects:
		print("build-wiring: found no test projects at all, which is not "
		      "a pass", file=sys.stderr)
		return 2

	if not built:
		print("build-wiring: tests.pro names no test project, so the "
		      "pattern has stopped matching", file=sys.stderr)
		return 2

	unbuilt = [p for p in projects if p not in built]
	if unbuilt:
		for project in unbuilt:
			print("test/%s: not named in tests.pro, so it is never built "
			      "and never run" % project, file=sys.stderr)
		print("build-wiring: %d of %d test project(s) unregistered"
		      % (len(unbuilt), len(projects)), file=sys.stderr)
		return 1

	print("build-wiring: %d header(s) named in project files and %d test "
	      "project(s) in tests.pro, so a change to each rebuilds and runs"
	      % (len(headers), len(projects)))
	return 0


if __name__ == "__main__":
	sys.exit(main())
