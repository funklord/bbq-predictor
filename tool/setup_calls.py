#!/usr/bin/env python3

"""Calls that must happen once, and whose absence nothing else notices.

`main` makes a few one-time setup calls whose failure mode is silence.
Remove one and the program builds, the suite passes, every other gate
stays green, and a platform nobody tests here misbehaves for somebody
else. There is no test that can catch it either: the function stays
correct, and what is gone is its only caller.

Written the day a refactor deleted one. The replacement text of a bulk
edit simply did not carry the line back; the build was green and 198
tests passed, and what found it was an assertion in the NEXT edit
looking for a comment that no longer existed (project.md sec 16.71.2).

Each entry names a call, how many sites must have it, and the measured
reason it is there. Adding one is a decision -- this is not a list of
everything main does, it is the list of things whose loss is invisible.
"""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import cxx_text

ROOT = Path(__file__).resolve().parent.parent
SOURCE = ROOT / "src" / "main.cpp"

REQUIRED = (
	(
		"bbq_install_accessibility_workaround",
		1,
		"Qt Widgets aborts on Android whenever a secondary window opens "
		"while an accessibility service is running (sec 10.6) -- which is "
		"to say, for the people who most need the screen described.",
	),
	(
		"bbq_ensure_tls_backend",
		2,
		"On Android the TLS backend does not load itself and every "
		"provider is HTTPS (sec 11.6). Two sites: the service entry and "
		"the application path. The service one is the half nobody runs "
		"by hand, so its loss would surface as a timer that fetches "
		"nothing.",
	),
)


def call_sites(text, name):
	"""Calls, not declarations: the name followed by an empty argument list."""
	return text.count(name + "()")


def control_passes():
	"""The comparison must be able to say no.

	A gate that reports an absence reads identically whether it looked or
	not, so both answers are provoked here before any file is read.
	"""
	present = "\tbbq_thing();\n\tbbq_thing();\n"
	return call_sites(present, "bbq_thing") == 2 and \
	       call_sites("nothing here", "bbq_thing") == 0


def main():
	if not control_passes():
		print("setup-calls: the control failed, so no result below means "
		      "anything", file=sys.stderr)
		return 2

	if not SOURCE.is_file():
		print(f"setup-calls: {SOURCE} is missing", file=sys.stderr)
		return 2

	if not cxx_text.self_check():
		print("setup-calls: the comment stripper is broken, so a "
		      "commented-out call would read as a live one",
		      file=sys.stderr)
		return 2

	text = cxx_text.without_comments(SOURCE.read_text(encoding="utf-8"))

	missing = 0
	for name, wanted, why in REQUIRED:
		found = call_sites(text, name)
		if found < wanted:
			missing += 1
			print(f"src/main.cpp: {name} is called {found} time(s), "
			      f"expected at least {wanted}", file=sys.stderr)
			print(f"setup-calls:   {why}", file=sys.stderr)

	if missing:
		print(f"setup-calls: {missing} of {len(REQUIRED)} required call(s) "
		      "are absent, and nothing else would have said so",
		      file=sys.stderr)
		return 1

	print(f"setup-calls: {len(REQUIRED)} call(s) whose absence is silent, "
	      "all present")
	return 0


if __name__ == "__main__":
	sys.exit(main())
