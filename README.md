# bbq-predictor

A Qt Widgets weather applet for the system tray and the desktop, built
around high-resolution temperature and rain graphs -- and, since that is
what it is named after, an answer to whether this afternoon is worth
lighting a fire for.

The graphs are the point. Everything else is secondary.

## What it does

- **One graph, several sources.** Observed station readings, a
  five-minute radar nowcast, a quarter-hourly forecast and an hourly one
  are drawn on a single continuous time axis, with a strip underneath
  saying which band produced each stretch of it.
- **Grilling windows.** The forecast is scored for temperature, rain
  rate, rain chance, wind and time of day, and the stretches worth
  using are shaded, with the best named above the graph.
- **Curves you choose.** Seven interpolation methods, from step to
  Akima to a natural cubic, plus a rounding radius -- with the real
  samples markable, so a smoothed line never has to be taken on trust.
- **A permanent history.** Every observation is kept for ever in a
  SQLite archive; drag the graph sideways to scroll back through it and
  use the wheel to zoom, from a quarter of an hour to years.
- **Forecasts that get marked.** A forecast is held only until the
  weather it predicted actually happens, then scored against it and
  discarded -- leaving the bias, MAE and RMSE for each band at each lead
  time, in the vocabulary forecast verification already uses.
- **A tray that says something.** The icon is the current temperature
  and turns red when the data behind it is stale.
- **Desktop and mobile shapes**, chosen by the device and overridable.

## Status

**It works, and it is one person's applet rather than a product.**

Fetching, parsing, drawing, scoring, configuration and the tray are all
built and in use. The test suite covers the model, the readers, the
curves and the layouts.

The Android build is wired and does not currently complete: Qt's own
AndroidX dependencies require a newer `compileSdk` than the SDK it was
tried against. See `project.md` sec 11.2.

`project.md` is the authoritative record -- what was decided, why, what
was measured, and what is still open. **Read it before the code.** It is
long because the reasoning is the useful part.

## Data sources, and a warning

**Weather Underground's public API was discontinued at the end of 2018,
and this project reaches it with a key scraped from wunderground.com's
own page bundle. That violates their terms of service, and it will
break.**

It was chosen deliberately, with the alternatives on the table, and the
reasoning is in `project.md` sec 2. If you are looking for something to
depend on, this is not that.

The other two providers need no key and break no terms:

| Band | Source | Cadence | Reach |
|---|---|---|---|
| Observed | a Weather Underground station | ~5 min | one day |
| Current | that station's latest reading | now | -- |
| Radar | MET Norway | 5 min | ~2 h |
| Nowcast | Weather Underground | 15 min | 7 h |
| Extended | Open-Meteo | 60 min | 16 days |
| Hourly | Weather Underground | 60 min | 15 days |

Bands are resolved by a declared priority, so a measurement always beats
a forecast and a gap is drawn as a gap rather than interpolated across.

## Configuring it

Settings live in `~/.config/bbq-predictor/bbq-predictor.ini`; the
history is a separate SQLite file under
`~/.local/share/bbq-predictor/`, because one is a preference you edit
and the other is measurement. The one
setting it cannot work without is the station:

    [General]
    station=ISTOCK822

It can also be typed into the Station field in the window, which writes
it there. Find a station id on Weather Underground's map, or leave it
empty and set `geocode_override=59.33,18.07` to run on forecasts alone.

The command line overrides a run without writing to the file:

    ./bbq-predictor --station ISTOCK822
    ./bbq-predictor --geocode 59.33,18.07

## Building

Needs Qt 6 (widgets and network) and a C++17 compiler.

    make            # build
    make run        # build and run
    make check      # style and tests: what must pass before a commit
    make help       # every target, with what it does

The build is `-Os`; `make DEBUG=1` gives an unoptimised, symbol-rich
one, and `make SANITIZE=1` adds ASan and UBSan independently of it.
Build output goes to `build/` and the location is settable:

    make BUILD_DIR=/tmp/bbq-asan SANITIZE=1

An isolated build stays in its own directory: only the default `build/`
is copied to `./bbq-predictor`, so a sanitized or cross build cannot
quietly replace the ordinary binary.

Android needs OpenSSL built for the device first -- Qt ships none, and
without it the applet can fetch nothing at all:

    ANDROID_NDK_ROOT=$HOME/Android/Sdk/ndk/27.2.12479018 \
        tool/build-openssl-android.sh arm64-v8a

The source comes from `apt-get source openssl`, so it is the version the
distribution pinned and patched rather than one this repository chose.

Then point the build at a Qt kit and an NDK:

    make android QT_ANDROID_ROOT=$HOME/Qt/6.12.0/android_arm64_v8a \
                 ANDROID_NDK_ROOT=$HOME/Android/Sdk/ndk/27.2.12479018

The Qt version is whichever is installed; the NDK is not a free choice.
It must match the one the kit names in its own `mkspecs/qdevice.pri`, and
`make android-check` compares them -- a mismatch builds and installs
perfectly and then fails to load on the device.

## Looking at it

Every layout defect in this project was found by rendering a picture and
looking at it, so the diagnostics are part of the build rather than
scratch work. All of them run headless.

    ./bbq-predictor --fetch-once              # every band, with holes and probes
    ./bbq-predictor --shot out.png            # the window
    ./bbq-predictor --shot out.png --layout mobile
    ./bbq-predictor --tray-icon icon.png      # the tray icon, which cannot be grabbed
    ./bbq-predictor --interp akima --smooth 1800 --shot out.png
    ./bbq-predictor --view 10800 --shot out.png   # a three-hour zoom
    ./bbq-predictor --shot out.png --layout mobile --size 320x700
    ./bbq-predictor --history                     # what the archive holds
    ./bbq-predictor --history-path /tmp/x.sqlite  # against a scratch archive
    ./bbq-predictor --seed-verification 0.6 --history-path /tmp/x.sqlite

## Contributing

`code-style.md` at the repo root has the rules: `snake_case`, tabs to
indent and spaces to align, lowercase filenames. `make style` checks
what can be checked mechanically, and `make check` adds the tests.

    make hooks      # install the commit-msg hook

## Licence

None. That is deliberate rather than an oversight -- see `project.md`
sec 8. No rights are granted, and the choice belongs to the copyright
holder.
