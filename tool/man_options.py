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

sys.path.insert(0, str(Path(__file__).resolve().parent))
import cxx_text  # noqa: E402  -- needs the path line above

# Each program against its own page. Two since the publisher left the
# applet (project.md sec 15.13): an option moved between them and left in
# the old page would otherwise read as documented, which is the second
# direction below arriving through a move rather than a typo.
PROGRAMS = (
	(Path("src/main.cpp"), Path("packaging/bbq-predictor.1")),
	(Path("src/publish/main.cpp"), Path("packaging/bbq-predictor-publish.1")),
)

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
		r'|arguments\.contains\s*\(\s*QStringLiteral\(\s*'
		# And the raw argv form. Added because a flag read before any
		# QStringList exists -- the Android service entry has to be,
		# since deciding it is what decides whether an application
		# object is built at all -- used neither form above and was
		# invisible here. The gate reported twenty-one options while
		# the program accepted twenty-two, which is the exact failure
		# it exists to prevent, arriving through a shape nobody had
		# thought of.
		r'|qstrcmp\s*\(\s*argv\[[a-z]\]\s*,\s*'
		# And the helper those raw reads went through once there were
		# three of them. The same shape one indirection along: the
		# literal moved from the qstrcmp to its callers, and this gate
		# went from finding twenty-two options to nineteen without the
		# program changing what it accepts. It reported all three,
		# which is the whole of why the form above was added.
		r'|has_flag\s*\(\s*argc\s*,\s*argv\s*,\s*)'
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


# What the packaging invokes the program WITH. A unit file is a caller
# like any other, and the only one whose failure nobody watches.
UNIT_DIR = Path("packaging/systemd")

# An option appearing in a unit's ExecStart or ExecCondition line.
IN_UNIT = re.compile(r"(--[a-z][a-z-]*)")


# And the wrapper both schedulers run, which is where the unit's command
# line went: the timer and cron share it, so the options live there now
# (project.md sec 15.12). Its arguments are built with `set --`, and only
# those lines are read -- its comments name options too, and a comment
# is not a call. It runs the publisher, so it is held to the publisher's
# options and not to the union, where an applet-only option would pass.
WRAPPER = Path("packaging/fetch")
WRAPPER_RUNS = Path("src/publish/main.cpp")


def options_in_units(texts):
	found = set()
	for text in texts:
		for line in text.splitlines():
			if line.startswith("Exec"):
				found.update(IN_UNIT.findall(line))
	return found


def options_in_wrapper(text):
	"""Continued lines joined first: the first version read one physical
	line at a time and never saw --signing-key, which sits on the
	continuation of its `set --`, so renaming it passed this gate."""
	found = set()
	for line in text.replace("\\\n", " ").splitlines():
		code = line.strip()
		if code.startswith("set -- "):
			found.update(IN_UNIT.findall(code))
	return found


def unit_only(unit_options, source_options):
	"""Options the packaging passes that the program does not accept."""
	return sorted(o for o in unit_options if o not in source_options)


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
	if undocumented != ["--real"] or invented != ["--invented"]:
		return False

	# And the unit comparison, which is silent in the same way.
	unit = "ExecStart=/usr/bin/thing --real --gone\nUser=nobody\n"
	seen = options_in_units([unit])
	if not (seen == {"--real", "--gone"} and
	        unit_only(seen, {"--real"}) == ["--gone"]):
		return False

	# And the wrapper: its `set --` lines, never its comments.
	wrapper = ("# --commented\nset -- --real \"$x\"\n"
	           "\tset -- \"$@\" --gone \\\n\t        --continued x\n")
	return options_in_wrapper(wrapper) == {"--real", "--gone", "--continued"}


def main():
	if not control_passes():
		print("man-options: the control failed, so no result below means "
			  "anything", file=sys.stderr)
		return 2

	for source, page in PROGRAMS:
		for path in (source, page):
			if not path.is_file():
				print(f"man-options: {path} is missing", file=sys.stderr)
				return 2

	if not cxx_text.self_check():
		print("man-options: the comment stripper is broken, so a "
		      "commented-out option would read as one the program "
		      "accepts", file=sys.stderr)
		return 2

	accepted = {}
	bad = False
	for source, page in PROGRAMS:
		source_text = cxx_text.without_comments(
		        source.read_text(encoding="utf-8"))
		page_text = page.read_text(encoding="utf-8")

		in_source = options_in_source(source_text)
		if not in_source:
			print("man-options: found no options in " + str(source) +
			      " -- the pattern has stopped matching, which is not the "
			      "same as a program with no options", file=sys.stderr)
			return 2
		accepted[source] = in_source

		undocumented, invented = compare(source_text, page_text)
		for option in undocumented:
			print(f"man-options: {option} is accepted by {source} but not "
			      f"in {page}", file=sys.stderr)
		for option in invented:
			print(f"man-options: {option} is in {page} but not accepted by "
			      f"{source}", file=sys.stderr)
		bad = bad or bool(undocumented or invented)

	# A UNIT FILE IS A CALLER, and the one whose failure nobody sees.
	#
	# The service runs on a timer, as its own user, and its stdout goes
	# where sec 17.4 says nothing reads. An option renamed in the program
	# and not in the unit fails there and only there -- the archive simply
	# stops advancing, which is the symptom sec 15.6 exists to remove.
	unit_texts = [path.read_text(encoding="utf-8")
	              for path in sorted(UNIT_DIR.glob("*.service"))]

	if not unit_texts:
		print("man-options: found no unit files, so nothing checked what "
		      "the packaging invokes", file=sys.stderr)
		return 2

	if not WRAPPER.is_file():
		print(f"man-options: {WRAPPER} is missing, so nothing checked what "
		      "the schedulers invoke", file=sys.stderr)
		return 2
	in_wrapper = options_in_wrapper(WRAPPER.read_text(encoding="utf-8"))
	in_units = options_in_units(unit_texts)
	if not in_wrapper:
		print("man-options: the wrapper names no option, so the pattern has "
		      "stopped matching", file=sys.stderr)
		return 2

	every = set().union(*accepted.values())
	unmatched = unit_only(in_units, every)
	unmatched += unit_only(in_wrapper, accepted[WRAPPER_RUNS])
	for option in sorted(set(unmatched)):
		print("man-options: %s is passed by the packaging and the program "
		      "does not accept it" % option, file=sys.stderr)

	if bad or unmatched:
		return 1

	counts = ", ".join(f"{len(accepted[s])} in {s}" for s, _ in PROGRAMS)
	print(f"man-options: {counts}, documented both ways, and "
	      f"{len(in_units | in_wrapper)} passed by the packaging that its "
	      f"program accepts")
	return 0


if __name__ == "__main__":
	sys.exit(main())
