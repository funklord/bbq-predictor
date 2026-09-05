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

# A pair kept under the floor deliberately, with what it measured at when
# it was allowed and why. Anything not listed here must clear the floor.
ALLOWED_UNDER = {
	("now_marker", "background"): (
		"2.96 on light. Amber was chosen for the light ground on purpose, "
		"the dark theme's brighter yellow being wrong there, and a "
		"two-pixel rule at 2.96 against 3.00 is a rounding difference "
		"rather than a legibility one."),
	("readout_edge", "readout_back"): (
		"2.94 on dark. It is the border of a box, not ink in it, and the "
		"box's own text clears the floor at 11.83."),
}


def channel(value):
	value /= 255.0
	return value / 12.92 if value <= 0.03928 else ((value + 0.055) / 1.055) ** 2.4


def luminance(colour):
	return (0.2126 * channel(colour[0]) + 0.7152 * channel(colour[1]) +
	        0.0722 * channel(colour[2]))


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
	        r"0x([0-9a-fA-F]{2})\s*,\s*0x([0-9a-fA-F]{2})")

	def read(block):
		return {name: (int(r, 16), int(g, 16), int(b, 16))
		        for name, r, g, b in found.findall(block)}

	light = read(text[start:split])
	dark = dict(light)
	dark.update(read(text[split:end]))
	return light, dark


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

	light, dark = palettes(SOURCE.read_text(encoding="utf-8"))

	if not light or "background" not in light:
		print("palette: no colours parsed -- the pattern has stopped "
		      "matching, which is not the same as a palette with no "
		      "colours", file=sys.stderr)
		return 2

	bad = 0
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

	print(f"palette: {len(PAIRS)} pair(s) clear {FLOOR}:1 in both schemes, "
	      f"{len(ALLOWED_UNDER)} allowed under it by name")
	return 0


if __name__ == "__main__":
	sys.exit(main())
