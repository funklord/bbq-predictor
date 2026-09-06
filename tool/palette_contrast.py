#!/usr/bin/env python3
"""Every colour this project draws on another must stay legible on it.

harmonization.md settles that a surface drawn onto a ground it does not
own must be legible either way, and gives 3:1 as the floor for anything
larger than body text. The graph palette is exactly that: two schemes,
one set of data colours shared between them because a measurement does
not change when the room gets darker, and a handful of surfaces that do.

WHICH PAIRS ACTUALLY MEET IS THE WHOLE OF THE KNOWLEDGE HERE, and it is
why this exists as a tool rather than as a paragraph. A naive sweep --
every colour against the plot background -- reports eleven violations in
a palette that has none. Gridlines are meant to be faint. Band shading
and the rain wash are fills, not ink. The readout's text is drawn on the
readout's own box, not on the plot. Measuring those against the
background is the wrong population, and the answer looks like a finding.

Exits 1 when a pair falls under the floor, so `make style` fails.
"""

import itertools
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import cxx_text  # noqa: E402  -- needs the path line above
from pathlib import Path

SOURCE = Path("src/graph/forecast_graph.cpp")

FLOOR = 3.0

# ink, ground, and why the two are ever drawn together.
PAIRS = [
	("axis_text", "background", "the scale numbers, on the plot"),
	("temperature", "background", "the forecast curve"),
	("corrected", "background", "the bias-corrected overlay and its caption"),
	("day_divider", "background", "the midnight rule"),
	("stale_warning", "background", "said when a band is old"),
	("readout_text", "readout_back", "the cursor readout, on its own box"),
]

# A pair kept under the floor deliberately, with what it measured at
# when it was allowed and why.
ALLOWED_UNDER = {
	("readout_edge", "readout_back"): (
		"2.94 on dark. It is the border of a box, not ink in it, and the "
		"box's own text clears the floor at 11.83."),
}

# THE PLOT HAS MORE THAN ONE GROUND, and for a long time this gate knew
# about one (project.md sec 16.28, sec 16.31). Everything in PAIRS is
# also drawn over the washes below, wherever the weather puts them, so a
# colour cleared against `background` has been cleared against the
# easiest case only.
#
# Named separately rather than folded in, because a wash is not a
# palette entry: it is a colour composited over background at the alpha
# the painter uses. `None` means the colour carries its own alpha, as
# grill_window does.
WASHES = [
	("grill", "grill_window", None),
	("chance", "chance", 60),
	("rain", "rain", 120),
]

# The grill cap is that colour's OWN alpha, read from the palette, so
# this gate and the painter cannot disagree about it. It used to be a
# literal 80 here copied from a literal 80 there -- two copies of one
# number, which is one more than can be kept true (sec 16.29). The other
# two alphas are still literals in the painter and are copied above,
# which is the same fault at one remove and is recorded in sec 16.31.

# The inks that meet a wash WITH NOTHING UNDER THEM.
#
# Three names are deliberately absent and each for a different reason,
# which is why they are listed here rather than just left out:
#
#   temperature   haloed (sec 16.32). The curve is stroked in the
#                 plot's own ground before the ink, so what it is read
#                 against is `background` -- already checked in PAIRS --
#                 whatever the weather shades underneath. `corrected`
#                 is NOT haloed and stays in the list below: a halo on
#                 it erased the curve it runs along (sec 16.32.1).
#
#   stale_warning DRAWN NOWHERE. It is set in both schemes, carried
#                 through the contrast clamp and asserted about by three
#                 tests, and no painter ever uses it (sec 16.32.2). It
#                 was in this list described as "said when a band is
#                 old", which is a claim about a drawing that does not
#                 happen.
WASHED_PAIRS = [
	("axis_text", "the scale numbers, where a wash shades them"),
	("corrected", "the bias-corrected overlay, over one"),
	("day_divider", "the midnight rule crossing one"),
	("now_marker", "the now line crossing one"),
]

# THE WORST INK OVER EVERY COMBINATION OF WASHES, MEASURED (sec 16.31).
#
# A table rather than an ignore list, because the honest version of this
# check is thirty-four per-ink waivers and a gate carrying that has been
# switched off by instalments. One number per combination says the same
# thing and cannot hide a new failure behind an old name.
#
# Two kinds of entry, and the gate reports the split rather than
# averaging over it:
#
#   >= FLOOR   a real pass. The combination is legible.
#   <  FLOOR   a TRIPWIRE. The number is what was measured on
#              2026-09-06 and is not endorsed; the gate's job is to
#              refuse to let it get worse while the fix is decided.
#
# It could not be fixed by dimming, and that was structural: the grill
# wash alone left the curve at 3.03:1 on dark, so there was no budget
# for a second, and no (chance, rain) pair down to alpha 5 cleared 3:1
# with all three present. The CURVE was fixed by giving it a ground of
# its own instead (sec 16.32); it no longer appears here at all, because
# what it is read against is `background` and PAIRS checks that.
#
# Every remaining number below is the bias-corrected overlay, which is
# not haloed: a halo on it erased the curve it runs along, and the fix
# for that is to draw every halo before every ink, which is a
# restructure rather than an alpha (sec 16.32.1).
WASH_WORST = {
	("light", "grill"): 3.34,
	("light", "chance"): 3.45,
	("light", "rain"): 2.32,
	("light", "grill+chance"): 2.73,
	("light", "grill+rain"): 1.92,
	("light", "chance+rain"): 2.03,
	("light", "grill+chance+rain"): 1.73,
	("dark", "grill"): 3.60,
	("dark", "chance"): 2.79,
	("dark", "rain"): 2.21,
	("dark", "grill+chance"): 2.47,
	("dark", "grill+rain"): 2.05,
	("dark", "chance+rain"): 1.71,
	("dark", "grill+chance+rain"): 1.61,
}

# How far a measured value may drift before the gate calls it a
# regression. Two hundredths: smaller than any real palette change and
# larger than the rounding in the table above.
WASH_SLACK = 0.02


def channel(value):
	value /= 255.0
	return value / 12.92 if value <= 0.03928 else ((value + 0.055) / 1.055) ** 2.4


def luminance(colour):
	return (0.2126 * channel(colour[0]) + 0.7152 * channel(colour[1]) +
	        0.0722 * channel(colour[2]))


def over(top, alpha, ground):
	"""`top` at `alpha` composited onto `ground`, as the painter does."""
	part = alpha / 255.0
	return tuple(round(part * top[at] + (1.0 - part) * ground[at])
	             for at in range(3))


def contrast(ink, ground):
	high = max(luminance(ink), luminance(ground))
	low = min(luminance(ink), luminance(ground))
	return (high + 0.05) / (low + 0.05)




def palettes(text):
	"""The light palette is the base; dark overrides part of it."""
	text = cxx_text.without_comments(text)
	start = text.index("bbq_graph_palette palette_for(")
	split = text.index("if (scheme != Qt::ColorScheme::Dark) {", start)
	end = text.index("\n}\n", split)

	found = re.compile(
	        r"chosen\.([a-z_]+)\s*=\s*QColor\(\s*0x([0-9a-fA-F]{2})\s*,\s*"
	        r"0x([0-9a-fA-F]{2})\s*,\s*0x([0-9a-fA-F]{2})"
	        r"(?:\s*,\s*(\d{1,3}))?")

	def read(block):
		colours = {}
		alpha = {}
		for name, r, g, b, a in found.findall(block):
			colours[name] = (int(r, 16), int(g, 16), int(b, 16))
			alpha[name] = int(a) if a else 255
		return colours, alpha

	light, light_alpha = read(text[start:split])
	dark_only, dark_only_alpha = read(text[split:end])

	dark = dict(light)
	dark.update(dark_only)
	dark_alpha = dict(light_alpha)
	dark_alpha.update(dark_only_alpha)

	return light, dark, {"light": light_alpha, "dark": dark_alpha}


def control_passes():
	"""Black on white must pass and white on white must not.

	A contrast check whose arithmetic is wrong reports a clean palette
	just as loudly as a correct one, and this file's whole failure mode
	is silence.
	"""
	white = (0xff, 0xff, 0xff)
	black = (0x00, 0x00, 0x00)
	return contrast(black, white) > 20.0 and contrast(white, white) < 1.01


def main():
	if not cxx_text.self_check():
		print("palette: the comment stripper is broken, so a "
		      "commented-out colour would still be checked",
		      file=sys.stderr)
		return 2

	if not control_passes():
		print("palette: the control failed, so no result below means "
		      "anything", file=sys.stderr)
		return 2

	if not SOURCE.is_file():
		print(f"palette: {SOURCE} is missing", file=sys.stderr)
		return 2

	light, dark, alphas = palettes(SOURCE.read_text(encoding="utf-8"))

	if not light or "background" not in light:
		print("palette: no colours parsed -- the pattern has stopped "
		      "matching, which is not the same as a palette with no "
		      "colours", file=sys.stderr)
		return 2

	bad = 0

	held = 0
	clear = 0

	for scheme, palette in (("light", light), ("dark", dark)):
		for _name, key, _alpha in WASHES:
			if key not in palette:
				print(f"palette: {scheme} has no colour {key!r}",
				      file=sys.stderr)
				return 2

		for ink, _why in WASHED_PAIRS:
			if ink not in palette:
				print(f"palette: {scheme} sets no {ink!r}, so it is "
				      f"gone from the program or commented out",
				      file=sys.stderr)
				return 2

		for count in (1, 2, 3):
			for combo in itertools.combinations(WASHES, count):
				ground = palette["background"]
				for _name, key, alpha in combo:
					part = (alphas[scheme][key] if alpha is None
					        else alpha)
					ground = over(palette[key], part, ground)

				tag = "+".join(name for name, _k, _a in combo)
				worst = min((contrast(palette[ink], ground), ink)
				            for ink, _why in WASHED_PAIRS)

				recorded = WASH_WORST.get((scheme, tag))
				if recorded is None:
					print(f"palette: no recorded worst for {tag} in "
					      f"the {scheme} scheme", file=sys.stderr)
					bad += 1
					continue

				if worst[0] < recorded - WASH_SLACK:
					print(f"palette: {worst[1]} over {tag} is "
					      f"{worst[0]:.2f}:1 in the {scheme} scheme, "
					      f"worse than the recorded {recorded:.2f}:1",
					      file=sys.stderr)
					bad += 1
				elif recorded >= FLOOR:
					clear += 1
				else:
					held += 1


	for ink, ground, _why in PAIRS + [(a, b, "") for a, b in ALLOWED_UNDER]:
		for scheme, palette in (("light", light), ("dark", dark)):
			if ink not in palette or ground not in palette:
				print(f"palette: {scheme} has no colour {ink!r} or "
				      f"{ground!r}", file=sys.stderr)
				return 2

			ratio = contrast(palette[ink], palette[ground])
			if ratio >= FLOOR or (ink, ground) in ALLOWED_UNDER:
				continue

			print(f"palette: {ink} on {ground} is {ratio:.2f}:1 in the "
			      f"{scheme} scheme, under {FLOOR}:1", file=sys.stderr)
			bad += 1

	if bad:
		return 1

	print(f"palette: {len(PAIRS)} pair(s) on the plot clear {FLOOR}:1 "
	      f"in both schemes, {len(ALLOWED_UNDER)} allowed under by name; "
	      f"of {clear + held} wash combination(s), {clear} clear the "
	      f"floor and {held} are held at their measured worst")
	return 0


if __name__ == "__main__":
	sys.exit(main())
