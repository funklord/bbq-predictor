#!/usr/bin/env python3
"""Every Qt signal this project declares must have something listening.

A signal with no connection is not a compile error, not a warning, and not
a test failure: the emit runs, nothing happens, and the feature it was
meant to drive is simply absent. This project has now met that twice --
`verified` was emitted for a consumer nobody ever wrote, and the scoring
sweep it announced ran for one station out of three.

Counting listeners is the cheapest instrument for that class, and it is
the one that needs no sibling to compare against: zero is wrong on its
own terms. It answers a structural question -- is this wired -- where a
behaviour test can only ask whether the common path happens to work,
which it often does for a reason other than the wiring.

Exits 1 when a signal has no listener, so `make style` fails.
"""

import re
import sys
from pathlib import Path

# A signal deliberately left for an outside consumer goes here, with the
# reason. An empty list is the honest state today: every signal this
# project declares is connected inside it.
ALLOWED_WITHOUT_LISTENER = {}

SIGNAL_BLOCK = re.compile(
        r'^signals:(.*?)(?=^\s*(?:public|private|protected|signals)\b|^\};)',
        re.M | re.S)
DECLARATION = re.compile(r'^(?:void\s+)?(\w+)\s*\(')


def declared_signals(root):
	"""Map each signal name to the header that declares it."""
	found = {}
	for header in sorted(root.rglob('*.h')):
		text = header.read_text(encoding='utf-8')
		for block in SIGNAL_BLOCK.findall(text):
			for line in block.split('\n'):
				line = line.strip()
				if not line or line.startswith(('/', '*', '#')):
					continue
				match = DECLARATION.match(line)
				if match:
					found.setdefault(match.group(1), str(header))
	return found


def main():
	root = Path(__file__).resolve().parent.parent
	signals = declared_signals(root / 'src')
	if not signals:
		print('signal-gate: no signals found, so nothing was checked',
		      file=sys.stderr)
		return 2

	body = []
	for where in ('src', 'test'):
		for path in sorted((root / where).rglob('*')):
			if path.suffix in ('.cpp', '.h'):
				body.append(path.read_text(encoding='utf-8'))
	body = '\n'.join(body)

	silent = []
	for name, header in sorted(signals.items()):
		if name in ALLOWED_WITHOUT_LISTENER:
			continue
		# `&class::name` is how a connection names a signal, and it is
		# how a slot names one too -- either is a consumer.
		if not re.search(r'&\w+::' + re.escape(name) + r'\b', body):
			silent.append((name, header))

	for name, header in silent:
		print(f'{header}: signal `{name}` has nothing listening')

	print(f'signal-gate: {len(signals)} signal(s), {len(silent)} unheard')
	return 1 if silent else 0


if __name__ == '__main__':
	sys.exit(main())
