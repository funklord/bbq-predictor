#!/usr/bin/env python3

"""C++ with its comments removed, for gates that read source.

EVERY GATE HERE THAT GREPS SOURCE COUNTED COMMENTED-OUT CODE AS LIVE
(project.md sec 16.41). Measured on one afternoon, by commenting out the
thing each gate exists to find and watching it report success:

    signal_listeners  both connects for a signal    "0 unheard"
    palette_contrast  a colour assignment           palette clean
    exit_codes        a `return 3;`                 3 still returned
    man_options       an option's parse site        22 options

Four gates, one fault, and in each the gate went on describing a program
that had stopped containing what it was describing.

Shared rather than copied four times, because four copies of a rule is
four things to be wrong -- which is the argument this project already
makes about a number written in two places.

The `//` inside a string literal is truncated by this. That can only
make a thing LOOK absent and never make an absent thing look present,
which is the safe direction: a false finding prints something to go and
read, a false silence prints nothing at all.
"""

import re

BLOCK_COMMENT = re.compile(r"/\*.*?\*/", re.S)
LINE_COMMENT = re.compile(r"//[^\n]*")


def without_comments(text):
	return LINE_COMMENT.sub("", BLOCK_COMMENT.sub("", text))


def self_check():
	"""Whether the stripping can be relied on at all.

	A gate calls this before using it, so a broken helper refuses to
	report rather than quietly returning the text unchanged -- which
	would restore the exact fault it was written for, in every caller
	at once.
	"""
	if without_comments("a // gone\nb") != "a \nb":
		return False
	if without_comments("a /* gone */ b") != "a  b":
		return False
	if without_comments("a /* two\nlines */ b") != "a  b":
		return False

	# And it must not eat live code.
	return without_comments("keep(this);") == "keep(this);"
