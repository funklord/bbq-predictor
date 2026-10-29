# src/wire/vendor

Not written here. Exempt from the style gate by the `vendor` component.

- `situ.h`, `situ.hpp`, `situ.c` -- situ's runtime, copied from situ at
  `714ac3c` (`runtime/c/` and `runtime/cpp/`), from the commit rather
  than a working tree.
- `bbq.hpp` -- generated from `../bbq.situ` by situ's compiler at the
  same commit, given a situ checkout:

      git -C <situ checkout> archive 714ac3c | tar -x -C <scratch>
      python3 <scratch>/bin/situc build --target cpp --out src/wire/vendor src/wire/bbq.situ

**Safe for downloaded files through the path the app uses.** At
`217ba65` validate(), check() and the accessors refuse hostile lengths,
and check() enforces `u8 x[remaining] max 0` after a dynamic member,
which it did not at `b186326`. The reader keeps its own exact-length
checks too: they refuse padded varints, which no cap can.

framed() and required() no longer read out of bounds (situ's b4cf766),
three ways they reported a short message complete are fixed (b78119f),
a struct holding a nested variable struct frames right (situ's 26.684,
fixed in 1e81500 and c10c83a), and since `714ac3c` so does a counted
run of fixed-size structs. The applet still never calls those two, and
the guard in `fuzz-wire` stays.

`fuzz-wire` drives them, against the contract a reader would rely on:
complete means `need` is within the bytes given; truncated asks for
more than it has; the answer never goes backwards as bytes arrive; and
if the whole input validates, the `need` bytes do too. It was off by
default (`FUZZ_FRAMING=1`) while situ failed it, and each failure was
sent there with its input: `month_body` and `head_body` called a short
message complete at `601fa96` (situ's inbox, 20261026T1249Z and
20261026T1317Z); 97d1343 fixed that and read a short `stations_body`
out of bounds (20261028T2255Z); 1605cd8 fixed that and still called a
two-entry `index_body` complete one entry short, as 601fa96 also did
(20261029T0315Z). None of those was taken. 714ac3c passes, so the case
is on by default now. Take a new commit only after re-running
`make fuzz-wire`, and regenerate all four files together, never one.

Measured for `714ac3c` (2026-10-29): the runtime files byte-identical
to `601fa96`'s. `bbq.hpp` differs outside framed() and required() only
in spelling -- `::situ::` before a struct name, `this->`, `raw()`
called as `rt::view::raw()`, and `band_` for `band`; with those
normalised and both functions cut from both files, the remaining 5104
lines are identical. The index_body input of 20261029T0315Z traps the
framing case at `601fa96` and passes at `714ac3c`. With the framing
case, 3,679,437 runs in 301 s, clean; without it, 1,787,207 runs in
301 s, clean. `make check` passing.

Measured for `601fa96` (2026-10-26): the runtime files byte-identical
to `217ba65`'s. `bbq.hpp` differs only inside framed() and required():
with the 32 of each cut from both files, the remaining 4925 lines are
identical, so nothing the reader calls changed. `make check` passing;
`make fuzz-wire` 1,585,458 runs in 300 s, clean; situ's gen-fuzz
harness for `bbq.situ`, built with `-DSITU_FUZZ` under ASan and UBSan,
777,003 runs in 300 s, clean.

Measured for `217ba65` (2026-10-24): the runtime files byte-identical
to `b186326`'s, and `bbq.hpp` changed only in framed(), required() and
check(); `make check` passing; with the reader's columns length check
disabled, test_wire's trailing-byte case passing at `217ba65` and
failing at `b186326`, so check() refuses on its own; `make fuzz-wire`
2,542,663 runs in 300 s, clean; situ's gen-fuzz harness for `bbq.situ`
507,949 runs in 300 s and the old crash input, clean.

For `b186326` (2026-10-23): the three out-of-bounds reproductions from
inbox 20261022T1621Z clean under ASan and UBSan; gen-fuzz 517,838 runs
in 300 s; `make fuzz-wire` 1,873,257 runs in 300 s.
