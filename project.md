# bbqpredictor

Read `~/.claude/CLAUDE.md` for prime directives.

This document is authoritative. Where it and the code disagree, the
document wins -- but the disagreement is raised rather than silently
resolved in either direction.

## 0. What this is

A C++/Qt Widgets weather applet, living in the system tray and in a
window.

**The single most important feature is the high-resolution
temperature and rain graphs from Weather Underground. Everything else is
secondary.** That sentence is the whole brief, and it is load-bearing:
where a decision trades graph quality against anything else, the graph
wins.

- App id: `se.vibes.bbqpredictor`
- Symbol prefix: `bbq_`
- Version: one place, the `VERSION` file

### 0.1 The name

`bbqpredictor` describes an ambition, not the current scope. Scoring the
forecast for grilling windows is **deferred, not planned** -- see sec 7.
It is named here so the next reader does not go looking for a feature
that was never built, and so nobody deletes the idea as a stray.

## 1. Toolkit

**Qt Widgets. Not QML, not QtQuick.** This is the global harmonization
rule, and it is also the right answer for a dense custom-painted graph.
Nothing in this tree requests `QT += quick`; that is deliberate, and this
line is the tripwire for the next person tempted to add it "for just one
screen".

Built against Qt 6.8 with qmake, wrapped by a hand-written top-level
Makefile -- the same split beerssh uses, and for the same reason: moc,
uic and rcc do not fit hand-written pattern rules, but everything else in
the tree is a plain Makefile and should look like one.

## 2. The data source

### 2.1 The problem

**Weather Underground has had no public API since the end of 2018.** This
matters more here than it usually would, because the WU graphs *are* the
product. The most important feature rests on the least certain
foundation, and pretending otherwise would be the wrong kind of quiet.

### 2.2 The decision

**The API key is scraped from wunderground.com's own page bundle.**

Chosen deliberately, with the alternatives on the table: a personal
weather station would earn a legitimate `api.weather.com` key, and
MET Norway or Open-Meteo would supply comparable resolution for free
without a key at all. Scraping was picked anyway. It is recorded here so
that it reads as a choice rather than as something nobody thought about.

**It violates Weather Underground's terms of service, and it will break.**
Not "may" -- the key rotates, the bundle gets reorganised, and the
extraction pattern is the first thing to go. Three consequences follow,
and each is a requirement rather than a nicety.

### 2.3 The key is runtime state, never a build constant

Extraction happens at startup, into a cache, with a re-extract path
triggered by the first 401. A key baked in at compile time turns the
application into a brick on whatever Tuesday WU rotates it, and the only
fix is a rebuild the user cannot perform.

### 2.4 Staleness is visible, always

This is the fragile joint in the whole program, and its failure mode is
not an error -- it is **a graph that keeps drawing yesterday's curve
while looking perfectly healthy.** A stale graph that looks fresh is
worse than no graph, because a decision gets made on it.

So: the last successful fetch time is part of the display, not hidden in
a tooltip, and a failed refresh is visible in the tray icon as well as in
the window. A silent fall back to cached data is a defect.

### 2.5 Be a polite client

A scraped key is somebody else's quota, and hammering is what gets a key
pattern noticed and closed. Cache to disk, survive a restart without
refetching, and poll no faster than the data behind the endpoint actually
updates -- which for an hourly forecast is not every thirty seconds. This
is self-interest as much as manners.

### 2.6 Open: which endpoints, exactly

Not yet settled, and it needs a session with the real traffic in front of
it rather than a guess written down here. What is known is the shape of
what is wanted (sec 3); which `api.weather.com` v3 paths supply each band
at what cadence is unverified, and **must not be written into this
document until it has been observed rather than assumed.**

### 2.7 Open: a fallback provider

Raised and not taken. A thin seam behind the fetch layer would cost
almost nothing now and is the difference between an afternoon and a
rewrite when the scrape breaks; the counter-argument is that an
abstraction built before its second implementation usually fits neither.

Recorded because it will come up again the first time the scrape fails,
and the answer should be a decision rather than a panic.

## 3. The graph is the program

The single hardest thing in this project, and it is a data problem before
it is a drawing problem.

**One continuous time axis carrying three sample rates from three
sources:**

| Band | Cadence | Relative to now |
|---|---|---|
| Observed / historical | station-dependent | behind |
| Nowcast precipitation | sub-hourly, 15 min or finer | next ~2 hours |
| Hourly forecast | hourly | out several days |

All four resolution axes were asked for explicitly, including visual
density as a goal in its own right -- the WU chart aesthetic, not merely
the underlying sample rate.

Two things follow:

- **The compositing model comes before any painting.** The joins between
  bands must not read as joins, which is a resampling and alignment
  question, not a pen-width question. This is the first real design work
  and it has not been done yet.
- **The forecast is a plain time series**, kept free of presentation
  concerns, so that something can later score over it (sec 7) without
  reaching into a widget.

### 3.1 Rendering: a prior, not a decision

The graph is expected to be a hand-painted `QWidget` with `QPainter`
rather than QtCharts. QtCharts is heavy, opinionated, and fights exactly
this case -- dense custom rendering over irregular sampling.

**This is a prior and not a finding.** It should be confirmed by trying
it, and this paragraph replaced with what was actually learned.

## 4. The tray

### 4.1 Open: which desktop

`QSystemTrayIcon` on Linux now means StatusNotifierItem. KDE Plasma
serves it natively; GNOME needs a shell extension, without which the tray
half of the brief is simply absent on that desktop.

**Unresolved, and it needs an answer before the tray work starts**, because
"applet for systray" is half the stated product and the answer decides
whether a fallback presentation is required.

## 5. Build

- `make` builds the app. It does not build tests.
- `make test` runs the suite -- **and there is no suite yet**, so it says
  so and fails, rather than reporting a pass over nothing.
- `make check` is what must pass before committing.
- `make style` gates indentation and the document against the tree.
- `make hooks` installs the commit-msg hook from `tools/hooks/`.
- `BUILD_DIR` is the build output directory, and is settable.
- `-Os`, per the global rule. qmake's release default is `-O2` and is
  applied by the *generated* Makefile, so `bbqpredictor.pro` is the only
  place saying otherwise takes effect.

### 5.1 Open: packaging

Not chosen. The global guidance prefers `dpkg-buildpackage` driving a
`dh`-based `debian/rules`, and notes that the seven private projects use
five different mechanisms between them -- so picking one here is a
convention change to raise, not a thing to do in passing.

## 6. Style

Three rules -- `snake_case`, tabs to indent and spaces to align,
lowercase filenames -- with the detail in `code-style.md` at the repo
root.

Qt's own API is called exactly as it is spelled (`setWindowTitle`,
`paintEvent`); names this project introduces stay `snake_case` with the
`bbq_` prefix where they reach the linker.

## 7. Deferred: the BBQ prediction

Scoring the forecast for good grilling windows. **Not being built now**,
and explicitly not designed around beyond sec 3's requirement that the
forecast stay a plain time series.

What makes a window good is unspecified and is the interesting part of
the feature -- temperature, rain, wind and time of day at minimum, with
weights nobody has stated. When it is picked up, that question is the
work.

## 8. Licence

**There is none, and that is not an oversight.** A licence is the
copyright holder's to choose and nobody else's, an absent one leaves
every option open, and no licence is better than a wrong one. Recorded
explicitly here so the absence does not read as a gap for a later pass to
close.

## 9. What has been decided, and what has not

Settled:

- Qt Widgets, Qt 6, qmake under a top-level Makefile (sec 1)
- The key is scraped, with its consequences as requirements (sec 2)
- Three bands on one time axis, forecast kept presentation-free (sec 3)
- BBQ scoring deferred (sec 7)
- No licence (sec 8)

Open, each needing a decision rather than a drift:

- Which WU endpoints supply which band (sec 2.6)
- Whether a fallback provider seam goes in now (sec 2.7)
- The compositing model itself -- the first real design work (sec 3)
- QPainter vs QtCharts, to be confirmed by trying (sec 3.1)
- Which desktop, and therefore whether the tray needs a fallback (sec 4.1)
- Packaging mechanism (sec 5.1)
