#!/usr/bin/env python3

"""Refuse a source file that no test project links, unless it is waived.

The suite links its sources by naming them, one project file at a time,
so a NEW source file is linked by nobody until somebody remembers. That
failure is silent in the direction that matters: the suite still builds,
still passes, and still prints the same totals, while the file it does
not contain goes untested.

This asserts the whole partition rather than the interesting half, which
is the only form that cannot rot (project.md sec 16.111):

  * a source no project links and no waiver names is refused;
  * a WAIVED source that has since been linked is refused too, so a
    waiver cannot outlive its reason quietly;
  * a waiver naming a file that no longer exists is refused, so the
    list cannot accumulate ghosts.

The second and third are what make the first trustworthy. A bare list of
exceptions is a claim; a list that cannot grow, shrink or go stale
without the gate saying so is a guarantee.

It replaces a COUNT. Sec 16.58 recorded "24 of 28" files linked, and by
the time anybody looked again the tree held 30 -- the rule intact, the
number wrong, and nothing in the ordinary course of work bringing the
two together. A number in prose is re-derived by nobody.

Matching is on the path below src/, never the basename: two files may
share a name, and a basename match would report the linked one and let
the other through -- a gate passing for the wrong file.

Terminates: one pass over a fixed file list, no recursion, no spawning.
"""

import glob
import os
import sys
import tempfile

# Each waiver carries its reason, because a reason is what lets the next
# reader decide whether it still holds. Paths are relative to src/.
WAIVED = {
	"main.cpp":
		"the entry point; the argument parsing it used to hold was "
		"extracted to cli/options.cpp, which test_cli does link, so "
		"what remains here is wiring",
	"net/probe.cpp":
		"the --probe diagnostic, whose predicate is two lines of what "
		"a live server replies -- watched failing against the real "
		"network instead (sec 16.61.2)",
	"net/tls_backend.cpp":
		"selects a TLS backend from the installed Qt plugins at "
		"startup; there is nothing to assert without the plugin path "
		"of a real installation",
	"wu/fetch_once.cpp":
		"the one-shot fetch, which talks to the live provider",
}


def sources(root):
	"""Every .cpp below root, as a path relative to it."""
	found = []
	for base, _dirs, files in os.walk(root):
		for name in files:
			if name.endswith(".cpp"):
				full = os.path.join(base, name)
				found.append(os.path.relpath(full, root))
	return sorted(found)


def linked_in(projects):
	"""The text of every project file, joined, for substring matching."""
	text = ""
	for path in projects:
		text += open(path, encoding="utf-8").read()
	return text


def check(root, projects, waived):
	"""Returns (unlinked, stale_waivers, ghost_waivers)."""
	text = linked_in(projects)
	present = sources(root)

	unlinked = []
	for path in present:
		if path in waived:
			continue
		if path.replace(os.sep, "/") not in text:
			unlinked.append(path)

	stale = []
	for path in sorted(waived):
		if path in present and path.replace(os.sep, "/") in text:
			stale.append(path)

	ghosts = [path for path in sorted(waived) if path not in present]

	return unlinked, stale, ghosts


def control():
	"""Prove the gate can speak, in all three directions it must."""
	with tempfile.TemporaryDirectory() as directory:
		root = os.path.join(directory, "src")
		os.makedirs(os.path.join(root, "sub"))

		for name in ("sub/kept.cpp", "sub/missed.cpp", "sub/waived.cpp"):
			open(os.path.join(root, name), "w", encoding="utf-8").write("\n")

		project = os.path.join(directory, "one.pro")
		open(project, "w", encoding="utf-8").write(
			"SOURCES += $$PWD/../src/sub/kept.cpp\n")

		unlinked, stale, ghosts = check(
			root, [project], {"sub/waived.cpp": "for the control"})

		if unlinked != ["sub/missed.cpp"]:
			return "an unlinked source was not reported: " + repr(unlinked)
		if stale or ghosts:
			return "a clean waiver was reported: " + repr(stale + ghosts)

		# A waiver whose file IS linked must be refused.
		_u, stale, _g = check(root, [project], {"sub/kept.cpp": "stale"})
		if stale != ["sub/kept.cpp"]:
			return "a waiver that is linked anyway was not reported"

		# A waiver naming nothing must be refused.
		_u, _s, ghosts = check(root, [project], {"sub/gone.cpp": "ghost"})
		if ghosts != ["sub/gone.cpp"]:
			return "a waiver naming no file was not reported"

	return None


def main():
	failed = control()
	if failed is not None:
		print("test-links: the control failed -- " + failed, file=sys.stderr)
		print("test-links: no result below means anything", file=sys.stderr)
		return 2

	projects = sorted(glob.glob("test/*.pro")) + sorted(
		glob.glob("test/*.pri"))
	if not projects:
		print("test-links: no test project files found", file=sys.stderr)
		return 2

	unlinked, stale, ghosts = check("src", projects, WAIVED)

	for path in unlinked:
		print("src/" + path + ": no test project links it, and no waiver "
		      "names it -- add it to a test's SOURCES, or waive it here "
		      "with the reason", file=sys.stderr)

	for path in stale:
		print("src/" + path + ": waived as unlinked, but a test project "
		      "links it now -- drop the waiver", file=sys.stderr)

	for path in ghosts:
		print("src/" + path + ": waived, but no such file exists -- drop "
		      "the waiver", file=sys.stderr)

	if unlinked or stale or ghosts:
		print("test-links: " + str(len(unlinked) + len(stale) +
		      len(ghosts)) + " problem(s)", file=sys.stderr)
		return 1

	total = len(sources("src"))
	print("test-links: " + str(total - len(WAIVED)) + " of " + str(total) +
	      " source(s) linked by a test, " + str(len(WAIVED)) +
	      " waived with a reason")
	return 0


if __name__ == "__main__":
	sys.exit(main())
