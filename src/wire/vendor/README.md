# src/wire/vendor

Not written here. Exempt from the style gate by the `vendor` component.

- `situ.h`, `situ.hpp`, `situ.c` -- situ's runtime, copied from situ at
  `f9fa599` (`runtime/c/` and `runtime/cpp/`), from the commit rather
  than a working tree.
- `bbq.hpp` -- generated from `../bbq.situ` by situ's compiler at the
  same commit:

      python3 ~/src/situ/bin/situc build --target cpp --out src/wire/vendor src/wire/bbq.situ

**Used only by the suite, on files the suite built itself.** At
`f9fa599` the generated C++ reads out of bounds on hostile input
(project.md sec 15.12.3), so the applet must not parse downloaded files
with it until situ's fix is taken and this directory regenerated from
the commit that has it. Regenerate all four together, never one.
