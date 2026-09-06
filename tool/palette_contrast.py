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

import re
import sys
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

# The plot has more than one ground, and for a long time this gate knew
# about one of them (project.md sec 16.28). Everything above is drawn
# over the grill window's shading too, wherever a good grilling window
# falls, and that tint is the DARKEST ground in the light scheme and the
# warmest in the dark one -- so a colour cleared against `background`
# has been cleared against the easier case.
#
# Named separately rather than folded in, because it is not a palette
# entry: it is grill_window composited over background at the alpha the
# graph actually uses.
OVER_GRILL = "over_grill"

# The cap is the grill colour's OWN alpha, read from the palette, so
# this gate and the painter cannot disagree about it. It used to be a
# literal 80 here copied from a literal 80 there -- two copies of one
# number, which is one more than can be kept true (sec 16.29).

GRILL_PAIRS = [
	("axis_text", "the scale numbers, where a window shades them"),
	("temperature", "the forecast curve, inside a grilling window"),
	("corrected", "the bias-corrected overlay, inside one"),
	("day_divider", "the midnight rule crossing one"),
	("stale_warning", "said over one"),
	("now_marker", "the now line crossing one"),
]

# A pair kept under the floor deliberately, with what it measured at when
# it was allowed and why. Anything not listed here must clear the floor.
ALLOWED_UNDER = {
	("readout_edge", "readout_back"): (
		"2.94 on dark. It is the border of a box, not ink in it, and the "
		"box's own text clears the floor at 11.83."),
}

# Nothing is allowed under the floor on the grill ground. The three that
# were -- temperature and stale_warning at 1.89, corrected at 2.24, all
# in the dark scheme -- were fixed rather than waived by dropping the
# dark wash from alpha 80 to 24 and moving the window's signal onto its
# edges (sec 16.29). An empty table is kept rather than deleted so that
# adding an entry is a visible act.
GRILL_ALLOWED_UNDER = {}


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

	for ink, _why in GRILL_PAIRS:
		for scheme, palette in (("light", light), ("dark", dark)):
			if ink not in palette or "grill_window" not in palette:
				print(f"palette: {scheme} has no colour {ink!r} or "
				      f"'grill_window'", file=sys.stderr)
				return 2

			ground = over(palette["grill_window"],
			              alphas[scheme]["grill_window"],
			              palette["background"])
			ratio = contrast(palette[ink], ground)
			if ratio >= FLOOR or (ink, scheme) in GRILL_ALLOWED_UNDER:
				continue

			print(f"palette: {ink} on {OVER_GRILL} is {ratio:.2f}:1 in "
			      f"the {scheme} scheme, under {FLOOR}:1", file=sys.stderr)
			bad += 1

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

	print(f"palette: {len(PAIRS)} pair(s) on the plot and "
	      f"{len(GRILL_PAIRS)} over the grill window clear {FLOOR}:1 in "
	      f"both schemes, {len(ALLOWED_UNDER) + len(GRILL_ALLOWED_UNDER)} "
	      f"allowed under it by name")
	return 0


if __name__ == "__main__":
	sys.exit(main())
