# src/wire/vendor

Not written here. Exempt from the style gate by the `vendor` component.

- `situ.h`, `situ.hpp`, `situ.c` -- situ's runtime, copied from situ at
  `b186326` (`runtime/c/` and `runtime/cpp/`), from the commit rather
  than a working tree.
- `bbq.hpp` -- generated from `../bbq.situ` by situ's compiler at the
  same commit:

      git -C ~/src/situ archive b186326 | tar -x -C <scratch>
      python3 <scratch>/bin/situc build --target cpp --out src/wire/vendor src/wire/bbq.situ

**Safe for downloaded files through the path the app uses.** At
`b186326` validate(), check() and the accessors refuse hostile lengths,
and framed() and required() no longer read out of bounds on the
truncated message reported to situ (inbox 20261022T1813Z; situ's
b4cf766). The applet still never calls those two: the one reproduction
is all that shows them safe, since neither `make fuzz-wire` nor situ's
own gen-fuzz harness calls them, and the guard in `fuzz-wire` stays.
`u8 x[remaining] max 0` after a dynamic member is not enforced at
`b186326`, so the reader keeps its exact-length checks. situ fixes that
after `b186326`, and says two framing faults remain in `framed()` and
`required()` (project.md sec 15.12.3), so both the checks and the
refusal stay when the pin next moves. Take a new commit only after
re-running `make fuzz-wire`, and regenerate all four files together,
never one.

Measured for `b186326` (2026-10-23): the three out-of-bounds
reproductions from inbox 20261022T1621Z clean under ASan and UBSan;
situ's gen-fuzz harness for `bbq.situ` 517,838 runs in 300 s, and its
old crash input, clean; `make fuzz-wire` 1,873,257 runs in 300 s, clean.
