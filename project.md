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

## 2. The data sources

Plural, deliberately. Weather Underground comes first and is the reason
the project exists, but it is **the first provider rather than the only
one** -- see sec 2.7.

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

### 2.6 The endpoints, as observed

**Observed on 2026-08-07 against the live service, not read off a
document.** Every row below was fetched and its response inspected; the
cadences, field names and orderings are what came back, not what an API
reference claims. This is unofficial and undocumented, so it will drift
-- re-observe rather than trusting this table's age.

| Band | Endpoint | Cadence | Span |
|---|---|---|---|
| Observed, fine | `/v2/pws/history/all` | **5 min** (288 rows/day) | one day per request |
| Observed, hourly | `/v2/pws/history/hourly` | 60 min (24 rows/day) | one day per request |
| Observed, now | `/v2/pws/observations/current` | latest | -- |
| Nowcast | `/v3/wx/forecast/fifteenminute` | **15 min** (28 points) | 7 hours |
| Hourly | `/v3/wx/forecast/hourly/15day` | 60 min (360 points) | 15 days |
| Daily | `/v3/wx/forecast/daily/{3,5,10}day` | daily | 3-10 days |
| Historical, ICAO | `/v3/wx/conditions/historical/hourly/1day` | 60 min, **descending** | 24 hours |

All take `apiKey`, plus `geocode=lat,lon` (v3 forecast), `stationId` (v2
PWS) or `icaoCode` (v3 historical), with `units=m&language=en-US&format=json`.

### 2.6.1 Extracting the key

A 32-character lowercase hex string. The page carries it in two distinct
forms, and **the difference decides whether extraction works:**

- A **config blob with several named keys** -- `SUN_API_KEY`,
  `WX_API_KEY` and others. These have *different scopes*. The weather
  endpoints above answer to `SUN_API_KEY`; another key from the same blob
  is a plausible-looking 32-hex string that fails on the endpoints
  actually wanted.
- **Fully-formed request URLs** embedded in the server-rendered payload,
  as `https://api.weather.com/v3/...?apiKey=<32 hex>&...`.

**Extract from the request URLs, not the config blob.** A key harvested
from a URL is one the site itself just used successfully against that
endpoint family, which is evidence; a key harvested by name from a
config object is a guess that the name means what it looks like. Match
`apiKey=([0-9a-f]{32})` and prefer a hit whose URL path matches the
endpoint about to be called.

### 2.6.2 The three traps

Each of these was found by inspecting real responses, and each would
produce a *plausible wrong graph* rather than an error.

- **Time is not represented the same way.** `hourly` and the historical
  endpoints carry `validTimeUtc`; **`fifteenminute` carries only
  `validTimeLocal`**, an ISO 8601 string with an offset. The band with
  the finest resolution is the one with no epoch field.
- **Order is not the same.** `conditions/historical/hourly/1day` comes
  back **newest-first**; the forecast series ascend. Plotted unreversed
  it draws a mirror image of the last 24 hours, which looks like weather.
- **Rain is a different quantity per band.** `fifteenminute` gives
  `precipRate` (a rate), `hourly` gives `qpf` (accumulation over the
  step), and `conditions/historical` gives `precip24Hour` (a running
  24-hour total, not a per-step value at all). Drawing these on one rain
  axis without converting is comparing three different measurements.

### 2.6.3 Two response shapes, so two parsers

The v3 endpoints are **column-oriented**: parallel arrays of equal
length, one per field, indexed positionally. The v2 PWS endpoints are
**row-oriented**: an `observations` array of objects, each nesting its
values under a unit-system key (`metric`, `imperial`).

They are not variations on a theme, and code that tries to be both is
worse than two small readers.

### 2.6.4 The nowcast is the least safe of them

`/v3/wx/forecast/fifteenminute` answers with the scraped key, and it is
the only source found so far for the sub-hourly band the brief calls for.

**It was not called by either wunderground.com page observed** -- the
forecast page or the map. That is not proof it is never used anywhere on
the site, but it does mean the endpoint most important to this project
is one the site does not visibly depend on. An endpoint the site does
not exercise can be withdrawn or re-scoped without the site breaking,
which is exactly the failure nobody would see coming.

**Sec 2.7 is therefore not a hedge, it is the plan.** Another provider
supplying sub-hourly precipitation is the insurance on the single most
important band, and it should not wait for the day this stops answering.

### 2.7 More than one provider, with Weather Underground first

**Settled: the fetch layer is multi-provider from the start.**

This was previously open, and the open version framed it as a *fallback*
-- WU-shaped code with a seam bolted on for the day the scrape breaks.
That framing was wrong and is recorded here so it does not come back.
What is wanted is a program that draws these graphs from whoever can
supply them, with WU first among several rather than the one true source
with an escape hatch.

The difference is not academic. A fallback seam gets the WU response
shape baked into everything above it, and every later provider is
translated into WU's vocabulary whether or not that fits. Designing for
several from the beginning means the internal time series (sec 3) is the
project's own, and every provider -- including WU -- is a translation
into it.

**Priority is real, though, and it is not a tie.** WU comes first: it is
what the brief asked for, it is what gets implemented first, and where
providers disagree about what to show, WU wins until something says
otherwise.

### 2.8 What makes something a candidate provider

**The graphs are the criterion.** This is not "support many weather
APIs" -- most of them offer a three-hourly forecast that would make the
graph worse, and adding one would be work spent making the product less
of what it is for.

A provider earns its place by supplying data at **comparable resolution
to what sec 3 asks for**: sub-hourly precipitation, hourly temperature
out several days, and ideally observed history. Anything that cannot
feed a graph of this quality is not a candidate, however convenient its
API.

The following are **candidates to evaluate, not decisions**, and each
needs its actual resolution and terms verified rather than taken from
this list:

| Provider | Why it is here | To verify |
|---|---|---|
| Weather Underground | The brief. First, per sec 2.7 | Endpoints (sec 2.6) |
| MET Norway | Free, no key; nowcast is radar-based and sub-hourly, with its best coverage over the Nordics | Nowcast area and cadence; the mandatory identifying User-Agent, which they enforce |
| Open-Meteo | Free, no key; publishes sub-hourly series for Europe and North America off high-resolution models | Which regions genuinely get sub-hourly, and the licence terms for the intended use |
| SMHI | Sweden's own, open and key-less | Whether resolution beats what the others already give here |

Two things to keep in mind while evaluating. **A provider with clean
terms is worth more than a marginally better curve**, since WU already
supplies the compromised path and a second one adds nothing. And a
provider's *coverage area* is part of its resolution: a five-minute
nowcast that stops at a border is hourly everywhere else.

## 3. The graph is the program

The single hardest thing in this project, and it is a data problem before
it is a drawing problem.

**One continuous time axis carrying three sample rates from three
sources:**

| Band | Cadence | Relative to now |
|---|---|---|
| Observed / historical | 5 min from a PWS (sec 2.6) | behind |
| Nowcast precipitation | 15 min | next 7 hours |
| Hourly forecast | hourly | out to 15 days |

Those are the measured figures from sec 2.6, not aspirations. The
observed band is the *finest* of the three at 5 minutes, which is the
opposite of what was assumed when this section was first written.

All four resolution axes were asked for explicitly, including visual
density as a goal in its own right -- the WU chart aesthetic, not merely
the underlying sample rate.

Two things follow:

- **The compositing model comes before any painting.** The joins between
  bands must not read as joins, which is a resampling and alignment
  question, not a pen-width question. This is the first real design work
  and it has not been done yet.
- **Normalising is the first thing the model does, and it is not
  cosmetic.** Sec 2.6.2 measured what arrives: one band with no UTC
  field, one in reverse order, and three different rain quantities --
  a rate, a per-step accumulation, and a running 24-hour total. The
  internal series stores one representation of time and one of rain, and
  every reader converts into it. A band that reaches the graph still
  carrying the provider's spelling is the bug that draws a believable
  wrong picture.
- **The forecast is a plain time series**, kept free of presentation
  concerns, so that something can later score over it (sec 7) without
  reaching into a widget.
- **The time series is the project's own, and providers translate into
  it** (sec 2.7). Not WU's response shape with other providers bent to
  fit -- WU translates into it exactly as the others do, and is not
  privileged by the data model even though it is privileged by priority.

A band and a provider are **not the same axis**. Three bands and several
providers means the bands need not all come from one place: the best
available nowcast and the best available hourly forecast may be two
different services, and the compositing model has to survive that rather
than assume a single source per graph.

That is a reason to design it now rather than discover it later. It is
also a reason the joins matter more than they would otherwise: a seam
between two bands from one provider is a resampling artefact, while a
seam between two providers can disagree about the actual weather.

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
- Multi-provider from the start, Weather Underground first (sec 2.7)
- Resolution is what qualifies a provider, not convenience (sec 2.8)
- Three bands on one time axis, forecast kept presentation-free (sec 3)
- The internal time series is ours; every provider translates into it,
  including WU (sec 2.7, sec 3)
- BBQ scoring deferred (sec 7)
- No licence (sec 8)

- The WU endpoints for all three bands, observed 2026-08-07 (sec 2.6)
- The key comes from an embedded request URL, not the config blob
  (sec 2.6.1)

Open, each needing a decision rather than a drift:

- Which providers past WU actually qualify, measured rather than
  assumed from the candidate table (sec 2.8)
- The compositing model itself -- the first real design work, and now
  with bands that may come from different providers (sec 3)
- QPainter vs QtCharts, to be confirmed by trying (sec 3.1)
- Which desktop, and therefore whether the tray needs a fallback (sec 4.1)
- Packaging mechanism (sec 5.1)
