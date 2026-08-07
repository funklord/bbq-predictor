# bbqpredictor

A Qt Widgets weather applet for the system tray and the desktop, built
around high-resolution temperature and rain graphs.

The graphs are the point. Everything else is secondary.

## Status

**Early.** The tree currently holds a build system, the style tooling, and
a window-plus-tray skeleton that compiles and runs. There is no weather in
it yet -- no fetching, no parsing, no graph beyond a placeholder that says
so on screen.

`project.md` is the authoritative record of what this is meant to be, what
has been decided, and what is still open. Read it before the code.

## Building

Needs Qt 6 (widgets and network) and a C++17 compiler.

    make            # build
    make run        # build and run
    make check      # what must pass before a commit
    make help       # every target, with what it does

The build is `-Os`; `make DEBUG=1` gives an unoptimised, symbol-rich one,
and `make SANITIZE=1` adds ASan and UBSan independently of it.

Build output goes to `build/` by default and the location is settable:

    make BUILD_DIR=/tmp/bbq-asan SANITIZE=1

## Data sources, and a warning

Weather Underground comes first -- it is the reason the project exists --
but it is the first provider rather than the only one. Anything that can
supply data at the resolution the graphs need is a candidate: sub-hourly
precipitation, hourly temperature out several days, and observed history.
Resolution is the criterion, not convenience, and a three-hourly forecast
would make the graph worse rather than better.

Weather Underground's public API was discontinued at the end of 2018, and
this project reaches it with a key scraped from wunderground.com's own
page bundle.

**That violates Weather Underground's terms of service, and it will
break.** It was chosen deliberately over the alternatives, and the
reasoning -- along with the requirements that follow from it -- is in
`project.md` sec 2. If you are looking for something to depend on, this is
not that; the other providers, when they land, will be the clean path.

## Contributing

`code-style.md` at the repo root has the rules: `snake_case`, tabs to
indent and spaces to align, lowercase filenames. `make style` checks what
can be checked mechanically.

    make hooks      # install the commit-msg hook

## Licence

None. That is deliberate rather than an oversight -- see `project.md`
sec 8. No rights are granted, and the choice belongs to the copyright
holder.
