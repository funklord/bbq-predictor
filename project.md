# bbq-predictor

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

- App id: `se.vibes.bbq-predictor`
- Symbol prefix: `bbq_`
- Version: one place, the `VERSION` file

### 0.1 The name

**The name describes the program now**, which it did not for most of
this project's life. It was `bbqpredictor` and the grilling prediction
was deferred; the prediction is sec 7 and the spelling is kebab-case,
which `code-style.md` already allows as the package-system spelling and
which a Debian package would have to use anyway.

The rename is worth one note beyond the spelling, because it cost
something not obvious. **`QStandardPaths::AppConfigLocation` is derived
from the application name**, so renaming the program moved the
configuration out from under it -- and the file it left behind was the
one holding the pinned station, which is the single setting the applet
cannot work without. Copied across by hand rather than lost. Anything
that renames this program again has to move that file with it.

The symbol prefix stayed `bbq_` throughout, and the include guards
`BBQ_`. Both were already short enough not to carry the project's full
name, which is why neither had to move.

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

### 2.3.1 The queued-request connections were not paired

**Found by reading, not by running. Fixed, and the fix is tested.**
Kept here in full because the reasoning is the useful part: this is the
only defect in the project that no amount of running would have
surfaced.

When `bbq_wu_client::send` is called before a key exists, it makes
*two* connections to the key source -- one to `acquired` that resends
the request, one to `failed` that reports it -- and each lambda tears
down only its own. So whichever fires, **the other survives**.

That is not merely a leak:

- **Acquisition succeeds.** The `failed` connection stays live. A later
  failure -- after a 401 invalidates the key (sec 2.3) -- fires it and
  emits `failed` for a product that already completed. The feed calls
  `finish_one()` again for a request it already counted.
- **Acquisition fails.** The `acquired` connection stays live. The next
  successful acquisition re-fires it and re-sends a request that
  already reported failure, producing a second answer for one request.

Either way the outstanding count is decremented more often than it was
incremented. `finish_one()` clamps at zero, so nothing goes negative --
it emits `settled` **early**, while requests are still in flight. In
`--fetch-once` that ends the loop before the data lands; in the applet
it is a spurious settle.

**The fan-out makes it worse.** A cold start with a station queues four
Weather Underground requests behind one key acquisition, so there are
four pairs, and every 401 retry cycle adds more.

The fix is not a third teardown but a different shape: the pending
requests are held in a list, the key source is connected **once** in the
constructor, and whichever signal arrives drains that list. There is no
per-request bookkeeping left to get wrong.

`waiting()` exists so the queue's emptiness is assertable from outside,
which is what makes the test below possible at all.

Why it never bit in practice: every observed run acquired a key on the
first attempt and never got a 401, which is the one path that leaves
the stale connection reachable. **It worked because the failure case
had not happened yet**, which is exactly the kind of correctness this
project does not want to rely on.

**How the fix is known to work.** `tests/test_client.cpp` drives the key
source's outcomes by hand -- queue two requests, report a failure,
then report an acquisition -- and asserts the queue stays empty. The
old mechanism was reinstated deliberately and the test fails against
it, at the assertion after the acquisition, with one request re-queued
that had already been answered. It passes against the fix. A test that
was not watched failing is not yet evidence of anything.

The test found something on the way, which is recorded because it is a
better example than the defect it was written for. Its first version
used `QTEST_APPLESS_MAIN`, and **without a `QCoreApplication`,
`QNetworkAccessManager` returns null replies** -- so the code under test
was connecting to `nullptr` and every assertion held for reasons
unrelated to the queue. It passed. The only sign was a warning in the
log. It runs `QTEST_GUILESS_MAIN` now, against a dead proxy on
localhost and without ever spinning the event loop, so nothing leaves
the machine and every assertion is synchronous.

### 2.3.2 A stalled connection used to be fatal, permanently

**No request had a deadline.** Found by reading, confirmed by measuring
against a server that accepts connections and never answers.

A reply that never finishes never calls `finish_one`, so `m_outstanding`
never returns to zero -- and `tick()` refuses to start a round while
anything is outstanding, deliberately, so that a slow round is never
stacked on top of itself. Together those two correct behaviours make one
stalled socket **permanent**: the graph freezes, auto-refresh stops for
the life of the process, the tray goes red at two hours and stays red,
and nothing short of a restart recovers. If the stall is the key page
rather than a band, `m_in_flight` never clears either, so every request
queued behind it waits for ever too.

Measured both ways, forty-five seconds against the stalling server:
before, six requests and not a single completion. After
`setTransferTimeout(30 s)`, every request fails at the deadline,
`m_outstanding` unwinds to zero, `settled` fires, and the next heartbeat
retries.

Thirty seconds is chosen for a slow mobile link, which sec 11 makes a
real case rather than a hypothetical one. A timeout arrives as an
ordinary reply error, so every band's existing failure path already
handles it -- no new branch, which is why the change is one line on the
manager the four providers share.

### 2.4 Staleness is visible, always

This is the fragile joint in the whole program, and its failure mode is
not an error -- it is **a graph that keeps drawing yesterday's curve
while looking perfectly healthy.** A stale graph that looks fresh is
worse than no graph, because a decision gets made on it.

So: the last successful fetch time is part of the display, not hidden in
a tooltip, and a failed refresh is visible in the tray icon as well as in
the window. A silent fall back to cached data is a defect.

### 2.4.1 Two bands were never refreshed at all

The heartbeat only ever asked Weather Underground. The radar and
extended bands were fetched once, in the startup burst, and never
again -- `tick()` walks the `bbq_wu_product` values, and neither of
those bands is one.

**Sec 2.4's staleness rule then reported it correctly, and looked like a
bug of its own.** `oldest_fetch_utc()` takes the oldest fetch across
every band precisely so a stalled one cannot hide behind fresh ones, so
the tray turned red exactly two hours after every launch and stayed red
while the four WU bands refreshed perfectly. The detector was right. The
refresh was incomplete.

The same reading found a second fault in the same place: the startup
fetches called the client directly instead of going through `attempt()`,
so nothing recorded that the bands had been asked for. The first
heartbeat saw a last-attempt of zero for all of them and refetched every
band **sixty seconds after launch**, which is exactly what the freshness
table exists to prevent, against the one provider whose quota is not
ours to spend.

Both measured, before and after, by tracing every outbound fetch across
two heartbeats. Before: six bands at startup, then four again at +61 s,
and radar and extended never again. After, with the two intervals
shortened so a heartbeat had to pick them up: six at startup, then only
radar and extended at +61 s and +121 s, and the WU bands silent until
their own intervals came due.

Every fetch goes through an attempt now, and an attempt is what records
the time -- the two are one operation rather than two that have to be
remembered together.

### 2.5 Be a polite client

A scraped key is somebody else's quota, and hammering is what gets a key
pattern noticed and closed. Cache to disk, survive a restart without
refetching, and poll no faster than the data behind the endpoint actually
updates -- which for an hourly forecast is not every thirty seconds. This
is self-interest as much as manners.

### 2.5.1 The refresh intervals

Auto-refresh is per band, not one interval for everything, because the
bands do not change at the same speed. Refetching all of them on the
fastest schedule would be several times the requests for no extra
information -- against somebody else's quota, on a scraped key.

| Band | Interval | Why |
|---|---|---|
| Current | 5 min | the station's own reporting cadence |
| Observed | 10 min | it lags by up to twenty minutes anyway (sec 3.9.1), so a tighter interval mostly re-downloads the same rows |
| Nowcast | 15 min | its own step |
| Hourly | 60 min | a fifteen-day forecast does not move between breakfast and lunch |

Two details make this polite rather than merely scheduled.

**The clock is checked, not counted.** A one-minute heartbeat decides
nothing on its own; each band's interval is measured against the wall
clock, so the tick rate bounds only how *late* a refresh can be, never
how often one happens.

**Backoff is on attempts, not successes.** A band whose endpoint is
failing never advances its success time, so it would look permanently
due and be retried every minute. The schedule therefore records when
each band was last *tried*. Staleness on the display still reads the
success times (sec 2.4), so a band failing quietly still shows as old
-- the two clocks answer different questions and are kept apart.

**Rounds never overlap.** A slow round is not given a second one on top
of it; the tick skips, which costs at most a minute.

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

### 2.6.5 The station is chosen by the user and pinned

The observed band (sec 2.6) is per-station: `/v2/pws/history/all` takes a
`stationId`, of the shape `ISTOCK822` as observed on a real page.

**Settled: the user picks the station, and it is pinned in config. It is
never auto-selected and never silently changed.**

The alternative -- resolve the nearest station to the current geocode on
each refresh -- is the tempting one and it is wrong, for the same reason
sec 2.4 exists. A nearest-station lookup means the graph's *provenance*
moves without the display changing: today's observed curve comes from a
garden two streets away, tomorrow's from an airfield, and both are
labelled the same. That is not a stale reading, it is a reading of
somewhere else, and it is even harder to notice.

Pinning also matters because personal weather stations are personal. A
station is somebody's hardware in somebody's garden; it goes offline,
gets moved, or reads badly in ways the owner knows about and a nearest
lookup does not. A user who has chosen theirs has chosen it for reasons
the program cannot reconstruct.

### 2.6.6 What follows from pinning

- **Discovery is a separate, explicit act.** Finding candidate stations
  is a picker the user opens, not something that runs on refresh.
  `/v3/location/near` is for that moment and no other. A refresh path
  that can discover a station is a refresh path that can change one.
- **A pinned station that stops reporting is said, not replaced.**
  Substituting a neighbour would be the sec 2.4 failure wearing a
  helpful face. The observed band goes absent, and absent is drawn as
  absent.
- **The observed band is optional.** No station configured is a normal
  state, not an error: the nowcast and hourly bands are geocode-based
  and need no station at all. The graph works without it and says which
  band is missing rather than showing a gap and hoping.
- **Config lives where Qt puts it** --
  `QStandardPaths::AppConfigLocation`, which is `~/.config/bbq-predictor/`
  on this platform, in INI through `QSettings`. This is the project's
  first configuration of any kind, so it sets that location for
  everything after it. Implemented in `src/model/settings.cpp`; see sec
  2.6.8 for what the command line does to it, and sec 0.1 for what a
  rename does to it.

### 2.6.7 The geocode derives from the station, with an override

Pinning a station introduces a second setting that can disagree with it:
the observed band is keyed by `stationId`, the nowcast and hourly bands
by `geocode`. Nothing structural stops a config naming a station in
Stockholm against a forecast point in Gothenburg, which would draw two
places on one axis without a word.

**Settled: the geocode is derived from the pinned station's own
coordinates. An explicit override exists, and when set it wins.**

So there is one setting to make in the normal case, and the two cannot
drift apart by accident -- only by somebody saying so.

### 2.6.7.1 The derivation needs no extra request

**Verified 2026-08-07 against a real response.** Every row from the PWS
endpoints carries its own metadata alongside the observation:

| Field | Example | Use |
|---|---|---|
| `stationID` | `ISTOCK822` | echoes the pinned station back |
| `lat`, `lon` | `59.339`, `18.055` | the derived geocode, to 3 decimals |
| `tz` | `Europe/Stockholm` | the station's IANA zone |

The band that needs the station therefore *supplies* the coordinate the
other bands need. No lookup endpoint, no second round trip, and no
separate place for the answer to go stale.

`tz` is a bonus worth noting rather than a requirement: sec 2.6.2's
awkward case is `fifteenminute` arriving as local time. Its offset makes
it parseable without help, so this is not load-bearing -- but a real
IANA zone for the station is better than an offset for anything that has
to name a time to the user.

### 2.6.7.2 Cache the derived geocode, do not re-derive on demand

Deriving from the observation response creates an ordering problem if
taken literally: the forecast bands need a geocode, the geocode comes
from the PWS response, so a cold start would have to fetch the observed
band first and make the other two wait behind it.

**So the derived geocode is stored in config beside the station**,
refreshed whenever the PWS responds with a different one. It is a cache
of a derivation, not a second source of truth, and the station remains
the thing the user chose.

This also keeps sec 2.6.6's promise honest. A station that is offline
yields no observation and therefore no coordinate -- but the cached one
is still there, so the nowcast and hourly bands keep working while the
observed band is reported absent. Re-deriving on demand would have made
a dead station take the whole graph down with it, which is precisely
what "the observed band is optional" was supposed to prevent.

### 2.6.7.3 An override must be visible

The override exists for a real case: somebody wanting a nearby station's
readings against a different forecast point, which is reasonable when
the nearest good station is not where you are.

**When it is set, the display says so.** An override that is silent
recreates the exact failure this section removed -- two places on one
axis with nothing to say which is which -- and the fact that a user
opted into it does not make the graph less misleading six months later.
Deliberate is not the same as remembered.

### 2.6.7.4 The cache has an in-memory half, and it was not being dropped

Sec 2.6.7.2 caches the derived coordinate in the config file, and
`bbq_settings::set_station` removes that cache the moment the station
changes -- for the reason 2.6.7 gives, that a coordinate belonging to
one station must never be used with another.

**The running feed kept its own copy, and nothing dropped that one.**
Changing the station in the window wrote the new id, cleared the cache
on disk, and left `m_latitude`, `m_longitude` and `m_have_geocode`
holding the previous station's garden. The refresh that followed
immediately aimed the radar, nowcast, extended and hourly bands there
while the observed band read the new station: two places on one axis,
which is precisely the failure this whole section exists to prevent, and
it survived because only half the state was being invalidated.

It compounded. The observed handler derives a coordinate only when it
does not already have one, so with the stale one still held the new
station's coordinate was **never learned** -- not for the rest of the
session, and never written back to the cache either. A restart was the
only thing that fixed it, and a restart made it look like it had never
happened.

The fix is to say where a coordinate came from. One chosen by
configuration -- `geocode_override`, or `--geocode` -- is **pinned**: it
belongs to no station and no station change may discard it. One derived
from a station, including the config cache that was derived from one
earlier, is not, and goes when the station goes.

`has_geocode()` is public so the distinction is assertable from a test
rather than only reasonable in the source, and `tests/test_feed.cpp`
asserts all three cases: derived does not survive, pinned does, and
re-setting the same id changes nothing. That last one matters because
the field writes on `editingFinished`, which fires when the box merely
loses focus -- treating that as a change would throw away a good
coordinate every time somebody clicked past it.

### 2.6.8 The command line overrides the run, not the configuration

`--station` and `--geocode` win for the run they are given on and
**write nothing**. Trying a different station therefore leaves the
configured one alone, which is what an override should mean.

That has one consequence worth stating, because getting it wrong was a
real defect rather than a hypothetical. The geocode derived from a
station (sec 2.6.7.2) is cached in config -- but only when the station
in use is the CONFIGURED one. An override run that cached its
derivation would file one station's coordinates against a config naming
another, and the next argument-free run would place the forecast bands
at a station it was not reading. Two places on one axis, arriving
through the door sec 2.6.7 had bolted.

Changing the station clears the cached coordinate for the same reason,
rather than leaving the previous garden's location behind it.

The station is editable in the window as well as in the file. A tray
applet whose one required setting can be given only on a command line
is one nobody can configure from the thing they are looking at.

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

**Two were measured on 2026-08-07 and one is now in use** -- see sec
2.9. What follows was the candidate list; the measurements are below
it.

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

### 2.8.1 What they actually deliver, measured

| Source | Cadence | Span | Rain | Wind | Key |
|---|---|---|---|---|---|
| MET nowcast | **5 min** | 1.8 h | `precipitation_rate`, **mm/h** | m/s | none |
| MET locationforecast | 60 min | 9.8 days | accumulation per hour | m/s | none |
| Open-Meteo 15-min | 15 min | **7 days** | accumulation | -- | none |
| Open-Meteo hourly | 60 min | 7 days | accumulation + probability | **km/h** | none |
| WU nowcast | 15 min | 7 h | `precipRate`, mm/h | km/h | scraped |
| WU hourly | 60 min | 15 days | `qpf` accumulation | km/h | scraped |

Three findings worth having.

**MET's nowcast beats WU's on resolution and loses on span** -- five
minutes against fifteen, but under two hours against seven. That is not
a better provider for one band, it is a different band, and sec 2.9
treats it as one.

**Open-Meteo publishes 15-minute data for seven days**, which is
neither WU nor MET's shape and covers the gap between them. It also
returns wind already in km/h and names the location's IANA zone in the
response, both of which this project had to work for elsewhere.

**Neither needs a key or breaks any terms.** Sec 2.2's compromise buys
nothing that MET does not give away for the first two hours, which is
worth knowing when the scrape eventually breaks.

### 2.9 MET Norway serves the radar band

**In use.** The sub-hourly band that sec 2.6.4 called the least safe
thing in the project now has a second, cleaner source -- and it was the
band that most needed one, since WU's fifteen-minute endpoint answers a
scraped key while being called by none of WU's own pages.

### 2.9.1 A second band, not a replacement

The first attempt substituted MET for WU's nowcast, and that was a
**regression dressed as an upgrade**: it traded four hours of
quarter-hour data for hourly, because MET reaches under two hours where
WU reaches seven. Caught by looking at the ribbon and seeing the fine
band end early.

So `nowcast_fine` is its own band at its own priority, above the
ordinary nowcast where they overlap and silent where MET does not
reach. The ribbon now runs radar, then nowcast, then hourly.

### 2.9.2 It is absent, not missing

The radar band is deliberately left out of sec 2.6.6's missing-band
report, and a MET failure is not announced as a band failure either.

It is a bonus where the provider reaches and simply does not exist
elsewhere, so reporting it would put a permanent complaint on the
display of everybody outside its coverage -- and the whole point of
naming a missing band is that the name means something.

### 2.9.3 The agent tells the truth here

MET requires clients to identify themselves contactably and blocks the
generic ones. That is a condition of a service given away free, so it
is met honestly -- where sec 2.2's scraper wears a browser's agent
because that whole path is already the compromised one.

A provider reached legitimately gets a truthful agent. The two live a
few files apart and the difference between them is the point.

### 2.10 Open-Meteo serves the extended band

**Quarter-hourly out to a week.** It covers the ground neither other
provider reaches at a useful resolution: MET's radar stops under two
hours, WU's fifteen-minute band at seven, and after that the choice was
sixty-minute steps or nothing.

It is complete, which was checked before it was designed around. Its
fifteen-minute series carries temperature, precipitation, wind AND
probability -- so a band that outranks the hourly one does not quietly
drop the fields sec 7 scores on. A band that wins by cadence and loses
by content would have made the forecast worse while looking sharper.

Priority sits under the ordinary nowcast, which shares its cadence over
a range aimed at the near term, and over hourly, which is what it
exists to replace.

### 2.10.1 It reads the clock the hard way

Open-Meteo's stamps are LOCAL and carry no offset -- `2026-08-07T00:00`
and nothing more -- so they cannot be read without the zone the response
names separately. An unreadable zone therefore discards the band rather
than falling back to anything: read in the wrong zone the whole series
shifts by hours while looking perfectly valid.

They are converted through the IANA zone rather than the single
`utc_offset_seconds` the response also offers. A fixed offset is right
until a daylight-saving change and silently an hour out afterwards, and
this series runs seven days -- long enough to contain one.

`timezone=auto` is requested deliberately for that name. Without a
pinned station this is the only provider here that supplies a real zone
rather than the bare offset a WU forecast implies (sec 3.12.1).

**An empty zone name is rejected before the zone is constructed, and
that order is the whole of it.** `QTimeZone(QByteArray(""))` does not
produce an invalid zone -- it produces the LOCAL one and reports itself
valid. So a response with a missing or empty `timezone` would have been
read in the viewer's clock, silently shifting the whole series and
putting back the exact defect sec 3.12.1 removed, in the one provider
whose stamps carry no offset to contradict it.

A wrong name is caught by `isValid`. An absent one is caught by
nothing, which is why it is caught by hand. **Found by a test on its
first run** (sec 5.2.2).

### 2.10.2 It closed a hole nobody asked it to

Sec 3.9.4 left the observed band's lag uncovered -- the stretch between
the station's last delivered row and now -- on the grounds that the
next refresh fills it. On the day this landed that lag was **four
hours**, and the extended band covers it.

That is modelled data standing where measured data has not arrived yet,
which is a real thing to be aware of rather than a free win. It is
acceptable here only because the ribbon names the band: the graph says
"extended" across that stretch, so nobody is being shown a model and
told it is a measurement. Sec 3.3's ordering still holds where both
exist, since observed outranks it by a wide margin.

### 2.10.3 The diagnostic runs through the feed, and reads the config

`--fetch-once` drives `bbq_wu_feed` rather than the WU client, so it
exercises every provider the application does and reports each band
beside the provider that supplied it.

It did not, for two commits. It predated the feed, drove the WU client
directly, and carried its own copy of the band dispatch and the geocode
derivation -- so it checked four Weather Underground products, neither
of the other two providers, and a second copy of an orchestration that
had moved on. **A check that no longer inspects what the application
does** is the vacuous pass this project refuses everywhere else,
arriving in the tool meant to catch it.

Driving the real thing also deleted a hundred lines. Two copies of an
orchestration are two things to keep in step, and the one nobody runs
is the one that drifts.

It reads the configuration too, in the same order the window uses (sec
2.6.8). It did not at first, so it announced "nothing configured"
against a config file with a station in it -- the same divergence
between check and application, arriving again in the same tool one
commit after being removed from it. Found by running it after the
rename, with nothing but the config to go on.

### 2.10.4 The quarter-hourly claim was false here

**This band was quarter-hourly for three commits and should not have
been.** At this project's station, Open-Meteo's `minutely_15` is
interpolated from its own hourly data.

The provider says so, and then its API proves it. The documentation
states that fifteen-minute data comes from HRRR over North America and
from ICON-D2 and AROME over central Europe, and is **interpolated from
hourly for other regions**. Asking the API for `models=icon_d2` settles
which side of that line a location falls on:

| Location | ICON-D2 |
|---|---|
| Berlin | 192 points, all present |
| This station, 59.34 N | HTTP 400, "No data is available for this location" |

**Why it matters more here than it would elsewhere.** The graph marks
real samples, and sec 3.11.3 makes those marks the thing that keeps a
smoothed curve honest -- the dots are the data and the line between
them is drawn. Quarter-hourly points that nobody modelled are not data,
so marking them puts dots on the graph at instants no forecast ever
described. That is the failure this project has spent its whole life
refusing, arriving through a provider's convenience feature.

So the band takes the hourly block, and its priority moved below WU's
hourly band, since two bands of one cadence are a tie and sec 2.7 gives
those to WU.

### 2.10.4.1 What the measurement could not do

Worth recording because the reasoning was sound and the method still
failed.

Comparing the two blocks -- does a fifteen-minute point sit on the
straight line between its neighbouring hours? -- looked decisive and is
not. Open-Meteo composes each block from whichever model suits it, so a
deviation means the blocks disagree rather than that the finer one has
structure, and an agreement means either derivation or genuinely smooth
weather. On the day it was run, Berlin *inside* the ICON-D2 domain and
this station *outside* it both came back at rounding level, because the
weather was calm in both. **A controlled comparison with a real control
still could not tell them apart.**

Precipitation is worse: an hourly accumulation split four ways is not
the linear interpolation of consecutive hours, so it deviates whether
or not anything was modelled.

What settled it was asking the provider which model covers the point --
a question with an answer, rather than a statistic with an
interpretation.

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
  question, not a pen-width question. It is designed in sec 3.1 to 3.7
  and not yet implemented.
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

### 3.1 The unit of the model is a span, not a point

    start_utc     epoch seconds, UTC
    duration_s    how long this sample covers
    temperature   optional
    precip_rate   optional
    ...           each field optional independently

**A sample covers an interval rather than marking an instant.** Three
reasons, and each of them is load-bearing rather than tidy:

- The bands have different step lengths (5, 15 and 60 minutes), so
  "the next sample" is not a fixed distance away.
- Rain is a mean over an interval, not a value at a moment (sec 3.2).
- The end of a band's coverage has to be knowable. A band that stops
  at +7 hours has to say so; with bare timestamps the last point is
  indistinguishable from a gap that happens to be at the end.

**The two quantities do not have the same semantics, and that is not
papered over.** Temperature is the value *at* `start`; rain rate is the
mean *across* `[start, start + duration)`. That asymmetry comes from the
data rather than from the model, and hiding it would only move it
somewhere less visible.

Canonical units are Celsius, millimetres per hour, and epoch seconds
UTC. **Every reader converts explicitly, even when `units=m` was
requested.** A units parameter is a request, not a guarantee, and a
silent unit error is the exact failure this project keeps naming: a
graph that is wrong and looks fine.

### 3.2 Rain is stored as a rate

The decision that makes one y-axis possible, and it is not arbitrary.

Sec 2.6.2 measured three different rain quantities arriving. There were
two candidates for what to store:

- **Accumulation per step** (mm) is *extensive* -- it depends on the
  step length. The same weather gives a different number at 5 minutes
  than at 60, so the bands cannot share an axis and every resample
  changes the values.
- **Rate** (mm/h) is *intensive* -- the same weather gives the same
  number at any cadence.

**So the model stores mm/h.** The conversions from what sec 2.6
measured are then almost free:

| Source | Field | Conversion |
|---|---|---|
| `fifteenminute` | `precipRate` | already mm/h |
| PWS history | `precipRate` | already mm/h |
| `hourly/15day` | `qpf` (mm per step) | `qpf / duration_hours` |

Write that division out even though `duration_hours` is 1 today and the
numbers are therefore equal. The day a provider offers a three-hourly
`qpf`, the shortcut is a silent factor of three.

**The honest caveat:** a rate meaned over 60 minutes and a rate meaned
over 15 are the same unit and not the same measurement -- one is
smoothed, and a downpour is flattened by it. Storing `duration_s`
alongside is what keeps that difference visible instead of lost.

**The ICAO historical path cannot supply rain at all.** Its only figure
is `precip24Hour`, a running 24-hour total; recovering a per-step value
means differencing successive samples, which is noisy and breaks at the
daily reset. That path is therefore a **temperature-only fallback**, and
the PWS band is the only real source for observed rain.

### 3.3 Precedence is declared, not inferred

The bands overlap. Nowcast and hourly both cover the next 7 hours, and
observed and nowcast can both cover the last few minutes.

The tempting rule is "the finest resolution wins", and it is rejected.
It is *emergent* -- it would change meaning silently the day a provider
adjusted its cadence, and nothing in the tree would record that the
graph had started preferring a different source.

**Each series carries an explicit priority instead**, and the order is
declared:

    observed  >  nowcast  >  hourly

The first `>` is the load-bearing one. **Measured beats forecast, always
and regardless of resolution.** An observation is what happened; no
forecast should overwrite it because it happens to have finer steps. The
second `>` is a genuine preference between two forecasts and is a
configured number, not a runtime derivation.

### 3.4 The composite is a view, not a merge

**The bands are not flattened into one array.** The composite holds the
series and resolves precedence when drawing.

Merging would be simpler to draw and it destroys the one thing this
project cannot lose: **provenance**. Three separate requirements need
it, and none of them survives a flatten -- sec 2.4 shows staleness *per
band*, sec 2.6.6 reports *which band is absent*, and sec 2.7 has bands
that may come from *different providers*.

A flattened array cannot say "the observed band is missing". It can only
produce a gap, and a gap that means "not configured" is drawn
identically to a gap that means "the station died" and to one that means
"it was not raining". That is the failure this whole document keeps
circling.

The cost is nothing. The bands are 288, 28 and 360 samples.

### 3.5 Never upsample, and downsample rain by maximum

**Never upsample.** Inventing intermediate samples is inventing data,
and an interpolated point is indistinguishable from a measured one once
it is in the array.

Downsample only where pixels are scarcer than samples, and **the
aggregation depends on the quantity**:

| Quantity | Aggregate | Why |
|---|---|---|
| Temperature | mean, or a min/max envelope | it varies smoothly |
| Rain rate | **maximum, never mean** | see below |

A five-minute downpour meaned into an hour disappears. For this program
that is the single most important thing on the graph -- and it is
exactly what sec 7's grilling window would ask about. **Averaging rain
is how a graph tells you the afternoon was dry when it was not.**

### 3.6 Gaps are drawn as gaps

A discontinuity larger than the band's own nominal step is a gap, and it
is **drawn as a break in the curve, never interpolated across**.

Joining two points across an hour of missing data draws a line that is
not a measurement, through a period nobody has any information about.
It is sec 2.4 in miniature.

**1.5 times the nominal step, and it has now been checked rather than
guessed.** Across all six bands from three providers -- 5, 15 and 60
minute cadences, 1086 samples in one run -- the rule reports no gaps
anywhere, which is the correct answer, since none of those responses
had one.

The tightest real case is the observed band, which arrives at 288 and
306 second strides against a 300 second median: a ratio of 1.06 against
a threshold of 1.5, so there is comfortable room before ordinary
jitter would be drawn as a break.

The rule was also suspected of a specific false positive that turned
out not to exist. MET's radar band was believed to coarsen after the
first hour, which would have put a 2x stride inside a band and been
flagged as a gap that is not there -- but measured across a full
nowcast every stride is exactly five minutes, and the belief was an
unchecked claim in a code comment rather than something the provider
does.

### 3.7 What makes a join disappear

Sec 3 asks that the joins not read as joins. The honest reading of that
is **they must not read as artefacts** -- and the way to get there is
not the obvious one.

**A join disappears because normalisation is right, not because it was
blended.** Once both sides are the same quantity, in the same units, on
the same axis, with their interval semantics respected, a continuous
curve is continuous because the weather is. Nothing needs smoothing.

**Any step that survives correct normalisation is information, and is
preserved.** Two providers disagreeing about the next hour is a fact
about the forecast, and the graph's job is to show it.

So there is **no crossfade, no blending, and no offset-correcting a band
to match its neighbour at the seam.** Every one of those invents
agreement that does not exist, which is the same sin as inventing a
sample.

This also gives a debugging rule worth having:

- A seam **within one provider** that survives normalisation is a **bug
  in the normalisation** -- most likely a unit, an interval, or the
  reversed historical series from sec 2.6.2.
- A seam **across providers** is **data**, and leaving it alone is
  correct.

### 3.8 Rendering: QPainter, confirmed by building it

The prior was a hand-painted `QWidget` over QtCharts. **Built, and the
prior held**, so this section now records what trying it taught rather
than what was expected.

The whole widget is one pass that reduces each pixel column to a value,
and then three passes that walk that vector -- rain, temperature, and
the provenance ribbon. Sec 3.5 falls straight out of the reduction: rain
takes the maximum over the column and temperature the mean, in adjacent
lines. There was nothing to fight.

What QtCharts would have fought is not the line but everything around
it: a ribbon under the plot coloured per band, a break in the series
wherever no band covers a column, and a per-column maximum rather than
a point sample. Each is natural in a paint method and each is an
argument with a series-and-axis model.

### 3.8.1 Two defects, both found by looking at the picture

Neither would have failed a test that checked the numbers, and that is
the point of rendering a screen and looking at it.

**The temperature came out as a staircase.** Holding a sample's value
flat across its whole span asserts that temperature is constant for an
hour and then jumps, which is false -- sec 3.1 makes temperature a
value AT an instant, so consecutive points are joined. Rain is the
opposite and stays flat, because it genuinely is a mean across its
span. The same widget therefore draws its two quantities with different
rules, and that is the model being read correctly rather than an
inconsistency.

**The first fix did nothing, for an instructive reason.**
`bbq_series::range` returns samples OVERLAPPING a column, which is
right for rain -- any span touching the column is part of its answer.
But an hourly sample overlaps all thirty of its columns, so the mean
branch always found one and the interpolation never ran. One helper,
two questions, and only one of them was being asked. Temperature now
counts a sample only where its START lands in the column.

### 3.8.2 The palette, measured from WU's own chart

Sec 0 asks for the Weather Underground chart aesthetic. **It was
measured on 2026-08-07 rather than chosen**, and the values are in
`src/graph/forecast_graph.cpp`.

Getting them took three attempts, and the first two failed in ways
worth recording because they say where the answer is NOT.

- **Not in the page or the stylesheet.** WU renders its chart
  client-side from lazily-loaded bundles. The stylesheet holds Angular
  Material framework tokens and nothing about the chart.
- **Not on the hourly forecast page.** That page is a TABLE. The graphs
  WU is known for live on the personal weather station dashboard, which
  is also where this project's observed band comes from -- the brief
  and the data source point at the same page.

With Qt WebEngine available, a throwaway tool rendered the dashboard
and the values were counted out of the pixels:

| Role | Value |
|---|---|
| Plot background | `#ffffff` |
| Alternating three-hour bands | `#f1f7fb` |
| Gridlines | `#e7e7e7` |
| Temperature | `#d5202a` |
| Dew point | `#5b9f49` |
| Precipitation rate | `#87c403` |
| Precipitation accumulation | `#17aadb` |
| Wind speed | `#0053ae` |

Two of them were corroborated independently, which is why the set is
trusted rather than merely recorded. `#d5202a` appears both in the
rendered chart and as a brand token in WU's stylesheet, arrived at by
different routes. And a later capture came out uniformly dimmed by a
modal overlay; inverting that dim reproduced the already-known red and
blue to within one digit, which is what made the precipitation colours
recovered the same way believable.

The alternating bands matter as much as the colours. They are the most
recognisable thing about the chart and the cheapest density cue there
is -- a ruler for the eye that costs no lines.

### 3.8.3 What the palette cost

**The graph no longer follows the desktop into dark mode.** The colours
were derived from `QPalette` before this and are fixed now.

That is a real trade and not an oversight: a white plot with pale blue
hour bands and a red line IS the aesthetic sec 0 asked for, and a
version of it that inverts on a dark desktop is a different chart. Sec
0 also settles the tie -- where a decision trades graph quality against
anything else, the graph wins.

Worth revisiting if the applet turns out to be used mostly on dark
desktops, which is a question about how people run it rather than one
this document can answer.

### 3.9 The hole at now, and the current band

**Found by running the model against live data on 2026-08-07**, not by
reading the endpoint table -- and what it turned out to be was not what
the first look suggested.

The observed band ends when the history endpoint last caught up, and
the nowcast begins at a quarter-hour boundary. Neither is anchored to
the present, so between them sits a hole. When the clock falls inside
it, the graph has nothing to say about **now**, which is the single
most important instant on it.

### 3.9.1 What the hole actually is

Not a fixed gap. **The observed band's lag is variable**, measured at
22 minutes on one fetch and 4 minutes on another a few minutes later,
which points at caching rather than at the station. The nowcast's first
sample is sometimes the current quarter-hour and sometimes the next.

So the hole opens and closes with the clock and with whatever the cache
last served. That is the worst shape a defect can have: a run that
happens to land in covered time reports success exactly as loudly as a
run that is genuinely whole.

**The diagnostic therefore scans rather than probes.** A single query at
now proves nothing; walking the whole composite and reporting every
uncovered interval produces a claim that can be checked.

The scan then produced a false alarm of its own, which is worth keeping
as an example rather than quietly deleting. It stepped a fixed stride
and judged "is now covered" from the stepped boundaries, so it reported
a hole at now while the composite was answering now perfectly well --
the instant had landed in the sliver between the last uncovered step
and the real edge of coverage. The check was wrong about code that was
right. That question is now asked of the composite directly, and only
the hole boundaries carry the stride's error.

### 3.9.2 The fix: a current band

`/v2/pws/observations/current` for a pinned station, and
`/v3/wx/observations/current` for a config with none.

**They are not equivalent, measured rather than assumed.** The station
endpoint carries `metric.precipRate`, an instantaneous rate, which is
what the model stores. The geocode one carries no rate at all -- only
`precip1Hour` and its longer siblings, which are trailing accumulations
and would become a plausible wrong number if divided into a rate for
now. So the geocode fallback yields **temperature and no rain**, which
is exactly what independently-optional fields are for.

Note also that the station's current endpoint spells temperature
`metric.temp` where the history endpoint next door spells it
`metric.tempAvg`. Same API, same quantity, two names.

### 3.9.3 The one place a measurement is knowingly extended

A current observation is an instant, and covering a hole needs a span.
`bbq_current_validity_s` is 15 minutes: the width of the gap it exists
to close, so nothing longer is ever needed and anything longer would
only let a stale reading pose as a fresh one.

**This is a deliberate, narrow exception to sec 3.5**, and it is
written down rather than slipped in. Two things keep it honest:

- **It is capped.** A reading older than the window stops covering
  anything, so it can never quietly paper over an outage.
- **It ranks below the forecast bands** -- priority 150, under the
  nowcast's 200. This looks like a contradiction of sec 3.3's "measured
  beats forecast" and is that rule read precisely: the rule governs an
  instant a band *genuinely* covers, and a current observation
  genuinely covers only the moment it was taken. A forecast made *for*
  the following minutes beats a reading stretched *into* them.

Ranking it there makes the extension provably harmless. The current
band paints only where nothing else reaches -- the hole it was added
for, and nothing else.

### 3.9.4 What it fixes, and what it does not

**Verified on live data, in the failing condition rather than beside
it.** A run with a station pinned caught the observed band lagging 26
minutes, leaving 13:30Z to 13:56Z uncovered by everything else -- and
the composite answered "now" from the current band. The same happens in
a geocode-only configuration, where the nowcast's first sample is still
in the future and the current reading is what sits in the gap.

**It anchors the present. It does not backfill the lag.** The current
reading is a single point at roughly now, so the stretch of time
between the observed band's last row and that reading stays uncovered
-- a hole of up to about fifteen minutes sitting in the *recent past*
rather than at now.

That residual is left alone deliberately. It is genuinely a period this
program has no data for yet, the next refresh fills it as the history
endpoint catches up, and the alternative -- stretching the observed
band's last sample forward to meet the current reading -- is the
inventing-data option sec 3.5 already rejected.

### 3.10 Rain chance is a third quantity, in its own panel

`precipChance` is not a flavour of the rain rate and is stored
separately. **A rate says how hard it would rain; a chance says whether
it will.** Ten percent of heavy rain and ninety percent of drizzle are
different afternoons, and neither number can be recovered from the
other.

It is absent on the measured bands, and correctly so: an observation
has no probability attached, it either rained or it did not. The
independently-optional fields carry that without a special case.

**Drawn in its own panel below the main plot**, sharing the x axis and
the hour banding -- the stacked shape WU's own dashboard uses. A third
line in the main plot would have to hang off one of the two axes there,
which would make a scale mean two things.

Two rules it does not share with the main plot:

- **Fixed 0 to 100, never scaled to what is visible.** A percentage
  means the same everywhere, and rescaling would make a dry day's five
  percent look like a downpour.
- **Downsampled by maximum**, like the rate and for the same reason
  (sec 3.5). A column holding one quarter-hour at eighty percent and
  three at ten is a column where it might well rain; meaning that down
  to twenty-eight hides the spike somebody planning an afternoon is
  looking for.

The colour is WU's rain-family cyan, taken from the accumulation series
on their dashboard. Said plainly rather than left as an implied
measurement: their dashboard plots observations and so has no
precipitation-chance panel to sample, so this is a WU colour used for a
WU-adjacent purpose, not one measured from the thing it draws.

### 3.11 Interpolation, as a choice of methods

**Built.** Four methods in `src/graph/interpolate.cpp`, chosen from a
drop-down that repaints immediately, with the real samples markable on
the curve.

**Some already exists.** The temperature line interpolates linearly
between samples at render time (sec 3.8.1), because sec 3.1 makes
temperature a value AT an instant and a staircase asserts something
false. What is wanted is that generalised and made a choice -- smoother
curves where they are honest, and off where they are not.

### 3.11.1 The rule it must not break

**Interpolated values live in the renderer and never in the series.**

This is the whole of it. Once an interpolated point is in a
`bbq_series`, nothing downstream can tell it from a measured one -- not
the graph, not sec 7's scoring, not a future export. Sec 3.5 forbids
upsampling for exactly that reason, and this feature is the shape most
likely to violate it by accident, because "just fill in the gaps in the
data" is the obvious implementation and the wrong one.

The existing temperature interpolation obeys this: it happens inside
one column reduction and nothing keeps it.

Two more that follow from rules already settled:

- **Never across a gap** (sec 3.6). A curve drawn through missing data
  is a claim about a period nothing reported on, and a smoother curve
  makes it more convincing rather than less.
- **Never across a provider seam** (sec 3.7). A step where two
  providers disagree is information; smoothing it is inventing an
  agreement.

### 3.11.2 The methods

| Method | Shape | Where it is right |
|---|---|---|
| Step | hold each value across its span | a mean ACROSS a span -- rain rate, rain chance |
| Linear | join consecutive samples | a value AT an instant, and when you want no opinion at all |
| Monotone (PCHIP) | smooth, cannot overshoot | the safe default |
| Akima | smooth, local | plateaus beside sharp changes -- this data's actual shape |
| Akima (modified) | as Akima, flatter on flat runs | long runs of equal samples |
| Natural cubic | C2 continuous, smoothest | dense well-behaved data |
| Catmull-Rom | C1 centred difference | what many tools call "spline" |

**Akima earns its place on this data specifically.** A cubic spline
decides every tangent from the whole curve, so one sharp change ripples
outward and wobbles regions that were flat. Akima decides each tangent
from only the four nearest slopes, so a change stays local -- and this
data is exactly plateaus beside fast drops. Rendered side by side, the
natural cubic visibly rings through the stepped evening while Akima
follows it and draws the morning ramp as the straight line it is.

**Two of them can overshoot: natural cubic and Catmull-Rom.** They will
draw a value beyond every sample -- confirmed by building it, when
switching to Catmull-Rom moved the temperature axis from 31/27 to 32/26
to fit numbers nobody reported. Marking the samples does not excuse
that, because the invented extreme sits BETWEEN the marks. They are
kept because they are legitimate choices for well-behaved data and
because seeing what the others avoid is worth something, but neither is
the default.

### 3.11.2.1 A correction, and a warning about names

**What this project called "natural cubic" was Catmull-Rom.** They are
different curves -- Catmull-Rom is a C1 Hermite spline from centred
differences, a natural cubic is C2 and solved from a tridiagonal
system -- and both are now offered under their own names.

The mistake is worth leaving recorded. It produced a curve that looked
plausible under a label that was wrong, which is this document's
recurring failure in a new place: nothing about the picture said the
name did not fit it.

### 3.11.2.2 The steps are in the data, not the curve

Worth knowing before reaching for a smoother method. **Weather
Underground reports whole degrees**, so a slow overnight fall arrives
as a staircase of one-degree drops, and no interpolator removes it --
every method here passes THROUGH the samples, so quantisation in equals
quantisation out. Smoothness only changes how the corners are rounded.

Removing it needs an APPROXIMATING method, which does not pass through
the samples at all. That is built and is sec 3.11.4.

### 3.11.3 Marking the samples is what makes it honest

This is the mechanism, and it resolves the tension rather than dodging
it. **Every real sample is markable on the curve**, so a smoothed line
never has to be trusted on its own: the marks say where the data is and
the curve says what is drawn between.

It also settles sec 3.11.1 from a second direction. Interpolated values
have to stay in the renderer, because marking the samples requires
knowing which points ARE samples -- and a series that has absorbed its
interpolation can no longer tell you.

Two things follow for whatever gets built:

- **The method is visible, not just active.** Which interpolation is in
  use belongs where the reader can see it, for the same reason sec 2.4
  puts the fetch time on the display.
- **The gap and seam rules survive every method.** No interpolation,
  however smooth, crosses missing data (sec 3.6) or a disagreement
  between providers (sec 3.7). Those are breaks in the data, not
  roughness in the curve.

### 3.11.4 Rounding, which is approximation and says so

**A separate control, not a seventh curve**, because it makes a larger
claim than any interpolation does: the line stops passing through the
data.

What it is for is the corners. Quantised whole degrees give the curve a
hard knee at every step, and no interpolator softens one -- they all
pass through the samples, so the knee is in the data. Rounding moves
the knots the curve is fitted to, and the corner opens out.

**Local linear regression with Gaussian weights.** A plain weighted
average would be fewer lines and is worse in two ways that matter here:
it flattens peaks, and it bends towards the interior at both ends,
inventing a turn at the edge of the window. Fitting a LINE rather than
a level through each neighbourhood removes both.

Weighted by real distance rather than by neighbour count, because the
bands sample at roughly 5, 15 and 60 minutes -- counting neighbours
would round an hour of the hourly band as hard as five minutes of the
observed one. The window is bounded at three sigma, which keeps it
linear in the sample count rather than quadratic.

**The setting is a duration**, because that is what it means: how wide
a corner may be. Not an abstract strength nobody can reason about.

### 3.11.4.1 What keeps it honest

**The marks do not move.** Rounding is applied to a copy of the knots
that only the curve is fitted to; the columns keep their measured
values, so the sample dots stay where the data is and the readout still
reports what a provider said.

So the curve visibly departs from the dots as rounding increases, and
that gap is the claim being made out loud. A smoothed line with no
marks would be a graph asserting readings nobody took.

### 3.11.5 The defaults, and why they are not the safest ones

**Akima, with thirty minutes of rounding.**

Neither is the most conservative choice available, and that is
deliberate. Monotone cannot overshoot and no rounding cannot mislead,
so a timid default would have picked those -- but the graph would then
open on a staircase of one-degree steps, which is an artefact of WU
reporting whole degrees rather than anything the weather did. A default
that shows the reporting instead of the weather is not the safe option,
it is the wrong one.

Akima because the shape here is plateaus beside fast drops, and it
keeps a sharp change local instead of ringing the flat parts around it.
Thirty minutes because that is roughly the width of a quantisation step
at this scale, so it opens the knees without moving the line far from
the dots.

Both are visible in the controls, reversible in one click, and drawn
over marked samples -- so what the data actually says is never hidden
by either. That is the condition that makes a non-conservative default
acceptable: it is a presentation the reader can see and undo, not a
claim they cannot check.

### 3.12 The readout snaps to samples

Hovering the graph shows a readout: the time, the values, and which
band and provider produced them.

**It snaps to the nearest real sample and never reports the cursor's
position.** That is the whole design. The curve between samples is
drawn rather than measured (sec 3.11), so reporting the value under the
pointer would put an interpolated number in a box that looks like a
reading -- wrong while looking fine, which is the failure this document
keeps naming. Snapping means every number in the box was reported by a
provider.

Naming the band and provider is what sec 3.4 kept the series separate
for. A reading that cannot say where it came from is most of the way to
a merged array.

The time comes from the sample's own timestamp, not from the column's
position. Deriving it from pixels put 06:00 samples in the box as
05:59 -- small, and exactly the kind of number that looks measured
because everything beside it is.

### 3.12.1 The axis is in the location's clock

**Settled: times are labelled where the weather is, not where the
reader is.** The graph also names the clock it is using, in the corner
by the time axis, because a fix nobody can see has not fixed the thing
that was wrong -- which was a graph quietly showing somebody else's
hours.

The zone is a property of the location, so it lives on the series and
the composite picks one:

- **A pinned station gives its own IANA name.** `tz` arrives in every
  PWS row, measured in sec 2.6.7.1 and noted then as a bonus. This is
  where it earned its place.
- **Without one, the forecast bands give an offset.** Every
  `validTimeLocal` carries the location's UTC offset, which is right
  for the moment it describes.

**The named zone is preferred over the offset**, and the difference is
not pedantry: an offset is a snapshot of a clock, correct until the
location changes it. `Europe/Stockholm` stays right across a
daylight-saving change; `+02:00` is wrong from that Sunday onwards, and
wrong in a way that looks like nothing at all.

Where neither says, the viewer's own clock is used and the corner says
`local` -- an honest admission rather than a guess.

The fetch time in the status line stays in the viewer's clock
deliberately. It answers "how long ago did this machine ask", which is
a question about the reader rather than about the weather.


## 4. The tray

### 4.1 Which desktop, answered by running it

`QSystemTrayIcon` on Linux means StatusNotifierItem. KDE Plasma serves
it natively; a bare GNOME session does not, and there the tray half of
sec 0's brief is simply absent.

**This was open until the applet was run on the real display, where the
tray is available.** It stayed open through the whole tray design, and
being unable to answer it turned out not to block anything -- because
the honest response to "there might be no tray" is a fallback, and the
fallback is worth having whatever the answer is.

Two things came out of it that a decision on paper would not have
produced:

- **Quit follows the tray.** A tray applet should not exit when its
  window closes, but that holds only where there IS a tray; without
  one the window is the entire interface, and keeping the process alive
  after it closes leaves something running with no way to see, reach or
  quit it short of `kill`. The setting is decided from
  `is_available()` before the window is shown, so the two halves cannot
  disagree.
- **The fallback is exercised constantly.** The offscreen platform
  reports no tray, so every rendered shot in this project takes the
  no-tray path. It is the most-tested branch in the program rather than
  a courtesy nobody runs.

### 4.2 The tray shows the weather

**The icon is the current temperature**, and it turns red when the data
behind it has gone stale.

That is sec 2.4's other half finally being done rather than asked for.
The document has said since the first commit that a failed refresh must
be visible *in the tray icon as well as in the window*, and the icon
was a placeholder dot for thirty commits -- which made the systray half
of sec 0's brief a coloured circle.

**Stale is two hours**, which is two missed hourly refreshes. The
slowest band legitimately reaches an hour old between fetches (sec
2.5.1), so anything tighter would cry stale on a working applet.

The tooltip carries what a number cannot: which band the reading came
from, how old the oldest band is, and the grilling verdict. **Stale is
said in words there as well as in colour**, because a colour alone is a
claim only somebody who already knows the convention can read.

### 4.2.1 The icon is measured, not guessed

The digits are sized by measuring the text and shrinking until it fits,
rather than by a fraction of the icon height.

The first version set the pixel size to the icon's height, which is the
height of the em box and not of the digits, so "20" rendered a size too
large and lost its top and bottom to the edges. Found by rendering the
icon to a file and looking at it -- `--tray-icon` exists for that,
because a tray cannot be screenshotted from here and the icon is now
the applet's main surface.

## 5. Build

- `make` builds the app. It does not build tests.
- `make test` builds and runs the suite. It is built by that target and
  by nothing else, so a plain build stays fast -- which is paid for by
  never judging a test from a binary the target did not rebuild.
- `make check` is what must pass before committing.
- `make style` gates indentation and the document against the tree.
- `make hooks` installs the commit-msg hook from `tools/hooks/`.
- `BUILD_DIR` is the build output directory, and is settable.
- `-Os`, per the global rule. qmake's release default is `-O2` and is
  applied by the *generated* Makefile, so `bbq-predictor.pro` is the only
  place saying otherwise takes effect.

### 5.0 The binary is replaced, never written through

`make` copies beside the target and renames over it.

Writing directly fails with `ETXTBSY` the moment the previous build is
running -- the kernel refuses to modify a file it is executing -- so
`make` broke for exactly as long as the applet was open, which is a
poor trade for a GUI meant to be left running while it is worked on.

`rename(2)` replaces the directory entry rather than the file, so the
running process keeps its inode and carries on with the old code while
the next launch picks up the new binary. It is also atomic: the
previous recipe could leave a *truncated* executable at that path if
the copy were interrupted, replacing a working program with a broken
one.

The temporary sits in the same directory deliberately, because rename
cannot cross filesystems and `/tmp` frequently is one.

### 5.1 Proposed: packaging

**A proposal, not a decision.** `build-and-commit.md` says choosing a
packaging mechanism is a convention change to raise rather than make in
passing, so this is what raising it looks like. Nothing below is built.

### 5.1.1 The mechanism is not really in question

`dpkg-buildpackage` driving a `dh`-based `debian/rules`, which is what
the global guidance prefers and what **every sibling that packages
already does** -- measured by reading each tree rather than taken from
the document:

| | flags | `debian/rules` |
|---|---|---|
| hydra, situ, netcfgd, fuzzypickles, fmake, beerssh | `-b -us -uc` | `%:` then `dh $@` |
| apt-emerge | `--build=binary --no-sign` | same |
| raidcfgd | checks for the tool first | same |

Seven of eight spell the flags identically and the eighth asks for the
same thing in long form. There is no live disagreement to settle here.

The rules files are 4 to 12 lines of actual content, and two overrides
recur: `dh_auto_install` calling `make install DESTDIR=...`, and
`dh_auto_test` disabled because the suite wants a build tree the
packaging does not keep. **This project would need both**, and its
`install` target already honours `DESTDIR`.

### 5.1.2 What is actually unsettled

Three spellings differ, and all three belong to the group rather than
to this project:

- **Where artifacts land.** `$(BUILD_DIR)/deb` in hydra, situ and
  raidcfgd; `$(BUILD_DIR)` in fmake and beerssh; `dist/` in apt-emerge.
  `$(BUILD_DIR)/deb` reads best -- it keeps build output under one
  variable and says what the directory holds.
- **How they are collected** out of the parent directory
  `dpkg-buildpackage` writes to. raidcfgd's has learned the most: it
  moves `name_*` **and** `name-*`, having found that the first version
  left the `-dbgsym` package behind for ever.
- **Whether `dh` is now *the* convention** rather than what everybody
  happens to do. That is the question `build-and-commit.md` leaves for
  the holder, and eight trees agreeing is a good moment to answer it.

### 5.1.3 What this project would need

    debian/control        one binary package; dh_shlibdeps finds Qt
    debian/rules          %: dh $@, plus the two overrides above
    debian/changelog      gated against VERSION, as situ does
    debian/copyright      see below -- this is the blocker
    debian/source/format  3.0 (native)

**AppStream metainfo is on that list too, and carries the same
blocker.** Without `packaging/$(APP_ID).metainfo.xml` the applet is
invisible to every software centre, which beerssh supplies and this
project does not. It cannot be written yet: AppStream requires
`<project_license>`, so producing one means stating a licence, and that
is the decision sec 8 says is not mine.

Two smaller pieces of desktop integration, one done and one not:

- **`StartupWMClass` is set**, measured with `xprop` on the running
  window rather than guessed. Without it the running window is not
  associated with its launcher and appears as a second, unnamed entry in
  the dock -- beerssh carries the same line and the same warning.
- **`QGuiApplication::setDesktopFileName()` is not called**, which is
  the Wayland half of that same association. It is a one-line change and
  is deliberately not made here, because it cannot be checked without
  building and an unverified change is not the kind this project
  commits.

Drafting this exposed one thing packaging would have shipped: the
desktop entry had named `Icon=se.vibes.bbq-predictor` since the first
commit and no icon existed. Fixed rather than left for the package to
carry -- `packaging/$(APP_ID).svg`, installed to
`share/icons/hicolor/scalable/apps` as beerssh does it.

### 5.1.4 The blocker is the licence, and it is not mine to move

`debian/copyright` carries a `License:` field. **This project has no
licence, deliberately (sec 8).**

The directive is explicit that a lint gate demanding a field is a
finding to report and not authority to decide, and that a blank field
must not be filled to quiet it. raidcfgd is the worked example and its
wording is the model: a `License: none-chosen` paragraph saying all
rights are reserved, that the absence is deliberate rather than an
oversight, and that **until a choice is made the package is not
distributable** -- the packaging exists so it can be built and
installed locally.

So packaging is possible now, and it produces something installable on
this machine and nowhere else. That may be exactly what is wanted; it
should be a decision rather than a discovery.

### 5.1.5 And a second thing to decide before distributing

Independent of the licence: a `.deb` is a distribution artifact, and
sec 2.2 records that the scraper violates Weather Underground's terms.

Building one locally changes nothing. Handing one to somebody else is a
different act from publishing source that carries a warning -- a
package installs and runs without anybody reading `project.md` sec 2
first. Worth settling alongside the licence rather than after.

### 5.1.6 Looking at it is the method

**Every layout defect in this project was found by rendering a picture
and looking at it.** Not one was found by reading the code, and none of
them would have failed a test that checked numbers:

- the temperature drawn as a staircase, which asserts that temperature
  is constant for an hour and then jumps (sec 3.8.1);
- the first fix for it doing nothing, because `range()` returns
  overlaps (sec 3.8.1);
- the radar band ending early when it replaced a longer one, seen in
  the provenance ribbon (sec 2.9.1);
- two bands sharing a colour in the strip whose only job is telling
  them apart (sec 2.10);
- the tray icon's digits clipped, because a font's pixel size is the em
  box and not the glyph (sec 4.2.1);
- the station field squeezed to `stati...` -- the one field the applet
  cannot work without (sec 2.6.6);
- a control reading "Off" over a visibly rounded curve, twice.

So the diagnostics exist to make looking cheap, and they are part of
the build rather than scratch work:

| Flag | What it renders |
|---|---|
| `--fetch-once` | every band through the feed, with holes and probes |
| `--shot FILE` | the window, headless |
| `--tray-icon FILE` | the tray icon, which cannot be screenshotted |
| `--cursor N` | parks the readout so a shot shows it |
| `--interp M` | a curve method, for comparing them side by side |
| `--smooth SECS` | a rounding radius, likewise |
| `--layout L` | the desktop or mobile shape, at a phone's proportions |

All of them answer before any widget is built or run under the
offscreen platform, so none needs a display.

### 5.1.7 The sanitizer build existed and had never been run

`make SANITIZE=1` was wired up early and nothing had ever executed what
it produced. The first run found two faults and a third on the way in,
which is a poor argument for building a gate and a good one for running
it.

**A live stack-use-after-scope, in the screenshot path.** The `--shot`
machinery kept a `bool taken` so the picture is grabbed once whether the
feed settles or the wall-clock timer fires first, and that bool was
declared inside the block that set the timers up. The lambdas reading it
run from timers during `app.exec()`, long after the block ended, so
every shot was deciding whether it had already been taken by reading a
dead stack slot. Undefined behaviour that worked: the slot generally
still held the right byte, which is why every picture this project
reasoned about came out correct. `main`'s own frame is alive for the
whole of `exec()`, so the fix is to declare it there.

**A leaked `QMenu`.** The tray's context menu is necessarily parentless
-- `QMenu` is a `QWidget` and `QSystemTrayIcon` is not, so there is no
parent to give it -- and `setContextMenu` does not take ownership. It
was never deleted. Nine and a half kilobytes across seventy-two
allocations, the menu and everything Qt hangs off one. A destructor owns
it now, and the sanitized build reports nothing at all.

**And `BUILD_DIR` did not isolate.** Found by being bitten rather than
by reading: `make BUILD_DIR=/tmp/bbq-asan SANITIZE=1` -- the README's own
example -- copied the sanitized binary over `./bbq-predictor`, where it
is slower, behaves differently, and looks identical. The whole reason
`build-and-commit.md` requires a settable build directory is so that an
isolated build cannot clobber a plain one, so the guarantee was exactly
backwards. The copy into the tree now happens only for the default
directory; anything else stays where it was built.

None of the three was reachable by reading alone, and all three sat
behind a target that had been present for weeks. **A gate nobody runs is
not a gate**, which is the same lesson as the four sessions of not
building, arriving from the other direction.

### 3.13 The graph was read end to end, and held

A thousand lines, the largest surface with no automated coverage, read
for the same faults the network paths turned out to have. **It held.**
No live defect. Recorded because "we looked and found nothing" is a
result, and an unwritten one gets re-looked-for.

What was checked and why each is safe: every divisor is guarded before
use -- `plot.width()` by the 20-pixel early return, `rain_high` by
starting at 1.0 and only growing, the temperature span by the
four-degree widening that runs before the padding. Column indices are
bounded by `plot.width()` on both the cursor search and the three draw
passes, and `columns` is filled with exactly that many entries. The
rain path's subpaths each open and close on the baseline, so the
implicit close a filled `QPainterPath` performs runs along the bottom
rather than across the data.

Three latent things, none of them reachable today, all worth knowing
before somebody makes them reachable:

- **`set_window()` has no callers.** It is superseded by `set_layout`,
  which takes the span from the layout metrics, and it is the only
  entry point that could set a zero span -- which would divide by zero
  in `seconds_per_pixel`. Left alone rather than deleted: removing
  public API is not a decision to take while reviewing something else.
- **The knot invariant depends on arithmetic nobody stated.** A column
  holding two sample starts would mean the readout's mean temperature
  carried the first sample's timestamp, which breaks sec 3.11.3's
  promise that every number in the box is one a provider reported.
  Measured rather than assumed: columns come out at 77 s on the desktop
  and about 130 s on mobile, against a finest band of 300 s, and the
  window cannot be dragged narrow enough to close the gap because the
  controls set a larger minimum than the graph does. **It is safe by a
  factor of two, not by construction.** A finer band, or a much wider
  span, would end that quietly.
- **The readout box flips left when it will not fit right**, and the
  flipped position is not clamped, so a widget narrower than the box
  would push it off the left edge. The comment says the box "never
  leaves the widget", which is true only because the flip is triggered
  by being near the right edge.

One thing was wrong and is fixed: the readout's text colour was written
into the painter as a literal while the box's background and border were
palette entries. This file's own opening note calls a constant beside
the palette "a third opinion nobody set", and that is exactly what it
was.

### 5.2 The suite, and what it is for

**Not coverage.** Every test asserts a claim this document makes, on
the reasoning that a claim nothing checks is a claim that quietly stops
being true. `make check` is style plus tests, which is what the failing
`make test` existed to become.

Three binaries rather than one, so a failure names its subject in the
target that failed: interpolation, model, readers. None of them links
the widgets, so the suite needs no display.

The properties worth naming:

- **Monotone never overshoots**, checked densely across every span
  rather than at a few points, because an overshoot is a bulge between
  samples and sparse sampling is how you miss one.
- **Catmull-Rom DOES overshoot**, asserted deliberately. It is the
  reason monotone is the default, so if a change ever made it bounded,
  this test failing is the right way to find out.
- **Every method passes through its knots**, which is the line between
  interpolation and the rounding in sec 3.11.4.
- **Local linear smoothing reproduces a straight line exactly**,
  including at the ends -- the specific thing a weighted average gets
  wrong.
- **Rain outweighs warmth**, which is sec 7.2's multiply-don't-average
  in one assertion.
- **Rows arriving newest-first come back sorted**, which is sec 2.6.2's
  measured trap.

### 5.2.1 The suite was checked against being vacuous

Two deliberate sabotages, because a green suite proves nothing until it
has been seen to fail:

- Removing the `qpf` division failed
  `hourly_converts_accumulation_to_a_rate` and nothing else. The
  fixture uses a TWO-hour step precisely so a reader that forgot to
  divide cannot pass on one-hour data.
- Setting the observed band's priority below the forecast's failed
  `measured_beats_forecast` and nothing else. Both bands in that test
  step identically, so resolution cannot decide it -- only the declared
  table can.

The suite also refuses to run over zero binaries, and that guard fired
for real on its first run: the binaries built into `build-tests/`
rather than the per-test subdirectories the glob expected, so a
correctly-built suite matched nothing. Without the check it would have
reported success over an empty list.

### 5.2.2 It found something on its first run

The provider tests are weighted entirely towards conversions, because
that is what a second provider is: the same quantities in somebody
else's units, spelling and clock. Every such mistake fails silently --
a wind out by 3.6, a rain rate out by 4, a series shifted by hours --
and none of them produces an error or an obviously wrong curve.

The first run failed. Open-Meteo's reader discarded a *wrong* timezone
name and accepted an *empty* one, because Qt returns the local zone for
an empty id and calls it valid. That is recorded in sec 2.10.1 and
fixed; the point here is that nothing else would have caught it. The
graph would have looked entirely normal, in the reader's own timezone,
which is the failure this project has now removed twice.

## 6. Style

Three rules -- `snake_case`, tabs to indent and spaces to align,
lowercase filenames -- with the detail in `code-style.md` at the repo
root.

Qt's own API is called exactly as it is spelled (`setWindowTitle`,
`paintEvent`); names this project introduces stay `snake_case` with the
`bbq_` prefix where they reach the linker.

**One shape came up repeatedly and is not settled**: a statement
continued across lines without an open bracket -- an operator-led
`<<` chain, a wrapped signature inside an anonymous namespace, a
`.arg()` chain. The gate wants an extra indent level there, and
`code-style.md`'s worked example passes it verbatim, so the gate is not
disagreeing with the documented rule; the shape is simply not among the
settled exceptions. It has been restructured around six times rather
than settled once. The finding, and what was checked, is in this
project's `code-style.md`.

## 7. The grilling prediction

**Built.** The forecast is scored for grilling weather, the good
stretches are shaded on the graph, and the best one is named in words
above it.

### 7.1 The preferences are preferences

Everything else in this document was measured against a real response.
**None of sec 7 can be**, and the code keeps the numbers in one struct
rather than scattered through the arithmetic so the two are hard to
confuse. They are one cook's answers and another would set them
differently.

| Factor | Policy | Because |
|---|---|---|
| Temperature | zero at 5 C, full marks by 25 C, **no upper penalty** | warmer is always better; only cold counts against |
| Rain rate | graded to zero at 2 mm/h | drizzle is survivable, a downpour is not |
| Rain chance | tempers the score at half weight | the rate is the promise, the chance is the confidence around it |
| Wind | fine to 15 km/h, ruinous by 45 | it steals heat, blows smoke and carries embers |
| Hour | prime 16-21, usable 11-23, otherwise 0.15 | late afternoon into the evening is the point |
| Length | 2 h to be offered, 3 h to be preferred | both were asked for and they are not in conflict: one is a floor, the other a ranking |

### 7.2 The factors multiply

Not an average, and this is the one structural decision in the scoring.

**An averaging score recommends grilling in the rain because it is
warm.** Multiplying means a window has to be decent in every respect
rather than trading a downpour against a pleasant temperature, which is
how anybody actually decides whether to light a fire.

Three smaller consequences, each chosen rather than fallen into:

- **A missing field is neutral, not pessimistic.** No temperature
  reading is not the same as freezing, and scoring it as cold would
  bury good windows for want of a field.
- **The small hours score low, not zero.** Somebody grilling at two in
  the morning has reasons of their own; a zero would hide genuinely
  fine weather rather than merely rank it below the evening.
- **A gap ends a window rather than failing it.** A stretch nobody has
  a forecast for is not one to recommend, and not one to condemn
  either -- the same reasoning as sec 3.6.

### 7.3 Longer is better, but only up to a point

A six-hour window is not twice the afternoon a three-hour one is; it is
the same afternoon with more of it spare. So length raises the ranking
up to the preferred three hours and does nothing after, which stops a
long mediocre stretch outranking a short excellent one.

### 7.4 What it needed from the rest of the project

**Wind, which the model did not carry.** Added to the sample and read
from all four bands -- and it set the same trap the temperature did:
`windSpeed` in the forecasts and in the station's current reading,
`windspeedAvg` in the station's history. Three spellings of one
quantity in one API, failing the silent way, as a field that is simply
never populated.

**The location's clock**, so "evening" means evening where the fire is
(sec 3.12.1). Scoring the hour of the day would otherwise have been
scoring the reader's day rather than the weather's.

### 7.5 Absent rather than cheerful

Where nothing qualifies, the line says so. **"No grilling window in the
next three days" is a useful answer**, and inventing a mediocre one to
fill the space would be the same failure as a graph that draws through
missing data.

## 8. Licence

**There is none, and that is not an oversight.** A licence is the
copyright holder's to choose and nobody else's, an absent one leaves
every option open, and no licence is better than a wrong one. Recorded
explicitly here so the absence does not read as a gap for a later pass to
close.

### 8.1 Published, by the holder's decision

This repository was local-only for most of its life, and that was a
decision rather than an oversight -- recorded here so a harmonizing
pass would not read the absence as drift and close it, since most of
the sibling projects carry a `github.com/funklord` remote.

**The copyright holder has since decided to publish it**, and that is
the one thing that moves it. The reasoning that kept it local is not
withdrawn so much as overruled by the person entitled to overrule it.

What that reasoning was, and why it still matters to know:

The tree contains a working scraper for Weather Underground's API key,
and sec 2.2 states in writing that using it violates their terms of
service. Keeping that on one machine and publishing it are different
acts. Publishing does not make sec 2.2 less true, and it is the reason
sec 2.2 is written as plainly as it is -- anybody arriving at this
repository meets that paragraph before they meet the code.

**Whether the repository is public or private is the holder's to set**,
and it is set on the hosting side rather than anywhere in this tree.
Nothing here depends on the answer; sec 2.2 is the same warning either
way.

### 8.1.1 No key has ever been in a commit

**Checked across the whole history before publishing, not just across
the working tree**: no 32-character hex string appears in any commit
this repository contains.

That is a property to keep rather than a fact to have established once.
A scraped key is somebody else's credential, git history is the one
place a mistake cannot be quietly deleted, and this repository now has
a remote -- so an accidental paste is published the moment it is
pushed.

The extraction pattern is documented in sec 2.6.1 and the key itself
is runtime state that is never written down (sec 2.3), which is what
makes the property easy to hold: there is nowhere in the tree a key is
supposed to live.

## 9. What has been decided, and what has not

### 9.1 Settled

**The data.**

- The key is scraped, with its consequences as requirements (sec 2)
- Multi-provider from the start, WU first; the internal series is ours
  and every provider translates into it, including WU (sec 2.7)
- Resolution is what qualifies a provider, not convenience (sec 2.8)
- The WU endpoints for every band, observed rather than documented
  (sec 2.6), and the key taken from an embedded request URL rather than
  the config blob (sec 2.6.1)
- MET Norway serves the radar band and Open-Meteo the extended one,
  each as a band of its own rather than a replacement, and each silent
  where it does not reach (sec 2.9, sec 2.10)
- The station is user-chosen and pinned; the geocode derives from it
  and is cached; an override wins and is shown (sec 2.6.5 to 2.6.7)
- Config is QSettings INI under `AppConfigLocation`, and the command
  line overrides the run rather than the configuration (sec 2.6.6,
  sec 2.6.8)
- Refresh is per band, on the clock, backing off on attempts rather
  than successes (sec 2.5.1)

**The model.**

- A sample is a span, not a point; canonical units C, mm/h, km/h,
  epoch UTC (sec 3.1)
- Rain is stored as a rate, so one axis works; rain chance is a third
  quantity and not a flavour of it (sec 3.2, sec 3.10)
- Precedence is declared, and measured beats forecast (sec 3.3)
- The composite is a view over the series, never a merge, because
  provenance is required (sec 3.4)
- Never upsample; downsample rain and chance by maximum (sec 3.5)
- Gaps are drawn as breaks, never interpolated across, at a threshold
  since checked against six bands rather than guessed (sec 3.6)
- Joins disappear through normalisation, never blending; a surviving
  step is information (sec 3.7)
- A current band anchors the present, capped and ranked below the
  forecasts so the extension can only fill a hole (sec 3.9)

**The graph.**

- QPainter over QtCharts, confirmed by building it (sec 3.8)
- The palette measured from WU's own dashboard, at the cost of
  following the desktop into dark mode (sec 3.8.2, sec 3.8.3)
- Seven interpolation methods chosen live, with the samples markable
  and monotone the default because it cannot overshoot (sec 3.11)
- Rounding is approximation and says so: the marks stay on the data
  while the curve leaves it (sec 3.11.4)
- Akima and thirty minutes as defaults, which are not the most
  conservative choices and are visible and reversible (sec 3.11.5)
- The readout snaps to real samples; times are in the location's clock
  and the graph names which clock (sec 3.12)

**The applet.**

- The tray shows the temperature and reddens when stale; quit follows
  tray availability (sec 4.1, sec 4.2)
- Desktop and mobile are two shapes rather than one that scales; the
  device supplies the default and the configuration overrules it, and
  the layout never resizes its own window (sec 10)
- The Android build runs on the shared vocabulary in `tools/android.mk`,
  spread from `~/.claude/tools/` like `style_gate.py` (sec 11)
- Grilling windows scored and shaded, preferences gathered in one place
  and marked as preferences (sec 7)

**The build.**

- The binary is replaced by rename, never written through (sec 5.0)
- Tests are built by `make test` and by nothing else; `make check` is
  style plus tests (sec 5.2)
- Looking at a rendered picture is the method, and the diagnostics
  exist to make it cheap (sec 5.1.1)
- No licence, deliberately and still (sec 8)
- Published by the holder's decision, having been deliberately local
  before it (sec 8.1)

### 9.2 Open

Each of these needs a decision rather than a drift. None of them blocks
anything.

- **A dark-desktop variant**, given the measured palette is fixed and
  light. A question about how the applet is actually used rather than
  one this document can answer (sec 3.8.3)
- **Packaging**, now drafted as a proposal rather than a gap (sec 5.1).
  The mechanism is not in question -- every sibling that packages uses
  `dpkg-buildpackage` with `dh`, seven of eight with identical flags.
  What needs deciding is where artifacts land, whether `dh` is now *the*
  convention rather than what everyone happens to do, and two things
  that are the holder's alone: that `debian/copyright` needs a
  `License:` field this project deliberately cannot supply, and that a
  `.deb` is a distribution artifact where the source is not
- **Finishing the Android build.** It stops at a `compileSdk 35` floor
  set by Qt's own AndroidX dependencies against an SDK whose newest
  platform is android-33 (sec 11.2). One `sdkmanager` line fixes it,
  and it is a download and a licence acceptance on a shared SDK rather
  than anything in this tree
- **Whether the three projects that shipped for Android first adopt
  `tools/android.mk`.** Their target names differ from the agreed ones
  in ways somebody has to decide about rather than sweep, so it belongs
  to a deliberate cross-project pass
- **Whether to take Open-Meteo's quarter-hourly block where it is
  genuine.** It is real over North America and central Europe and
  interpolated elsewhere (sec 2.10.4), so using it correctly means
  probing model coverage for the configured point and choosing per
  location. Worth it only for someone inside those domains, which this
  station is not

### 9.3 Raised elsewhere

- **Statement continuations without an open bracket** have been
  restructured around six times rather than settled once. The gate is
  not disagreeing with the documented rule -- that was checked -- but
  the shape is not among the settled exceptions. It belongs to the
  global `code-style.md` rather than to this project, and the finding
  is recorded in this project's copy (sec 6)

## 10. Desktop and mobile are two shapes

**Not one layout that scales.** A phone has no pointer to hover, no
tray to sit in, a screen that is tall rather than wide, and a finger
instead of a cursor -- so the controls, the time window and the readout
each want a different answer rather than a bigger one.

The numbers live in one struct (`bbq_metrics`) so the two shapes can be
compared by reading it instead of hunting through paint code. Every
mobile value has a reason that is not "bigger":

| | Desktop | Mobile | Why |
|---|---|---|---|
| Time window | -3 h to +21 h | -2 h to +10 h | the same span on a narrower screen is the same data drawn thinner, and resolution is this graph's whole claim |
| Tick step | 3 h | 6 h | three-hourly labels collide at that width, and a collided label is worse than a missing one |
| Controls | one row | two columns | ten things in a row on a phone are two millimetres wide each |
| Control height | natural | 44 px | a finger is not a cursor |
| Marks and line | 2.0 / 2.0 | 3.0 / 2.6 | a phone is held further from the eye than a monitor sits from it |

### 10.1 The device decides, the configuration overrules

The default is compiled in: a build for Android is a build for a phone.
Asking the screen at runtime guesses wrong on exactly the machines
people notice -- a desktop with a touchscreen, a tablet in a keyboard
case, a phone plugged into a monitor.

So the setting is `auto`, `desktop` or `mobile`, and **a preference
somebody set is better evidence than a pixel count**.

### 10.2 The layout does not resize its own window

On a device the window is whatever the system gives it, so a layout
that resized itself would be arguing with the window manager about
something it does not own. The shape has to work at the size it is
handed -- which is also the only way to know it works.

Previewing the mobile shape on a desktop is a separate concern and
belongs to whatever is previewing, which is why `--layout` sizes the
window in the shot path rather than in the layout.

### 10.3 Three things a look found, that reading did not

- **The mobile shape was a font size.** `stack_controls` was set and
  only the spacing changed, so the controls stayed in one row and the
  "mobile" window came out *wider* than the desktop one. A flag that
  nothing acts on is worse than no flag.
- **A sentence was setting the width of a graph.** The verdict label's
  size hint is its text on one line, which held a phone-shaped window
  open to 1249 pixels. Wrapping was not enough; the horizontal hint had
  to be ignored outright.
- **The units label lost its units.** A right gutter chosen to look
  narrow clipped "1.0 mm/h" to "1.0 mm", then to "1.0 mm/l" at the
  second guess. It is measured from the widest string it has to hold
  now, the same way the tray icon's digits are (sec 4.2.1).

## 11. Android

The build is wired and harmonized. **It does not complete on this
machine**, for a reason that is a missing SDK package rather than
anything in the tree -- see sec 11.2.

### 11.1 The vocabulary is shared, the build rule is not

`tools/android.mk` carries the target names, the preflight, the
versionCode, the adb plumbing and the signature check. It is spread
verbatim from `~/.claude/tools/android.mk`, the same model as
`style_gate.py` and for the same reason: a copy in the repository is
reachable by CI, and a file under `~/.claude` is not.

    make android            debug build, installable on any device
    make android-aab        the Play bundle; needs a keystore
    make android-install    install on the attached device
    make android-run        install and launch
    make android-log        this app's log, and only this app's
    make android-uninstall  remove it
    make android-check      everything the build needs, checked by name

`harmonization.md` settled those names and this is the first project to
carry them. **`apk` is deliberately not among them**: netcfgd's `apk`
is Alpine's packaging command, and one word meaning two things across
sibling trees is how somebody eventually runs the wrong one.

Only the build rule lives in this project's Makefile, because qmake and
CMake differ and that difference is the project's own.

Three things came from other projects' scars rather than from thinking:
the ABI is read from the Qt kit rather than chosen twice; the
versionCode is derived from `VERSION`, because a hardcoded one allows
exactly one upload; and the artifact's signature is **read back from
the file**, because flags handed to Qt's generated Makefile are dropped
silently and beerssh shipped a "release build" carrying the Android
debug key.

### 11.1.1 A target nobody can find does not exist

`make android-install` puts the APK on the attached phone and has since
the fragment landed. It was asked for anyway, as `make adb`, because
**`make help` listed `android`, `android-aab`, `android-run` and
`android-log` and not it.**

The adjacency made it worse: `make install` sits a few lines above and
means the *desktop* one, so somebody scanning for "install" finds a
target that installs on this machine and reasonably concludes there is
no phone equivalent.

The names are not the problem and were not changed -- `harmonization.md`
settled them so a habit learned in one project is correct in the next,
and a synonym added here would start the divergence the fragment exists
to end. What was wrong is that the help advertised four of seven
targets, so the answer to "how do I flash this to a phone" was a
question rather than a line.

All seven are listed now, with a note that `make install` is the
desktop one.

### 11.1.2 The include stole the default goal

**For four sessions, a plain `make` built nothing.** It ran
`android-check`, failed for want of `QT_ANDROID_ROOT`, and stopped.

`include` is where make first sees a target, and `tools/android.mk` was
pulled in above `all` so that a project rule could use `ANDROID_ABI`
without redefining it. That placement is right; the consequence is that
the fragment's first target became the default goal. Nothing warned,
because nothing was wrong -- make did exactly what it is specified to
do.

What let it survive is the interesting half. It was introduced during
the Android work and the four sessions after it were documentation,
packaging and review, all under a standing instruction not to build. The
regression was not subtle or rare; it was simply never executed. **A
build that nobody runs is a build whose state is unknown**, and the
gap between "the tests pass" and "the tests were run" is exactly the
distance this project keeps insisting on elsewhere.

`.DEFAULT_GOAL := all` is stated above the include now, with the reason
attached. The shared fragment carries the warning too, at the top where
somebody copying it into a fifth project will read it -- it cannot fix
this for them, having no way to know what their default goal should be,
so saying so is the most it can do.

### 11.2 What stops it here

The C++ compiles for arm64 against NDK 25.2. Gradle then refuses:

    Dependency 'androidx.core:core:1.16.0' requires ... a newer compileSdk

Qt 6.10's Android support pulls AndroidX libraries that require
**compileSdk 35**, and this SDK has platforms up to android-33. No
setting in this tree changes that -- the platform has to be installed:

    sdkmanager "platforms;android-35"

That is a download, a licence acceptance and a change to a shared SDK
outside this repository, so it is recorded rather than done. The Qt kit
also names `android-ndk-r27c` as the NDK it was built against, and 25.2
is the newest here; it compiled, so that is a caution rather than a
finding.

**The preflight deliberately does not check for this.** It checks what
it can name -- kit, ABI, NDK, SDK, JDK -- and a compileSdk requirement
belongs to whichever AndroidX versions a given Qt release happens to
pull, which is not something a Makefile can know without asking Gradle.
Guessing it would be a check that goes stale silently, which is worse
than the honest Gradle error.

## 12. The history is permanent, the forecasts are not

Everything before this section was an applet with no memory. Each refresh
replaced the bands in place, closing the window discarded them, and the
graph could only ever show the window the layout gave it.

**Observations are now kept forever. Forecasts are kept only long enough
to be checked against them, and are then thrown away.** That is the whole
rule, and it is what keeps a permanent store bounded: the only thing that
grows without limit is the record of what actually happened, which is
also the only part that cannot be re-fetched.

### 12.1 What is kept, what is discarded

**Kept forever**: every measurement. The `observed` band's station rows
and the `current` band's instantaneous readings are both measurements of
the real world, so both are archived. A station reporting every five
minutes produces about 105,000 rows a year, which is a few megabytes --
a decade fits in a file nobody will notice.

**Kept until checked**: forecast samples, in a pending queue. A forecast
sample is held until the observation for the time it predicted arrives,
at which point it contributes one error to the statistics and the row is
deleted.

**Kept forever, but tiny**: the accumulated statistics. Sums and counts
rather than the samples they came from, so the table has a fixed size
however many years pass.

**Never stored**: the raw provider responses. They are a fetch away and
they are somebody else's format.

### 12.2 Why not a standard interchange format

Asked directly, and worth recording because the answer is not "there
isn't one" -- there are several, and none of them wants to be appended
to every five minutes.

- **CF-conventions NetCDF**, `featureType = timeSeries`, is the genuine
  meteorological standard for station series and is read by xarray, R
  and Panoply. It is built for write-once archives, has no Qt binding,
  and this machine carries only the runtime library with no headers.
- **WMO BUFR** is the observation *exchange* format: table-driven,
  designed for transmission between weather services, and nobody keeps a
  personal archive in it.
- **GRIB** is gridded model output. A point time series is the wrong
  shape for it entirely.
- **Parquet** is the modern analytics standard and compresses beautifully,
  but appending one row at a time is precisely what columnar storage is
  bad at, and it is not installed here.

**SQLite is a container rather than a rival to any of those.** It gives
indexed range queries -- the thing that makes scrolling back through a
year cheap -- crash safety, and correct behaviour when two copies of the
applet are open, which happens on this machine. Measured before
choosing: `Qt6Sql` and the `libqsqlite.so` driver are both already
installed, so it costs a line in the `.pro`, a packaging dependency and
one more module in the Android kit, and no new package here.

Exporting to CF-NetCDF or to CSV stays possible and is where
interoperability actually matters. **Being handed a standard file is
worth a great deal; being unable to append a row cheaply is worth
nothing.**

Settings stay in the INI file. The store is for measurements, not
preferences, and sec 2.6.6's reasoning about a config file being
something a person can open is unchanged.

### 12.3 Verification uses the field's own vocabulary

The request was for a "forecast deviation factor". That quantity already
has a name, and several relatives worth having beside it:

- **ME, the mean error, or bias** -- the signed mean of forecast minus
  observed. This is the deviation factor, and correcting a forecast with
  it is long-established practice under the name **MOS**.
- **MAE** and **RMSE** -- the unsigned magnitude of the error. These are
  not decoration. **Bias can sit at zero while a forecast is wildly wrong
  in both directions**, so a store that recorded only bias would report a
  useless forecast as a perfect one.
- **Lead time** stratifies all of them. A one-hour prediction and a
  ten-day prediction are not the same claim, and averaging them together
  produces a number that describes neither.

Adopting the standard names costs nothing and makes the output mean
something to anybody who knows the field.

### 12.4 Rain chance needs a different instrument

**A forecast of "40% chance" is not wrong when it stays dry**, so mean
error cannot score it. The standard tool is the **Brier score** with a
reliability curve: of all the occasions the forecast said 40%, did it
rain on roughly 40% of them?

That is a different accumulation -- probability bins against observed
occurrence, not a running sum of differences -- so it is a second table
rather than another column in the first. Rain is taken to have occurred
when the observed rate exceeds 0.1 mm/h, which is a threshold and is
labelled as one.

### 12.5 The correction is a band, not an edit

Once a bias is known the forecast could simply be adjusted, and it is
not. **The corrected curve is drawn as its own band**, in its own
colour, with its own entry in the provenance ribbon.

The reason is sec 3.4 and sec 3.11.3, which this project has held to
everywhere else: the graph shows what a provider reported, and a number
nobody reported does not get to look like one that was. A correction is
this program's opinion about somebody else's forecast. It can be drawn,
and it must say that is what it is.

It applies only where there is enough evidence to justify it -- a
handful of comparisons is noise, not a bias -- so a minimum sample count
gates it, and below that the band is simply absent rather than
uncorrected-but-drawn.

### 12.6 The give-up rule

A pending forecast whose valid time has passed without an observation
ever arriving must expire, or every outage leaks rows for ever. It is
deleted once its valid time is far enough in the past that no
observation is coming.

The queue is bounded a second way, deliberately. The same valid time is
re-forecast on every refresh, and keeping all of them would store the
same hour hundreds of times. **One forecast is kept per band, per valid
time, per lead-time bucket** -- the first seen in that bucket -- which is
exactly one verification sample per bucket and turns an unbounded queue
into a few thousand rows.

## 13. Navigating the graph

**Drag to pan, wheel to zoom about the cursor, double-click to return to
now.** The map and charting convention, chosen because it is the one
most people already have in their hands.

The window stops being a layout constant and becomes graph state. The
layout still supplies the *initial* span (sec 10), which is what a fresh
window should open at; after that the view is the user's.

Panning away from now stops the view following the clock. A graph that
kept scrolling itself while being read would fight the reader, and
double-click is the way back rather than a mode nobody can find.

### 13.1 What "snappy" costs

Sixty frames a second is sixteen milliseconds, and a full paint measured
7.6 ms with a single day of data. That is headroom, but not much, and
three things get worse with a permanent store rather than better:

- **`bbq_series::range()` scans linearly from index 0.** With a year in
  memory that is about 10^8 operations per repaint and panning would
  crawl. It becomes a binary search, which `at()` already uses on the
  same sorted vector.
- **Only the visible window is loaded.** The store is queried by time
  range against an index; the whole history is never in memory at once,
  however much of it accumulates.
- **Zoomed out, there are more samples than pixels**, so columns
  aggregate. That is already how `reduce()` works, and the store makes it
  load-bearing rather than incidental.

### 13.2 The marks stop when they would lie

Below one sample per pixel the sample marks are not drawn.

Sec 3.11.3 makes the dots mean "a real sample, here". Drawn at a zoom
where twenty samples share a pixel they would merge into a band of ink
that implies a density of measurement nobody made, which is the
graph-that-is-wrong-while-looking-fine this project keeps refusing.
Absent dots say "zoomed out"; smeared dots say something false.

**This is a judgement made without being asked, and it is reversible.**
The alternative worth considering is a min/max envelope per column, which
is honest in a different way -- it shows the spread the mean hides -- and
is more work than the first version of this needs.
