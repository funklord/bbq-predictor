# bbq-predictor

Whether an afternoon is good for anything outdoors is decided by the
next few hours at one place, and that is the part weather software
answers worst. A forecast is a model's guess on a grid kilometres
wide, refreshed every hour or so; a radar extrapolation is better for
the next two hours and says nothing about temperature; and the one
measurement of what is really happening -- a personal weather station
down the street -- sits on a different website from either. Each
source is shown in its own app, as hourly icons or a smoothed line,
and none of them says how often its forecasts have been right where
you are.

bbq-predictor puts all of it on one graph: the station's own readings
up to now, then radar for the next two hours, then the quarter-hourly
and hourly forecasts, on one continuous time axis at the resolution
each source actually has, with a strip underneath saying which source
drew each stretch. It keeps every observation it ever fetched, checks
every forecast against what the station then measured, and uses the
record to correct the forecast and to say how far to trust it. It sits
in the system tray as the temperature, opens to the graph, and runs on
Android with a home-screen widget. The name is the question it was
written to answer: whether this afternoon is worth lighting a fire for.

Against what people use for the same question -- Weather
Underground's own site and app, yr.no, meteoblue, and the weather
applets that come with GNOME and KDE:

- **Several sources on one axis, each named.** Weather Underground
  draws its own forecast and yr.no draws MET Norway's; here a station,
  MET Norway's five-minute radar, Weather Underground's quarter-hourly
  and hourly forecasts and Open-Meteo's sixteen days are one curve,
  resolved by a declared priority so that a measurement always beats a
  forecast of the same minute, and the source of every stretch is drawn
  under it.
- **Drawn at the data's own resolution, honestly.** Rain is reduced by
  its maximum when zoomed out, so a five-minute downpour is not
  averaged into a dry hour; a gap in the data is drawn as a gap, never
  joined across; zoomed out far enough that a column holds a whole day,
  the day's range is drawn behind the mean. The real samples can be
  marked, so a smoothed line never has to be taken on trust, and the
  curve is one of seven interpolation methods, from steps to Akima.
- **A history that is kept.** Every observation goes into a SQLite
  archive for good, and the graph scrolls and zooms through it, from a
  quarter of an hour across the window to years. A desktop weather
  applet keeps nothing; Weather Underground keeps a station's history,
  but on its own pages, apart from any forecast.
- **Forecasts that are marked.** Each forecast is held until the
  weather it predicted has happened, then scored against the station
  and discarded, leaving the bias, mean absolute error and RMSE of each
  source at each lead time. That record draws a bias-corrected line
  beside the forecast, so the difference between what was said and what
  this place's history suggests is on the graph rather than asserted.
  None of the sites above scores its forecasts against the station you
  watch.
- **An answer to the question.** The forecast is scored for
  temperature, rain rate, rain chance, wind and time of day, the
  stretches worth using are shaded, and the best is named in words
  above the graph -- or "no grilling window in the next three days",
  rather than a mediocre one invented to fill the space.
- **Where you look anyway.** The tray icon is the current temperature
  and turns red when the data behind it is stale; on a phone the
  widget draws the graph and says how old the picture is.

## Status

**It works, and it is one person's applet rather than a product.**

Fetching, parsing, drawing, scoring, verification, configuration and
the tray are built and in use, on the desktop and on Android. The test
suite covers the model, the readers, the curves, the layouts and the
graph's gestures.

The server half -- a publisher that fetches for a station and writes
signed files, and a daemon that serves them, so clients read published
data rather than each scraping on their own -- is built and packaged.
The applet reads it first and scrapes only when it is gone, once it is
built with the publisher's public key.

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

Needs Qt 6 -- widgets, network, sql and positioning -- and a C++17
compiler. On Debian:

    sudo apt-get install -y qt6-base-dev qt6-base-dev-tools \
        qt6-positioning-dev libgl-dev pkg-config libqt6sql6-sqlite

The last of those is a **run time** dependency rather than a build one:
the SQLite driver is loaded rather than linked, so it appears in no ELF
header, nothing in a successful build mentions it, and without it the
archive cannot be opened at all. `debian/control` names it under
`Depends` for the same reason.

This line said "widgets and network" until CI needed the list written
down and the two disagreed.

    make            # build
    make run        # build and run
    make check      # style and tests: what must pass before a commit
    make help       # every target, with what it does

Or with [fmake](../fmake), which needs no build file and nothing beyond the
Python standard library:

    python3 ~/src/fmake/fmake    # build, same sources, same Qt

**Not `/usr/bin/fmake`.** The packaged one predates `$file()` and leaves the
reference in the value as text, so the build succeeds and the binary
carries the literal string. Measured: `--version` printing
`$file(VERSION)`. Run the one in fmake's own tree until a newer package is
installed.

`fmake.toml` says two things it cannot read off the tree: the version, from
the `VERSION` file the Makefile and `debian/changelog` already agree about
rather than written a third time; and that `build-*` holds qmake's own
output, which carries a `moc_*.cpp` for every class that fmake also mocs,
so a tree anybody has built in offers two of each metaobject.

The build is `-Os`; `make DEBUG=1` gives an unoptimised, symbol-rich
one, and `make SANITIZE=1` adds ASan and UBSan independently of it.
`make DEBUG_INFO=1` is a third and different thing: still `-Os`, still a
release build, but carrying debug info, which is what the package build
uses so its `-dbgsym` has something in it.
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

## Installing it

    make install                  # binary, desktop entry, icon, manual page
    man bbq-predictor             # every option, not just what --help lists

Or as Debian packages:

    make deb                      # lands in build/deb, runs the suite first
    sudo apt-get install -y ./build/deb/bbq-predictor_*.deb

## Running the server

`bbq-predictor-server` is what vibes.se runs, and needs no display: the
publisher, `bbq-predictor-publish`, is built on Qt's core, network and
SQL libraries and nothing that draws. A five-minute round (a systemd
timer, or cron where systemd is not running) fetches every band and
writes the station's signed files; `bbq-predictor-serve` serves them.

    sudo apt-get install -y ./build/deb/bbq-predictor-server_*.deb
    sudo -u bbq-predictor bbq-predictor-publish \
            --generate-signing-key /var/lib/bbq-predictor/signing.key

The public key that prints is what the applet is built with. Then name
the station, and start the daemon once the first round has published:

    # /etc/default/bbq-predictor
    BBQ_STATION=ISTOCK877

Unset, the round **skips** itself and says why rather than fetching for
a station nobody named. It runs as the `bbq-predictor` account, and the
archive it builds is deliberately readable.

## On a phone

`make android` builds an APK and `make android-install` puts it on an
attached device -- `make android-check` says what is missing first, by
name, rather than failing halfway through Gradle.

There is a home-screen widget. Add it from the launcher's widget picker;
it draws the graph the applet last rendered and **says how old that
picture is** once it is more than twenty minutes stale, because a
forecast that looks current and is six hours old is worse than no widget
at all. It only changes while the applet runs: there is no background
fetch, and the age is stated rather than the staleness hidden.

## Contributing

`code-style.md` at the repo root has the rules: `snake_case`, tabs to
indent and spaces to align, lowercase filenames. `make style` runs the
gates: indentation, `project.md` against the tree, every signal having a
listener, the manual page against the options the program accepts,
palette contrast in both schemes and over every wash the plot lays down,
the fetch exit codes against the systemd unit that forgives one of them,
every XML file parsing, every header and test project being named where
the build can see it, and the handful of setup calls whose absence
nothing else would notice. `make check` adds the tests, and a compile of
the Android-only code (`ANDROID_COMPILE=no` skips it where there is no
Android kit).

No number here on purpose. It said "six" while the Makefile ran eight,
because two were added without this line being touched -- a count in
prose is a fact about the tree that nothing re-derives.

    make hooks      # install the commit-msg hook

## Copyright

    Copyright (C) 2026 Nabeel Sowan <nabeel@vibes.se>
