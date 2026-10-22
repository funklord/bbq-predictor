# src/wire/vendor

Not written here. Exempt from the style gate by the `vendor` component.

- `situ.h`, `situ.hpp`, `situ.c` -- situ's runtime, copied from situ at
  `56dcad1` (`runtime/c/` and `runtime/cpp/`), from the commit rather
  than a working tree.
- `bbq.hpp` -- generated from `../bbq.situ` by situ's compiler at the
  same commit:

      git -C ~/src/situ archive 56dcad1 | tar -x -C <scratch>
      python3 <scratch>/bin/situc build --target cpp --out src/wire/vendor src/wire/bbq.situ

**Safe for downloaded files only through the path the app uses.** At
`56dcad1` validate(), check() and the accessors refuse hostile lengths
(situ's fix for bbq-predictor's report), but framed() and required()
still read out of bounds on a truncated message (reported back to situ,
inbox 20261022T1813Z). The applet parses whole, size-capped files and
never calls either; `make fuzz-wire` is what holds it to that. Take a
new commit only after re-running that, and regenerate all four files
together, never one.
