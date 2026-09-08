#!/usr/bin/env python3

"""Refuse a QtTest slot that QtTest will silently not run.

QtTest reads a slot named `foo_data()` as the DATA PROVIDER for a test
called `foo()`. It is not itself a test: it is never listed by
-functions, never counted in the totals, and never executed unless a
`foo()` exists to consume it. So a test whose name merely ENDS in _data
compiles, links, and quietly does nothing -- and the suite reports the
same "all passed" it would report if the test were there.

Found the hard way (project.md sec 16.95): a new test called
the_resolution_is_counted_from_the_data was read as the data function
for a test called the_resolution_is_counted_from_the, which does not
exist. Every intermediate artifact contained the method -- the generated
moc, the object file, the binary's string table -- and only the runtime
metaobject did not, so the search went through the toolchain before it
went to the name.

The rule is exact rather than heuristic, so there is nothing to suppress:
a slot ending in _data is correct when the slot it feeds exists, and is a
silent no-run when it does not.

Terminates: one pass over a fixed file list, no recursion, no spawning.
"""

import glob
import os
import re
import sys
import tempfile

SLOT = re.compile(r"^\s*void\s+([a-zA-Z_][A-Za-z0-9_]*)\s*\(\s*\)\s*;")

SUFFIX = "_data"


def slots_in(path):
	"""Every slot name declared in a `private slots:` block."""
	names = []
	inside = False

	for line in open(path, encoding="utf-8"):
		stripped = line.strip()

		if stripped.endswith("slots:"):
			inside = True
			continue

		if inside and stripped.startswith("};"):
			inside = False
			continue

		if inside and stripped in ("public:", "private:", "protected:"):
			inside = False
			continue

		if not inside:
			continue

		found = SLOT.match(line)
		if found:
			names.append(found.group(1))

	return names


def check(paths):
	"""Return every (path, name) that QtTest would not run."""
	silent = []

	for path in paths:
		names = slots_in(path)
		declared = set(names)

		for name in names:
			if not name.endswith(SUFFIX):
				continue
			if name[:-len(SUFFIX)] in declared:
				continue
			silent.append((path, name))

	return silent


def control():
	"""Refuse to report anything until the check is seen to speak.

	A gate whose only output is silence has to demonstrate on every run
	that it can say something, or its silence means only that it ran.
	"""
	with_test = (
		"class t : public QObject {\n"
		"private slots:\n"
		"\tvoid a();\n"
		"\tvoid a_data();\n"
		"};\n")
	without_test = (
		"class t : public QObject {\n"
		"private slots:\n"
		"\tvoid b_data();\n"
		"};\n")

	with tempfile.TemporaryDirectory() as directory:
		clean = os.path.join(directory, "clean.cpp")
		broken = os.path.join(directory, "broken.cpp")

		open(clean, "w", encoding="utf-8").write(with_test)
		open(broken, "w", encoding="utf-8").write(without_test)

		if check([clean]):
			return "a data function WITH its test was reported"

		if len(check([broken])) != 1:
			return "a data function with no test was NOT reported"

	return None


def main():
	failed = control()
	if failed is not None:
		print("test-slots: the control failed -- " + failed, file=sys.stderr)
		print("test-slots: no result below means anything", file=sys.stderr)
		return 2

	paths = sorted(glob.glob("test/test_*.cpp"))
	if not paths:
		print("test-slots: no test sources found", file=sys.stderr)
		return 2

	silent = check(paths)

	for path, name in silent:
		feeds = name[:-len(SUFFIX)]
		print(path + ": " + name + "() ends in " + SUFFIX + ", so QtTest "
		      "reads it as the data function for " + feeds + "(), which "
		      "does not exist -- it will never run", file=sys.stderr)

	if silent:
		print("test-slots: " + str(len(silent)) + " slot(s) QtTest would "
		      "not run", file=sys.stderr)
		return 1

	total = sum(len(slots_in(path)) for path in paths)
	print("test-slots: " + str(total) + " slot(s) in " + str(len(paths)) +
	      " file(s), none named so that QtTest would skip it")
	return 0


if __name__ == "__main__":
	sys.exit(main())
