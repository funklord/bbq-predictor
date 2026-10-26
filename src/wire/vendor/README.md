# src/wire/vendor

Not written here. Exempt from the style gate by the `vendor` component.

- `situ.h`, `situ.hpp`, `situ.c` -- situ's runtime, copied from situ at
  `601fa96` (`runtime/c/` and `runtime/cpp/`), from the commit rather
  than a working tree.
- `bbq.hpp` -- generated from `../bbq.situ` by situ's compiler at the
  same commit, given a situ checkout:

      git -C <situ checkout> archive 601fa96 | tar -x -C <scratch>
      python3 <scratch>/bin/situc build --target cpp --out src/wire/vendor src/wire/bbq.situ

**Safe for downloaded files through the path the app uses.** At
`217ba65` validate(), check() and the accessors refuse hostile lengths,
and check() enforces `u8 x[remaining] max 0` after a dynamic member,
which it did not at `b186326`. The reader keeps its own exact-length
checks too: they refuse padded varints, which no cap can.

framed() and required() no longer read out of bounds (situ's b4cf766),
three ways they reported a short message complete are fixed (b78119f),
and since `601fa96` a struct holding a nested variable struct frames
right too (situ's 26.684, fixed in 1e81500 and c10c83a). The applet
still never calls those two, and the guard in `fuzz-wire` stays.

`make fuzz-wire FUZZ_FRAMING=1` drives them, against the contract a
reader would rely on. Complete means `need` is within the bytes given,
and those `need` bytes alone answer the same; truncated asks for more
than it has; and if the whole input validates, the `need` bytes do too.
At `601fa96` it broke the second inside five minutes: given 95 bytes,
`month_body::required()` said complete at 82, and given those 82 it
said truncated and asked for 87 (situ's inbox, 20261026T1249Z, with the
input). With month_body left out, `head_body` did the same on 13 bytes:
complete at 10, then truncated asking for 11 (20261026T1317Z). So it stays off by default, and the guard stays, until a situ
commit passes it. Take a new commit
only after re-running `make fuzz-wire`, and regenerate all four files
together, never one.

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
