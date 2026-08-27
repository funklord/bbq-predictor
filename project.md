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

**How the fix is known to work.** `test/test_client.cpp` drives the key
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

### 2.3.3 One null timestamp claimed the band began in 1970

The forecast readers take time from `validTimeUtc` where it exists and
from `validTimeLocal` otherwise. The local path discards the whole band
on a single unparseable timestamp, and says why: a series missing an
arbitrary sample from its middle draws a gap that means nothing.

**The UTC path had no such guard**, and `QJsonValue::toDouble()` answers
0 for anything that is not a number. So one null became a sample at the
epoch -- and that is not a small wrong number. The series then claims to
begin on 1 January 1970: `begin_utc()` says so, the composite's coverage
says so, and the graph draws a gap of fifty-odd years beside an hour of
weather.

Both paths are equally strict now. **A guard on one branch of a choice
and not the other is decoration**: the input decides which branch runs,
and the input is the thing being defended against.

Found by reading rather than by a failure, and pinned by a test that was
watched failing first -- a null in the middle of an otherwise ordinary
hourly response.

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
rather than only reasonable in the source, and `test/test_feed.cpp`
asserts all three cases: derived does not survive, pinned does, and
re-setting the same id changes nothing. That last one matters because
the field writes on `editingFinished`, which fires when the box merely
loses focus -- treating that as a change would throw away a good
coordinate every time somebody clicked past it.

#### 2.6.7.5 Freshness is about a place, and the place can move

Dropping the stale coordinate (sec 2.6.7.4) was half the repair. The
other half is that every forecast band carries a freshness interval --
fifteen minutes for the nowcast, an HOUR for the hourly band -- and
those intervals answer "has the weather here changed since we asked".
They say nothing whatever about a different here.

So changing the station left the graph drawing the OLD town's forecast
until each timer ran out on its own, while the observed band beside it
already described the new one. That is the two-places-on-one-axis
failure of sec 2.6.7 arriving by the clock instead of by the
coordinate, and the earlier fix could not see it because it was looking
at the coordinate.

A real move now forgets the freshness of the bands that are asked for
BY COORDINATE -- nowcast, hourly, current-by-point, radar and extended
-- so the next round re-asks immediately. The station-keyed products
need nothing: `refresh()` re-fetches them unconditionally.

**Only a real move counts.** `set_geocode` is also how a coordinate
DERIVED from an observed response is stored, which happens on most
fetches, so resetting unconditionally would re-fetch every band every
time. The coordinates are compared before they are stored.

The backfill has its own reset for its own reason (sec 12.13): its
interval is a statement about yesterday's data rather than about a
place, and that reasoning fails on a station change too -- a different
station has a different yesterday.

**Neither reset is covered by a test.** The feed's scheduling state has
no observable handle, and `test_feed` deliberately touches no network,
so asserting this would mean either exposing internals for the test or
firing real requests. Recorded as unverified rather than left to look
verified.

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

#### 3.12.1.1 Measure the label, do not guess its width

The tick labels were drawn centred in a box hardcoded to 48 pixels, so
any stamp wider than the box overflowed and was cut at BOTH ends:
`Wed 14:00` rendered as `Ved 14:0`.

**A third of the labels were always correct, which is why it survived.**
`Fri` fits in 48 pixels; `Wed`, `Thu` and `Sat` do not. The fault
therefore appeared to follow the day of the week rather than the
drawing code, and a screen with `Fri` in the middle of it looked fine.

It had been noticed once before and answered in the wrong place. The
comment that used to sit above this code recorded the identical glyph
loss -- `Wed 02:00` clipped to `Ned 02:00` -- and fixed it by dropping
any label within a guessed 24 pixels of a margin. **That treated the
two positions where it had been seen and left every clipped label in
the middle of the graph untouched**, because the margin was never the
cause; the box was. It also dropped labels that would have fitted.

The box is measured from the text now, and the margin test uses the
same measurement, so a label is omitted only when it would genuinely
reach past a margin. `QFontMetrics` was already included and already
used twice elsewhere in this file -- the measurement was available all
along and this one site simply assumed instead.

**The general form is worth keeping.** A defect that presents on some
inputs and not others invites a fix at the position where it was
noticed, and that fix will pass the test that found it. Ask what
separates the inputs that fail from the ones that pass -- here, the
pixel width of three glyphs -- before deciding where the fault is.

**A label's limit is not always the plot edge.** With a gutter the zone
name sits outside the plot and the edge is the true boundary; edge to
edge on a phone there is no gutter, so the zone name is drawn INSIDE
the plot on the same row as the tick labels. A tick that cleared
`plot.left()` therefore landed on top of it, printing `CEST` and
`11:00` over each other as `CEST |1:00`.

That survived the measurement fix above and was photographed on the
device afterwards, which is the useful part: **the two faults looked
identical and had nothing to do with each other.** One was a box too
narrow for its text, the other two labels entitled to the same space.
A fix that makes a symptom rarer is not evidence that its cause was
the one found.

The limit is measured from the zone string rather than from the
80-pixel box it is drawn in, so a short name like `CEST` costs one tick
label instead of three.

### 3.14 The temperature axis holds still

The scale is computed from what is visible, which is exact and, while
scrolling, unreadable. Dragging slides new extremes in and out, so the
axis moves continuously under the curve -- and **the eye cannot tell a
temperature rising from an axis falling.**

Measured across five views of the same twelve-hour window, each shifted
twenty minutes: the top of the scale went 19.14, 19.15, 19.48, 19.81,
20.15. Every pan, a different axis.

Two things fix it, and both are needed:

- **The range is rounded OUTWARD to a quantum**, so it changes in
  visible steps rather than continuously.
- **The previous range is held** while it still contains the data and
  has not become wastefully large. Without the second half of that
  condition, a scale stretched once by a hot afternoon would stay
  stretched all week.

The same five views with the setting at 60 give 10.20 to 20.40, five
times over.

**It is a slider, not a switch**, because the useful answer is not yes or
no: a wide graph wants a firm scale to read a trend against, a narrow one
wants the detail back, and where between those is a matter of what
somebody is looking for. Zero keeps the exact-following behaviour every
version before this had, because that is a legitimate thing to want
rather than a bug being preserved.

### 3.15 The day is a boundary, not another tick

The axis answered "what hour is this" and never "what day". Past a span
of about a day the three-hourly shading stops carrying it: **that
shading is a ruler for the eye, and a ruler has no boundaries in it.**
Placing a point in a day meant counting bands back to a label on the
bottom edge, which is arithmetic the graph exists to avoid.

A heavier line at local midnight, running through both panels and the
ribbon so that a day reads as one column all the way down, and the day
it opens named beside it.

**The differentiator is form, not strength**, and the first version got
that wrong. It was a 2px line at the strongest contrast on the plot,
argued from "a boundary must outrank the ruler it interrupts". True,
and the wrong axis to argue on: in a picture whose lines are otherwise
measurements, the most emphatic line reads as a measurement.

Measured on the dark scheme, luminance across one row:

    day divider    74      <- brightest thing on the plot
    temperature    71
    grill bands    64
    grid           54
    background     35

The divider outshone the data it was drawn behind. Reported from the
phone as needing "less contrast, to signify they are part of the
background", which is exactly what the numbers say.

So it is broad and solid where the grid is thin and dotted, and it sits
with the furniture rather than above the data -- 57 against the grid's
54, below the bands at 64 and the curve at 71. **A band is legible at a
contrast far below what a line needs**, which is what makes the trade
available at all: width buys back the visibility that lowering the
contrast costs.

What actually answers "which day is this" is the NAME, bold on its
plate. The band only has to say where the day starts.

**The midnights are stepped through `QDateTime::addDays`, not by adding
86400 seconds.** A day is 23 or 25 hours on the two changeover nights,
so a fixed stride walks the divider an hour off the boundary on the
first of them and then keeps it there for the rest of the year -- a
fault that would appear twice a year, in a build nobody changed, and
look like a rendering bug rather than an arithmetic one. It is the same
reason the axis itself is in the location's clock (sec 3.12.1): the
clock the reader is standing in has irregularities that arithmetic on
epoch seconds does not model.

The name is drawn after the data, on the translucent plate the edge
labels use, because a divider nothing names says only that something
changed here. It is bold, since it has to be findable at a glance among
the hour labels along the bottom, which are quiet on purpose.

The colour is per scheme rather than shared, for the reason the rest of
the furniture is (sec 10.3): the measured data colours stay put across
light and dark, and only the ground and the furniture move.

### 3.16 The rain scale is fixed, so light rain looks light

The rain trace was scaled to the largest rate in view, floored at 1.0
mm/h, and drawn to 45% of the temperature panel at full height. **That
made the height meaningless as a quantity.** Drizzle at 1 mm/h reached
the same place as a downpour would, because each was the worst thing on
its own screen -- and the only clue was a small `1.0 mm/h` label at the
edge, which nobody reads before forming an impression. Reported from
the phone as "what is the green thing and why is it high up", which is
the question a scale like that is guaranteed to produce.

Full scale is now a constant 10 mm/h. The number is the meteorological
classification rather than a taste: light rain is under 2.5 mm/h,
moderate runs to about 10, and heavy is past it. So the bottom quarter
of the trace is light rain, the middle is moderate, and a trace filling
the band is genuinely heavy -- and it means the same thing on every
screen, at every zoom, on every day.

**Rates above full scale clamp rather than rescale**, and the edge
label gains a `+`. Rescaling is what the old behaviour did and it is
exactly the fault being removed; dropping the excess silently would
break sec 3.5, which takes the MAXIMUM rain in a column precisely
because losing a downpour is the one thing this graph cannot afford.
A clipped trace says so instead.

Measured on a render with synthetic observations at three rates, since
the live forecast had no rain to draw:

    24 mm/h    clamped at full scale, label reads "10+ mm/h"
    2.5 mm/h   about a quarter of the band
    0.4 mm/h   a few percent -- visible, and plainly slight

The old behaviour would have drawn all three at the same height on
three different screenfuls.

### 3.17 The readout is a row, not a panel

The readout was six stacked lines in an opaque box pinned inside the
plot: time, temperature, rain, chance, wind, and the source. It blocked
the graph it was describing. On a desktop that is a nuisance; on a
phone the box is a large share of a small picture and there is no
second window to move it to. Reported as "the box that shows data
blocks the view pretty seriously".

**Height was the fault, not width**, so the fields join into a single
row along the top of the plot. Six lines becomes one.

Three things went with it, each because something else already says it:

- **The provider.** `nowcast / wunderground` was the longest line in
  the old box and therefore set its width, to say what the provenance
  ribbon underneath says in colour. The band name stays; the provider
  goes.
- **The second decimal on rain.** It never decided anything.
- **The weekday, on a narrow screen.** The graph names every day across
  the top now (sec 3.15), so a readout repeating it is the cheapest
  thing in the row to lose.

**The narrow spelling is not a fallback for an unusual case.** The
Fold's cover screen is 320 logical pixels and the roomy row does not
fit it at all, so treating narrow as the exception would have stripped
the row back to the time and the temperature on the device people
actually carry. Below 420 pixels the weekday goes and the separators
drop from three spaces to one -- five separators at three spaces is a
dozen characters of nothing, which on that screen is a whole field.

Measured, and only then dropped. The row is fitted against the plot
width, and if it still does not fit, fields are removed from the END,
which is why they are built in order of what a reader wants: the time
and the temperature survive to the last. Clipping instead would be the
fault of sec 3.12.1.1 all over again. As it turns out nothing has to be
dropped at either size:

    320 px    07:15 17.0 C 0.0 mm/h 1% 9 km/h nowcast
    1080 px   Fri 07:00   17.0 C   0.0 mm/h   1%   9 km/h   nowcast

### 3.18 A sharpening band refines a quantity; it does not own a column

The temperature line disappeared, occasionally, in stretches. Reported
from the phone twice before it was caught in a screenshot, and the
screenshot is what solved it: the readout at the cursor said

    04:45  0.0 mm/h  radar

-- a time, a rain rate, a band, and **no temperature**.

MET Norway's nowcast is a five-minute radar extrapolation, and asking
it directly settles what it carries:

    step  0  02:40Z: air_temperature, precipitation_rate, wind_speed, ...
    step  1  02:45Z: precipitation_rate
    step 22  04:30Z: precipitation_rate
    steps carrying air_temperature: 1 of 23

**One step in twenty-three has a temperature.** The radar band is the
finest thing on the graph, so it won every column inside its two-hour
window, and `reduce()` gave the winner the whole column. With no
temperature in twenty-two of its samples there were no temperature
knots, so no curve was fitted and the line was absent -- exactly where
the data to draw it had been fetched and was sitting in memory.

**The code contradicted this document**, which already said what the
band was for: radar and extended "sharpen bands that already have a
source rather than supplying one that would otherwise be blank". It was
not sharpening. It was taking over.

So ownership skips it. `bbq_composite::owner_at()` returns the finest
band that describes the weather, `at()` keeps its old meaning, and the
rain is then sharpened from radar afterwards -- which is the half that
makes the band worth fetching at all, five-minute precipitation being a
better answer than an hourly mean.

**This is not the blending sec 3.7 forbids.** That rule is about one
quantity averaged across sources. Here each quantity still has exactly
one source, and the column still reports one band's account of itself;
radar refines the single field it is expert in. Both the drawn value
and the knot are replaced together, or the trace and the readout would
disagree about the same column.

The skip is named rather than inferred from whether a sample happens to
carry a temperature. A data-driven test would hand radar the column for
the five minutes of its first step and take it back for the next two
hours -- ownership flickering with the clock, which is worse than
either answer consistently.

A column that only radar covers still falls back to it, since a missing
shower is not an improvement on a missing line.

#### 3.18.1 The same fault in the scorer, where it cost more

The graph was not the only consumer taking the finest band's word for
everything. `bbq_grill_score()` called `at()` directly, so for the next
two hours it scored a radar sample -- and its own rule says an absent
temperature is neutral rather than cold, which is right when nothing
knows the temperature and wrong when the hourly band knows it and was
merely outranked.

**A freezing dry evening therefore scored as though it were warm**, in
exactly the window somebody is deciding whether to light a fire. The
graph fault made the picture wrong; this one made the recommendation
wrong, which is the thing the program is for.

The composition lives in `bbq_composite::resolved_at()` now, so the
rule is stated once and every consumer asking "what is it like at this
instant" gets the same answer. That the graph and the scorer had
drifted apart is the argument for putting it there rather than fixing
the scorer in place.

Guarded by a test that was watched to fail: with the scorer reading the
winning band's raw sample again, `radar_does_not_hide_the_cold` reports
1 C scoring as though the temperature were unknown.

The tray and the verification note had drifted too, and were found by
asking the question rather than by anyone noticing:

- **The tray showed `--` and "No reading for now"** whenever the
  observed band's last measurement had just ended, because radar then
  won `now` and carries no temperature. Intermittent by construction --
  it depended on how long ago the station last reported.
- **The verification note looked up the record of the winning band**,
  so it described radar rather than the band the graph had drawn, and
  radar has no temperature record to describe.

`correction.cpp` asks the same question and is correct, which is worth
recording: it wants only to know that SOMETHING covers the instant and
reads no field off the winner. A consumer is at risk here when it takes
a VALUE from the reading, not when it takes coverage.

Both halves are tested now, and both tests were watched to fail --
`radar_still_sharpens_the_rain` covers the half that is easy to lose
while fixing the first, since keeping radar from owning a column is
only correct if its rain still arrives.


### 12.15 The seeding refusal is tested by running the program

`--seed-verification` writes invented statistics, and it must never
write them into the real archive. The guard is four lines in `main()`:
no `--history-path`, no seeding.

Nothing checked it, and it is precisely the kind that stops working
without anyone noticing. **It produces no output when it is doing its
job, and what it prevents is silent too** -- fabricated bias rows
sitting in the store looking like measurements, feeding the corrected
band onto the graph. The APK signature check in this project stopped
matching when a tool changed its output format and reported nothing
wrong for months (sec 11.4); this is the same shape in a place where
the damage is to data rather than to a build.

It cannot be tested by linking, because it lives in `main()` and a test
binary cannot have a second one. So `test_seed` runs the built program.

**Both directions, and the second is not garnish.** One case asserts
the refusal without `--history-path`; the other asserts that seeding a
scratch file DOES work. Without the second, the first would pass just
as loudly if the binary were missing, broken, or refusing everything --
which is exactly the failure the suite hit while being written, and
which the refusal test alone reported as success.

The check is on the filesystem as well as the exit code. Every standard
location is redirected into a temporary directory, and the test asserts
that no `.sqlite` appears anywhere beneath it. A program that refused
politely and wrote the rows anyway would pass an exit-code assertion.

Two things this cost, both worth knowing:

- **`make test` now builds the application.** The suite exercises the
  program, so it depends on it. Tests are still not built by the
  default target; the dependency runs the other way.
- **The path must be absolute.** `ARTIFACT` is a bare name for an
  in-place build, `QFile::exists` resolved it against the working
  directory and `QProcess::start` searched `PATH`, so the child never
  launched -- and the run took two milliseconds while reporting a
  failure about missing output rather than about a missing program. A
  test that cannot start its subject should say so in those words.

### 12.13 The archive has no today in it

`record: none yet` on the verdict line, on two phones, for days. The
store said why:

    forecast_pending   1634 rows
    observation           6 rows
    verification          1 row
    reliability           0 rows

Six observations across three days, and their times give it away --
two per day, always the first eleven minutes of the LOCAL day:

    08-11 22:04Z, 22:09Z
    08-12 22:04Z, 22:09Z
    08-13 22:04Z, 22:09Z

The observed band asks `/v2/pws/history/all?date=<today>`, and **that
endpoint is an archive: today is not in it yet.** Whatever the hour, it
answers with the first couple of rows of the day and nothing since.
Every ten-minute refresh got the same two rows, the primary key on
(station, valid_utc) deduplicated them, and the archive grew by two a
day.

Measured against the same station on the same afternoon, changing only
the date:

    date=today       2 samples, 00:04..00:15 local
    date=yesterday   288 samples, a full day at five minutes

288 is the figure the client's own comment promises of this endpoint.
Nothing was wrong with the station, the parser, or the store.

**Verification is a whole section of this document and it could never
have worked.** A forecast is scored by matching it against an
observation at the same instant; with two observations a day there was
nothing to match. The queue was not the problem -- 1634 forecasts were
waiting patiently for measurements that were never going to arrive.

Yesterday is fetched separately now, on a six-hour interval since it
cannot change, and from the startup path as well as the heartbeat: the
heartbeat refuses to run while anything is outstanding, a launch has
everything outstanding, and a phone that is opened and backgrounded may
never reach an idle beat at all. The same product and handler serve
both days, because the reply is archived and the series is then rebuilt
from the STORE rather than from the reply -- so two days accumulate
instead of replacing one another.

One launch now archives 290 observations where it archived 2.

**The lesson is about silence.** Nothing failed. Every band reported
success, `missing` said `none`, the fetch log looked healthy, and the
one line that could have said otherwise -- `record: none yet` -- reads
exactly like a feature waiting for enough data. A pipeline starved at
its source looks identical to one that is merely young.

### 12.14 A sensor that never moves is not a measurement

With observations finally arriving (sec 12.13), the first real
verification came out as this, against the station this project was
written for:

    hourly  temperature  bucket 4  n=12  bias -6.67  MAE 6.67

Bias equal to MAE means every error had the same sign, which is a
signature rather than a result. The archive said why: 292 observations,
temperature 22.0 in every one of them. Asking Weather Underground for
the raw day confirmed it is not a parsing fault -- the API returns

    tempAvg = tempHigh = tempLow = 22
    dewptAvg = 22
    humidityAvg = 99
    windspeedAvg = 0..7        <- moving normally
    qcStatus = 0

for all 288 rows. Temperature equal to dew point at 99% humidity, held
for a day while wind and pressure vary, is a soaked or enclosed probe.
`metric.tempAvg` is the correct field; the data behind it is not a
measurement.

**The consequence is not confined to a table.** The corrected band
(sec 12.5) is drawn from these numbers, so a stuck probe becomes a
curve on the graph carrying the authority of a measurement. -6.67 C is
not a forecast error; it is the distance between the weather and a
broken sensor, and it was about to be subtracted from the forecast.

So a quantity whose observations never change across six hours and
twenty-four samples is not scored, and the refusal says so out loud:

    temperature at ISTOCK822 never changed across 23 hours of 288
    observations; not scoring it

**Per quantity, not per station.** The same station's wind moves and is
worth scoring -- its +8.2 km/h bias is a real systematic error, a
sheltered garden reading lower than a regional forecast, and exactly
what the correction exists to remove. Refusing the whole station would
throw that away with the bad field.

The thresholds are deliberately conservative. An hour of unchanging
temperature is ordinary weather, particularly at the whole-degree
quantisation this source reports; six hours of it, across a sunrise or
a sunset, is a fault. The test is exact equality rather than a
tolerance, because what it catches is a repeated number, and a
tolerance would begin refusing calm days.

**This does not fix the station.** It stops the program stating a
confident number about a forecast on the strength of a probe that is
not reporting the weather. Choosing a better station is the other half,
and it is not the program's to choose.

### 12.12 The forecast's record, beside the verdict it produced

The verification tables (sec 12.3) were collected to answer one
question -- how much should this forecast be trusted -- and answered it
only to `--history`. The verdict line carries it now:

    Best window: Thu 13:00 to 22:00 (9.0 h, score 0.76)
    record: hourly @12h  bias +1.2 C, MAE 1.7, rain skill 0.34 (n=50)

**The band and the lead are taken from the window itself**, not chosen.
A record for some other band at some other lead would be a true number
about the wrong thing.

Two details carry meaning rather than decoration. The bias keeps its
SIGN, because a band that runs warm and one that runs cold are different
problems and "1.2" says neither. And rain is reported as skill rather
than as a raw Brier score, because 0.1 is excellent in a dry climate and
poor in a changeable one -- only the comparison against always
predicting the base rate says which this is.

Where nothing has been checked yet it says so, rather than leaving the
space blank. An absence reads as "nothing to report"; the truth is that
nothing has been scored yet, which is a different thing and the normal
state of a fresh install.

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
- `make hooks` installs the commit-msg hook from `tool/hooks/`.
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

### 5.1.8 The hole scan could not report a hole at the end

`--fetch-once` walks the composite looking for uncovered stretches. Two
faults sat in its loop and cancelled each other out on ordinary data.

It scanned up to AND INCLUDING the end of coverage. A sample covers a
half-open span, so `at(end)` is never covered -- meaning every single
run opened a hole on its final step. And a hole still open when the scan
finished was dropped: not counted, not printed, not in the total.

Together those were invisible. The spurious final hole was silently
discarded by the same bug that would have discarded a real one. **A
genuinely uncovered tail would have been reported as `holes 0`** -- a
check answering the opposite of the truth, which is worse than one that
does not exist.

The scan stops before the exclusive end now, and an open hole is closed
against it. This is the same shape as the signature check in sec 11.2.1
and the reliability table in sec 12.11: a check that cannot report a
fault reads exactly like one finding nothing wrong.

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
for real on its first run: the binaries built into `build-test/`
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
- The Android build runs on the shared vocabulary in `tool/android.mk`,
  spread from `~/.claude/tool/` like `style_gate.py` (sec 11)
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
  `tool/android.mk`.** Their target names differ from the agreed ones
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

### 10.3 Light, dark, or the device's answer

**This reverses sec 3.8.3**, which fixed the graph's palette to Weather
Underground's measured colours on a white plot and refused to follow the
desktop into dark mode. That reasoning is still recorded and was right
for what it was deciding: a white plot with pale hour bands IS the look
sec 0 asked for, and following a desktop theme would have traded a
measured aesthetic for a system preference.

What changed is where the applet runs. **A white rectangle at night on a
phone is not a style choice, it is a torch.**

The data colours are identical in both schemes. They are measurements of
somebody else's chart (sec 3.8.2), and a measurement does not change
because the room got darker -- the temperature red is WU's red either
way. What changes is the ground it is drawn on and the furniture around
it: background, grid, band shading, axis text. Two entries are lifted in
dark rather than the palette being reworked, because WU's darker greens
and blues were chosen against white and go muddy on black.

Three things were decided while building it:

- **`auto` UNSETS the override rather than pinning the current answer.**
  Setting today's scheme would freeze it, so a phone switching to dark
  at sunset would stop being followed and the setting called "auto"
  would be the one that no longer was.
- **The palette is set explicitly, not left to the style.** Asking Qt to
  change colour scheme moved the graph and left the controls light -- a
  dark plot in a light window, which is worse than either and was the
  first thing a rendering showed. Whether a style honours the hint is
  the style's business; what the applet looks like is not.

  **The hint is advisory, and measurably so.** A test asserting on it
  found that `colorScheme()` reports Unknown immediately after
  `setColorScheme(Dark)` on the offscreen platform -- the request is
  simply ignored. So the theme is carried entirely by the palette, and
  the test asserts on that: light and dark must differ, and dark must be
  the darker of the two. Asserting on the hint would have passed where a
  platform honours it and failed where one does not, while saying
  nothing about what anybody sees on either.
- **The whole window follows, not only the graph.** The controls are
  what the eye lands on first on a phone.

### 10.4 On mobile there is no frame at all, and the numbers sit on the plot

A gutter is a frame by another name. The left margin and the measured
right one together take about a tenth of a phone screen to hold four
short labels, and that tenth is plot -- the difference between reading
an evening and squinting at it.

So on mobile the graph has **no horizontal margins anywhere**: not in
the window layout, and not inside the graph either. The axis numbers --
the temperature high and low, `100%`, the rain scale, the wind scale and
the zone -- are drawn ON TOP of the plot, each on a translucent plate in
the plot's own background colour. They do not need room; they need
contrast, which is a different problem with a much cheaper answer.

Both shapes draw their edge labels through one helper, so the desktop's
gutter and mobile's overlay cannot drift apart. The vertical margins
stay: the verdict above and the controls below need separating from the
plot, and the safe area (sec 11.4) is still added on every side, because
system furniture is not decoration and cannot be traded for width.

**One bug here is worth keeping.** The edge-to-edge rule was written
inside the `QT_VERSION >= 6.9` guard that the safe-area query needs. The
desktop build is 6.8, so the whole block compiled away and the mobile
shape silently kept its margins on the one platform where they could be
looked at. The shape must not depend on the Qt version; only the safe
area does.

### 10.5 Three defects a narrow screen found, and the flag that found them

The Fold's cover screen clipped the controls: the station field ran past
the right edge and the gutter labels were cut mid-character, `rain %`
reading as `ra`. Neither showed on the other phone, which is wider.

`--size` renders at a given size, so the defect became reproducible on
the desktop in one command instead of a build-install-look cycle per
attempt. **A defect found on one device stays device-specific until it
can be provoked somewhere it can be iterated on**, and guessing a width
to test at is how that happens.

Two separate faults came out of it.

**The mobile grid paired controls by POSITION.** It walked the control
list two at a time, which works exactly as long as every control has a
label -- and the checkboxes do not. From the first checkbox onward every
label sat beside somebody else's control: "Layout:" next to "Wind", its
combo on the following row beside "Theme:". A label that names the wrong
thing is worse than no label, and this was live on both phones rather
than being a narrow-screen problem at all. It is paired by meaning now:
a label takes the control that follows it, and unlabelled controls share
a row with each other.

**Nothing allowed the fields to shrink.** The station box's 150-pixel
minimum was chosen for a desktop row and had become a lower bound on the
whole window, so a narrow screen pushed the controls wider than the
display and clipped whatever was on the right. The value column takes
the slack now and the floor is low enough for a phone.

#### 10.5.1 The same fault again, in the widget nobody looked at

The cover screen clipped again, months later and in the same way: every
control on the right lost its border and the rain label `10 mm/h` was
cut to `10`. The cause was `bbq_forecast_graph`'s own
`setMinimumSize(360, 180)` -- **the identical mechanism as the station
box above**, in a different widget, left behind when that one was
fixed.

The arithmetic is worth stating because it is not visible from the
screenshot. The cover display is 840x2289 PHYSICAL pixels at 420dpi,
which is 320 LOGICAL pixels wide. A 360-wide floor overhangs that by
40, about a ninth of the screen. Nothing in the layout was wrong; there
was no room for it to be right in.

It also defeated the first attempt to reproduce it. Rendering at
`--size 840x2289` fits comfortably, because on the desktop those are
logical pixels and it is 2.6 times the room the phone has. **The size
to reproduce at is the logical one**, and the way to notice the
mistake is that the render came back 360 wide when 320 was asked for --
the floor announcing itself.

The lesson is the one in sec 3.12.1.1, arriving from a different
direction: the earlier fix addressed the widget where the symptom was
seen rather than the class of fault, so the same defect waited in the
next widget with a minimum in it. When a fix is "this floor was too
high", the question to ask before closing it is which other floors
there are.

### 10.6 A slider crashes the program on Android

Tapping anything in the window killed the process:

    JNI DETECTED ERROR IN APPLICATION: JNI CallVoidMethodV called with
    pending exception java.lang.NoSuchMethodError: no non-static method
    "...AccessibilityNodeInfo$RangeInfo;.<init>(IFFF)V"
    Fatal signal 6 (SIGABRT)

**It is Qt's bug, and it needs three things at once**, which is why
months of device checks never saw it. Qt's accessibility bridge builds
a `RangeInfo` for any widget that exposes a value, using a constructor
that exists only from **API 33**; the device is Android 10, **API 29**.
Qt guards the result with `if (rangeInfo.isValid())` but never clears
the JNI exception the failed construction leaves pending, so the next
JNI call aborts. It happens only while an accessibility service is
running -- here `com.jamworks.bxactions`, a Bixby-button remapper --
and only for a widget with a value range. Ours is the "Steady scale"
slider of sec 3.14.

Qt version-gates other calls in the same function
(`androidSdkVersion() >= 36`), just not this one.

Untouched, the program runs perfectly, which is exactly why every
earlier device check passed: nothing had ever tapped the screen.

**On Android the control is a drop-down instead.** A combo box carries
no value range, so the node is never built and the fault cannot occur.
The setting is the same number either way -- Off, Slight, Steady, Firm
against 0, 25, 60, 100 -- and the desktop keeps the slider, because the
argument in sec 3.14 for a continuous control still holds where it
does not crash. A stored value from a desktop is matched to the NEAREST
choice rather than reset, since a phone that silently zeroed it would
be worse than one that rounds.

**A drop-down was the first answer and it was too narrow.** Removing
the slider removed one widget with a range; the station list's popup
has a SCROLLBAR, which has a range too, and selecting from it aborted
the program exactly as the slider had. Qt creates scrollbars itself
inside every scrollable view, so no arrangement of the interface
avoids them -- which is what makes per-widget avoidance the wrong
shape of fix.

**What is narrow enough is withholding the VALUE.** Qt sets
`info.hasValue` if and only if the accessible interface offers a value
interface, so an accessibility factory that hands sliders, scrollbars,
dials and progress bars a plain `QAccessibleWidget` -- which has none
-- means the node is never built. Names, roles, focus and text still
reach Android; only the number is withheld, and only where it cannot
be delivered. Everything else is declined, so Qt's own factory answers
for it and accessibility is otherwise untouched.

The mechanism is tested: `a_slider_reports_no_value_to_accessibility`
asserts that a slider and a scrollbar come back without a value
interface and that a label is declined, and it was watched to fail.
**The Android half is not tested**, and cannot be from here: the phone
that reproduced the fault no longer runs an accessibility service, both
emulator images on this machine are API 33 -- above the version where
the bug exists -- and a test that cannot fail is not evidence. What
would close it is any accessibility service running for two minutes
while the station list is opened.

This is a workaround for somebody else's defect and should be removed
when Qt carries the version guard. It is written as a platform
conditional rather than a layout one for that reason: the fault follows
the operating system, not the shape of the window.

## 11. Android

The build is wired and harmonized. **It does not complete on this
machine**, for a reason that is a missing SDK package rather than
anything in the tree -- see sec 11.2.

### 11.1 The vocabulary is shared, the build rule is not

`tool/android.mk` carries the target names, the preflight, the
versionCode, the adb plumbing and the signature check. It is spread
verbatim from `~/.claude/tool/android.mk`, the same model as
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

`include` is where make first sees a target, and `tool/android.mk` was
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

### 11.2 It builds, and what it took

**`make android` produces an installable arm64 APK.** 52 MB, debug-signed,
against the Qt 6.10 kit. Four things were in the way and none of them
was the thing recorded here before.

**Build it with NDK r27c** -- `ndk;27.2.12479018` -- which is the NDK the
kit names and the one sec 11.3 explains the cost of not using. Measured
since: it builds, packages and signs, and the applet loads on the
device.

**The platform floor was real and is gone.** Qt 6.10 pulls AndroidX
libraries needing a `compileSdk` of at least 34, and this SDK reached
android-33. android-36 is installed now, which clears it.

**`ANDROID_SDK_ROOT` was set and never exported.** androiddeployqt reads
the SDK location out of the deployment-settings JSON, and qmake writes
that from the ENVIRONMENT rather than from any make variable, so it fell
back to a path baked into the Qt installation: `/opt/android/sdk`. Every
source compiled and the shared object linked before it failed on a
directory nobody in this project had ever named.

**androiddeployqt picked the wrong platform.** Its default is "the
highest available" and it chose `android-33-ext5` over `android-36`, so
Gradle refused the build for the same compileSdk reason as before -- with
the platform now installed. `--android-platform` is named explicitly, and
the generated `apk` rule hardcodes that flag away, so the target reuses
Qt's own `apk_install_target` and replaces only the deploy step.

**Gradle chose a JRE for its toolchain.** It took java-21-openjdk, which
ships no compiler, and reported "does not provide the required
capabilities: [JAVA_COMPILER]" while a perfectly good JDK 17 sat on PATH.
**The preflight had passed, honestly and uselessly**: it checked that
javac existed and Gradle then used a different JVM entirely -- a check
verifying something other than the thing it protects. `JAVA_HOME` is
resolved from javac and exported now, so the JVM the preflight approves
is the JVM Gradle runs.

### 11.2.1 The signature check had been switched off by a tool update

The APK built, and the fragment announced `signed by ` with nothing
after it.

`apksigner` prints `V2 Signer: certificate DN:` in build-tools 37; the
check read only `Signer #1 certificate DN:`. So it matched nothing,
printed an empty name, and -- this is the part that matters -- **the
guard that fails a release built with the debug key could never fire**,
because it tests a string that was always empty.

That guard exists because beerssh shipped a debug-signed release under a
success message. A tool's output format changed and quietly disabled it,
which is the same failure one layer up: **a check that cannot report a
fault reads exactly like one finding nothing wrong.** Both formats are
accepted now, and an unreadable signature is an error rather than a
blank, because "who signed this is unknown" must not look like a pass.

The artifact verifies as `C=US, O=Android, CN=Android Debug`, which is
correct for a debug build and is what the release guard would refuse.

### 11.3 What the phone showed that no rendering could

It installs, launches and draws: the mobile layout, the stacked
controls, the dark theme, and an honest "no station pinned and no
geocode set" on a device with no config yet. The station field works and
persists -- `station=ISTOCK822` was read back out of the app's own INI --
and SQLite works, with `history.sqlite` and its WAL files sitting beside
it.

Three things only the device could show.

**It died on the first launch, after every gate had passed.** The Qt kit
names `android-ndk-r27c`; the build used NDK 25.2, and Qt 6.10's
libraries want `std::pmr::monotonic_buffer_resource` from a newer
libc++:

    dlopen failed: cannot locate symbol
    "_ZTVNSt6__ndk13pmr25monotonic_buffer_resourceE"

Sec 11.2 had recorded that NDK mismatch and dismissed it -- "it compiled,
so that is a caution rather than a finding". **It compiled, linked,
packaged, signed and installed, and then would not load.** NDK 30 runs
too, but r27c is what the kit names, and matching it removes the guess
rather than surviving it. r27c is what the build uses now.

**There is no TLS.** Every fetch fails before it starts:

    current: no API key: TLS initialization failed

Qt for Android does not bundle OpenSSL, and `Qt/Tools/OpenSSL` here
holds only the source. So the applet on the phone can reach nothing at
all, and the station field is innocent -- it saved exactly what it was
given. This is the next piece of work and it is not small: OpenSSL has
to be cross-compiled for the ABI and bundled through
`ANDROID_EXTRA_LIBS`. **beerssh already has a script that builds
precisely this** (`tool/build-deps-android.sh`, which its own header
says has never been run), so this is a candidate for shared tooling
rather than a second copy -- and per `harmonization.md` that is an
observation to raise, not an extraction to make in passing.

**The system bars overlap the content**, because targetSdk 36 puts the
app edge-to-edge on Android 15 and up: the verdict line under the clock,
the status line under the navigation buttons.

The fix for the last of these is in, and its first version was wrong in
a way worth keeping. `QWindow::safeAreaMargins()` reports
`QMargins(0, 0, 0, 0)` on this device -- measured, not assumed -- and the
first attempt ASSIGNED it to the layout, which wiped the ordinary
padding and pressed everything flat against all four edges. Worse than
the overlap it was meant to cure. **The safe area is added to the
layout's own margins now, never substituted for them**, so an unknown
safe area costs nothing and a real one is respected. Whether Android
ever reports a non-zero one through Qt 6.10 is unsettled and needs a
device to answer.

### 11.4 The kit moved, and the preflight now checks the thing that bit

Qt on this machine went from 6.10.0 to 6.12.0 between one session and the
next, and every Android instruction here named 6.10.0 -- including the
one the shared fragment prints when it cannot find a kit, which was
therefore advice pointing at a directory that no longer existed.

Measured rather than assumed: **6.12.0 builds, packages and signs with
the same r27c**, which its own `qdevice.pri` names exactly as 6.10.0 did.
The examples name 6.12.0 now, and the fragment's error message names no
version at all -- an example carrying a version number goes stale every
release, and a stale example is worse than a placeholder because it
looks checked.

**The preflight compares the NDK in use against the one the kit names.**
It reads `DEFAULT_ANDROID_NDK_ROOT` from the kit's `qdevice.pri` and
`Pkg.Revision` from the NDK's `source.properties`, and refuses a major
version mismatch.

This is not the compileSdk case sec 11.2 declines to guess at. The kit
states its NDK outright, so the check reads a fact rather than
predicting one -- and it is the fault that cost the most here, because it
is invisible until the device refuses to load the app. Watched failing
against a fabricated NDK 25 against the 6.12 kit, and `make android-check
ANDROID_NDK_MISMATCH_OK=1` still gets through for anyone who has a
reason.

### 11.5 TLS, and where the source comes from

Qt for Android ships no OpenSSL, so `QSslSocket` cannot start and every
provider here is HTTPS. That is not a degraded build; it is one that can
fetch nothing, and Qt says so plainly once you look:

    W qt.tlsbackend.ossl: Failed to load libssl/libcrypto.

`tool/build-openssl-android.sh` cross-compiles it, and the interesting
decisions are about the source rather than the compiler.

**The system's OpenSSL cannot be used, and not for one reason but two.**
It is x86-64 where the phone is aarch64, and it is linked against glibc
where Android has bionic -- so even a Debian arm64 build would not load.
Only source is portable across that gap.

**The source comes from apt.** `apt-get source openssl` gives a version
the distribution pinned, with a signature and a checksum apt verifies
before we see it, and it arrives with the distribution's security patches
already applied -- the fetch for 3.5.6 applied fixes for CVE-2026-42766
and a use-after-free in `PKCS7_verify` on the way past. A tarball
fetched by a line in a shell script would move that trust decision into
this repository, where nobody reviews it.

**The old Qt's copy was refused.** `Qt-old/Tools/OpenSSL/src` was still
on the machine and would have saved the download: it is 1.1.1q, dated
July 2022, end-of-life since September 2023. Convenient is not the same
as safe, and this is the one place in the project where the difference
is measured in somebody's security rather than in a wrong graph. The
script refuses anything that is not 3.x, by version rather than by
policy note.

### 11.5.1 Two mistakes, both from believing a requirement that was not there

**The version check failed open.** It read `OPENSSL_VERSION_TEXT` out of
`opensslv.h`, which OpenSSL 3 does not ship -- it GENERATES that header
during configure, from `VERSION.dat`. So against a 3.5.6 tree the check
found nothing and refused to proceed, which was the right outcome by
luck; had the case been ordered differently it would have sailed past.
**A security check that cannot read a version must fail closed**, and it
reads both layouts now.

**The libraries were renamed, and that broke them.** Believing
androiddeployqt required `lib<name>_<abi>.so`, the build renamed them and
used `patchelf` to fix the SONAME. The device refused the result:

    dlopen failed: cannot find "9_REQ_fp" from verneed[0]
    in DT_NEEDED list for libcrypto_arm64-v8a.so

which is not a missing symbol. Rewriting `.dynstr` shifted the offsets
the version-needs section points into, so it named a string that was
never a filename. **androiddeployqt has no such requirement** -- it
copies extra libraries verbatim -- and Qt's TLS backend finds them by
pattern, `libcrypto.*` and `libssl.*`, not by exact name. OpenSSL's
Android targets already emit unversioned libraries, because Android will
not follow a `.so.3` and an APK may only carry files named `lib*.so`.
There was nothing to correct. **The libraries ship exactly as linked.**

The script checks the thing that would otherwise fail only on the
device: that every library `libssl` needs is either in the package or
part of Android.

### 11.5.2 What is proven and what is not

Proven: the libraries build, they are aarch64, they are in the APK
beside Qt's own OpenSSL backend plugin, the app installs and runs, and
**`Failed to load libssl/libcrypto` no longer appears at all**.

Not yet proven: that a fetch completes. The phone was locked for the
run, so Android had the activity stopped and Qt never got past creating
its window -- no config was written and no store was created, which is
correct behaviour for an app that has not been resumed. It needs one
unlocked run to confirm.

### 11.5.3 A removed library still shipped

Two libraries were dropped from `ANDROID_EXTRA_LIBS` and deleted from
`deps/`, and the next package contained them anyway.

androiddeployqt COPIES extra libraries into its staging directory and
leaves whatever is already there. Nothing prunes it, so the artifact
described a configuration that no longer existed anywhere in the tree --
and the build reported success while doing it.

**Removing a file from a project has to remove it from the artifact**,
or every stale build is a package nobody can account for. The staging
directory is cleared before each deploy. Wholesale removal is right in
exactly this shape: the directory is one the build created, it is named
relative to a `BUILD_DIR` the project owns, the variable is checked
non-empty first, and the next step refills it.

This is the same class as every other stale-state trap here (sec
11.6.2), and it is the one that would have shipped: the others wasted
time, this one put files in a package.

### 11.6 TLS on Android: what it was, after four wrong answers

The applet on the phone could fetch nothing. Qt reported
`qt.tlsbackend.ossl: Failed to load libssl/libcrypto` and every band
failed with "TLS initialization failed".

**It was not the libraries.** Qt's automatic plugin discovery never
loads the OpenSSL TLS backend on Android. The plugin sits in the same
directory as the cert-only backend that DOES load, loads perfectly when
asked directly, and Qt emits no diagnostic about skipping it -- not even
with `qt.tlsbackend.ossl.debug` turned on. `bbq_ensure_tls_backend()`
asks: it is a no-op wherever TLS already works, so it needs no platform
test at the call site.

**FIXED, at the fourth attempt, by renaming the libraries.** Qt asks the
Android linker for `libssl_3.so` and `libcrypto_3.so`; the project was
shipping `libssl.so` and `libcrypto.so`. The account below is kept in
full, including two repairs that did not work and one confirmation that
was false, because the wrong turns are the useful part.

The measured result, on an SM-N960F:

    supportsSsl        yes
    active backend     openssl
    found at runtime   OpenSSL 3.5.6 7 Apr 2026
    OK wunderground, met.no, open-meteo; 1 of 6 failed

The single failure is `api.weather.com` answering HTTP 401 to the
probe's deliberate `apiKey=0` -- a completed TLS handshake carrying a
real reply, which is the opposite of the fault.

**Where Qt looks, and why both of its routes missed.**
`qsslsocket_openssl_symbols.cpp` builds the library name as
`"ssl" + ANDROID_OPENSSL_SUFFIX`, defaulting to `"_" QT_OPENSSL_VERSION`
-- `_3` for OpenSSL 3.x -- and dlopens it by name, letting the Android
linker find it in the application's own library directory. Unsuffixed
files are simply not what it asked for.

Qt then has a fallback that globs for `libssl.*`, which WOULD have
matched the old names. It scans `libraryPathList()`: `LD_LIBRARY_PATH`,
then `/lib`, `/usr/lib`, `/usr/local/lib`, `/lib64` and `/system/lib`.
**An Android app process has `LD_LIBRARY_PATH` unset** -- measured from
`/proc/<pid>/environ` -- and its libraries live in
`/data/app/<package>-<hash>/lib/<abi>`, which is on the LINKER's search
path and on no list Qt scans. So the first route asked for a name that
did not exist and the second looked in directories that did not hold it.

**Why the failure was mute, and why `cert-only` was misleading.**
`QTlsBackend::availableBackendNames()` filters on `backend->isValid()`,
and for the OpenSSL backend `isValid()` IS `ensureLibraryLoaded()`. The
plugin was found, loaded and constructed every time; it was discarded
one step later, by a filter that reports nothing. Meanwhile `cert-only`
is compiled into QtNetwork and needs no plugin at all, so its presence
in the list was never evidence that plugin loading worked -- and reading
it as evidence is what made four wrong answers look reasonable.

An earlier version of this section said the opposite, so the correction
below stands as written.

Two repairs were tried and neither worked:

- **Widening the search** to `applicationDirPath()` and
  `QLibraryInfo::PluginsPath` as well as `libraryPaths()`. The search
  was never the problem: the plugin was always found.
- **`instance()` instead of `load()`.** This one is a real improvement
  and is kept -- `load()` only maps the shared object, while a
  `QTlsBackend` registers when it is CONSTRUCTED -- but it is not the
  fault either. The plugin now constructs successfully and the backend
  list does not change.

Measured on an SM-N960F, with the plugin instantiated by hand:

    supportsSsl        NO
    available backends cert-only
    libplugins_tls_qopensslbackend_arm64-v8a.so  load ok, instance ok

**A plugin that constructs and still does not appear in
`availableBackends()` is the actual open question**, and it is a
different question from the one this section spent four rounds on. Note
that `cert-only` proves nothing about discovery: it is compiled into
QtNetwork and needs no plugin, so its presence in the list was never
evidence that plugin loading worked. That misreading is what made every
wrong answer look plausible.

Qt emits no diagnostic about any of it, with
`qt.tlsbackend.ossl.debug=true` set and its output captured to file.

**How the false claim happened, because it is the more useful lesson.**
The evidence offered was that `history.sqlite-wal` stood at 832272
bytes after the install. The size was real. The file's mtime was 72
minutes BEFORE that install, so the data was from an earlier run and
the number said nothing about the change it was cited for. **A file's
size answers "is there data", never "did this change produce it"** --
only the mtime does, and only against a timestamp taken beforehand. The
`git log` now carries the claim in commit 5866ad0, which is why it is
corrected here in prose rather than quietly dropped.

This is the vacuous pass of `evidence.md` in its most expensive form:
not a check that inspected nothing, but a real measurement of the wrong
thing, cited with confidence.

Four things were blamed first, and each cost a build-install-test cycle:
the library names, the NDK, whether the libraries were extracted from
the APK, and the packaging. Two of those turned out to be real problems
worth fixing anyway -- the NDK mismatch (sec 11.3) and extraction (sec
11.6.2) -- but neither was this.

### 11.6.1 The probe, and why it should have been first

`--probe` fetches a list of URLs and reports what TLS resolved to. Each
target separates a specific pair of explanations rather than being
thorough: plain HTTP tells "no network" from "no TLS", a boring HTTPS
host tells "TLS broken" from "that provider broken", and
`supportsSsl()` with the two library version strings answers directly
what four rounds of inference could not.

On the device it said, in one run:

    supportsSsl        NO
    available backends cert-only
    lib/arm64          libcrypto.so libssl.so
    OK   http, FAIL every https

Libraries present, network fine, backend absent -- in one run, where
inference had taken four.

**It was not the whole diagnosis, though this said it was.** Three more
rounds went by before the reason the backend was absent came out, and
that line of the report is exactly where the remaining trouble was
hiding: `libcrypto.so libssl.so` are the names the probe FOUND, and
they are not the names Qt was asking for. The probe was reporting the
right directory and the right files and confirming, run after run, a
premise nobody had thought to doubt. A measurement can be accurate,
repeatable, and still answer a question next to the one that matters.

**The lesson is the order of operations.** Four inferences were drawn
from a one-line warning and two of them were wrong; the probe produced
more fact in a single run than the entire sequence before it. When a
remote failure reports one line, the next step is an instrument, not a
theory.

It writes a file as well as logging, because on Android the early part
of startup happens before Qt installs its logcat handler -- so the first
version of the probe reported nothing at all, on the one platform it was
written for.

### 11.6.2 Five false readings, all from measurement rather than code

Worth listing, because each looked exactly like a real failure:

- **`strings` does not find `QStringLiteral` text.** It is UTF-16, so a
  check for freshly added code in a built library reported it missing.
- **`run-as ... sh -c "rm -f ..."` quoting failed silently**, so a stale
  report was read as a fresh one.
- **The APK path hash revealed a stale install**: the report being read
  came from a previous package.
- **`androiddeployqt --no-build` succeeded against leftover state** and
  appeared to generate a Gradle project it had not.
- **`adb exec-out screencap` piped through a shell mangled the PNG**,
  producing a blank white image of a screen that was drawing correctly.
  Capturing to a file on the device and pulling it works.

The libraries themselves needed one real fix: `legacyPackaging=true`, so
Android extracts them to `lib/<abi>/` where Qt's directory scan can see
them. Inside the APK they are invisible to it.

### 11.7 The device archive cannot be written from the desk

`run-as` reads the application's own directory and **cannot write to
it**. Measured on the SM-F926B running Android 15: reading
`files/history.sqlite` works, and every attempt to create a file in the
same directory answers

    /system/bin/sh: can't create probe-write.txt: Read-only file system

So there is no route from a workstation into the on-device store.
Anything that has to change it -- a purge, an import, a repair -- must
go through the application, which writes its own directory perfectly
well. `adb` can look and cannot touch.

**This was learned by destroying the archive.** The temperature
verification rows were poisoned by a stuck sensor (sec 12.14) and the
purge was done off-device: copy out, fold the write-ahead log in, drop
the bad rows, write back. The copy and the purge were correct and
verified -- `integrity_check` ok, every other table intact. The write
back failed, and by then the write-ahead log had already been removed
to keep it from contradicting the file that was about to replace it.
The WAL held 1.9 MB against a 40 KB main file, so the archive went from
292 observations, 1396 queued forecasts and 32 verification rows to
nothing.

**The ordering is the whole lesson.** Write the replacement first and
remove the old state only once the replacement is confirmed in place;
better still, prove the destination is writable with a throwaway file
before touching anything that matters. The check was run afterwards,
which is the wrong end of the operation and turned a recoverable
mistake into a destructive one.

What made it survivable is worth recording too. Observations are
**re-fetchable** -- the backfill of sec 12.13 pulls a full day back from
Weather Underground's archive on the next launch -- and the pending
queue refills from the next forecast fetch. Only the accumulated
verification counts were genuinely lost, about a day of them. A store
whose contents can be rebuilt from their sources is a store that
tolerates this kind of mistake; one holding the only copy of anything
would not have.

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

### 12.9 What building it changed

Three things the design got wrong, all found in the first hour of the
band existing and none of them predictable from the paper version.

**The ribbon is the wrong place for it, and sec 12.5 said to put it
there.** The ribbon answers "which band produced this stretch of the
drawn data". A corrected curve is not produced by a band; it is this
program's adjustment of one. Putting it there would make one strip
answer two different questions. It is drawn as a dashed overlay in a
colour deliberately outside Weather Underground's measured set -- it is
not their data, and a curve wearing their palette would say it was --
and it is labelled where it starts, because an unexplained second line
on a weather graph is worse than no second line.

**A bucketed bias drew a staircase, twice, for two different reasons.**
The first was the bucketing itself: bias is stored per lead bucket, so
subtracting it directly steps at every bucket edge and gives the line
structure that came from the schema rather than from the weather. It is
interpolated between bucket centres now, held flat beyond the buckets
that have evidence.

That fixed less than it looked like it should, because the second cause
was different and larger: **a forecast sample holds one temperature
across its whole span** (sec 3.1), so sampling the composite every
quarter hour repeats an hourly value four times. The red line is smooth
because the graph joins sample STARTS. The correction has to be built on
the same starts or it disagrees with the curve it is drawn against, in a
way that looks like data. One corrected point per underlying forecast
sample.

**The two lines have to get identical treatment.** The forecast is
rounded by the smoothing setting and the overlay was not, so the gap
between them was part bias and part smoothing -- and the entire purpose
of drawing them together is that the gap IS the correction. Anything
applied to one is applied to the other.

A fourth was found by the test rather than by looking, which is the one
worth keeping in mind: the interpolation clamped at its upper end and
not its lower, so any lead shorter than the first bucket's centre ran
the line backwards off the end of the evidence and produced a bias of
the wrong sign. **A correction that made the forecast worse, at the one
lead time where it is most nearly right.** At those leads the error is a
fraction of a degree and the rendered curve looked entirely reasonable.

### 12.10 Rain is corrected too, and independently

The first version corrected temperature alone, which was short of what
was asked for: a deviation factor for the real values, not for one of
them. Rain rate is corrected now as well, and **the two are independent**
-- a provider can be reliably warm and perfectly good about rain, so one
number covering both would describe neither. Either may be drawn without
the other, and a quantity with no evidence behind it is absent rather
than corrected by zero, which would draw a line claiming the raw
forecast had been checked and found right.

**Wind is corrected too, and drawn, because a correction nothing draws
is a correction nobody can check.** It matters here through the grilling
score (sec 7) rather than through the graph, so it is offered rather
than imposed: a checkbox, off by default, because the plot already
carries three quantities and a fourth line by default would cost every
reader something to gain what only some of them want.

It gets **its own axis**, labelled in the right gutter, rather than
being hung off the temperature or rain scale. That is the same objection
sec 3 records against putting a percentage on an existing axis: a scale
meaning two things means neither. It is drawn thin and dotted and in a
muted colour, under everything else, because it is context rather than a
headline. Corrected wind is floored at zero for the reason rain is.

Three things this turned up:

- **A corrected rain rate is floored at zero.** A band over-forecasting
  by more than it forecast would otherwise produce negative rainfall and
  the graph would draw rain below its baseline. The same clamp sec
  3.11.2 puts on the drawn curve, for the same reason.
- **A rain overlay with no rain in it is not drawn.** On a dry forecast
  the raw rate is zero, a positive bias corrects it below zero, and the
  clamp puts it back at zero -- an honest dashed line lying flat along
  the baseline for the width of the graph, saying nothing. A line that
  says nothing still has to be read before it can be dismissed.
- **The overlay counts towards the scales.** It did not, and a
  correction large enough to leave the axis range ran off the top of the
  plot and was clipped at the widget edge -- a curve that simply stops,
  which reads as a rendering fault rather than as a value out of range.

And one defect, found by the test rather than by looking: an early guard
still required a temperature, left over from when temperature was the
only quantity corrected. **A sample carrying rain and no temperature was
dropped entirely**, so rain could never be corrected on its own. It was
invisible in every rendering, because the bands that carry rain here
carry temperature as well.

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

### 12.7 Three numbers the implementation forced

None of these was in the design and all three had to be decided to make
it work. Recorded because each is a threshold, and a threshold nobody
wrote down gets treated as a law later.

- **The bands do not share a clock.** An hourly forecast lands on the
  hour; the station reports whenever it feels like it. Demanding an
  exact match between a forecast's valid time and an observation would
  verify almost nothing, so they pair within 150 seconds -- about half
  the station's cadence, which is the widest that cannot reach the wrong
  sample.
- **Rain is taken to have occurred above 0.1 mm/h.** The Brier score
  needs a yes-or-no outcome and the world supplies a rate, so something
  has to draw the line. This is that line and nothing else depends on it.
- **A forecast is given up on 36 hours after its valid time.** Long
  enough that a station down overnight still gets verified when it comes
  back, short enough that the queue does not carry an outage for ever.

The database runs in WAL mode with `synchronous = NORMAL`. Two copies of
the applet open at once is a real case on this machine and the default
journal makes one block the other; the risk accepted in exchange is that
a crash can lose the last transaction, which is five minutes of weather
that gets re-fetched anyway.

### 12.8 The observed band is served from the store, not from the fetch

The obvious wiring keeps the fetched band in memory and reaches for the
database only when somebody pans past it. That makes history a special
case, and special cases are where the disagreements live -- two paths to
the same band, and a seam at the edge of the fetched window where they
meet.

**So the fetch's only job is to keep the store current, and the composite
always reads its observed band back out.** Panning into last March is the
same operation as looking at this afternoon, and there is no seam because
there is no second path.

Three consequences worth stating:

- **The `current` band is NOT archived**, and that is correctness rather
  than an oversight. A current reading carries the declared validity of
  sec 3.9, and storing it with that span would put a band of priority
  300 across minutes nobody measured, overruling the forecasts sec 3.3
  ranks above it exactly so its extension stays harmless. Nothing is
  lost: the station's own history reports the same reading on the next
  observed fetch, with an honest duration.
- **A store read is stamped with when the band was FETCHED**, not when it
  was read back. Reading from disk is not freshness, and a store read
  that stamped itself as new would make sec 2.4's staleness check report
  a dead feed as healthy every time the view moved.
- **Reloading is skipped when the view is already inside what is
  loaded**, and a margin of one span either side is taken when it is not.
  The view emits on every mouse move of a drag, so this has to be cheap
  when the answer is already in memory.

Verification runs when a round settles rather than on a timer of its own:
a round is precisely when new observations have arrived, so it is the
only moment anything new can be checkable.

`--history` reports what is actually in there -- row counts, the earliest
observation, how many forecasts are waiting, and the error table by band
and lead time. Measured on the first live run: two observations, because
that is all the station published today, and 775 forecasts queued.
**Nothing verified yet, which is right**: every queued forecast is still
in the future.

### 12.11 Reading the chance score, and archiving the diagnostic

**The reliability table was write-only.** It accumulated on every check
and nothing ever read it, which is a store growing a column nobody can
see -- and the argument for keeping it was that a percentage forecast
needs a different instrument. An instrument nobody reads is not one.

`--history` reports it now, and reports it against its reference. **A
raw Brier score means nothing on its own**: 0.1 is excellent in a dry
climate and poor in a changeable one. So the baseline printed beside it
is the score a forecaster earns by ignoring the weather entirely and
always predicting the observed base rate, and the skill is how much
better than that this band managed -- zero being no better than knowing
nothing. Always saying fifty percent where it rains half the time scores
0.25 and a skill of exactly zero, which is the case the test pins.

Under each score is the reliability curve, which asks the only question
a percentage can be held to: **of all the times this band said forty
percent, how often did it rain?** A calibrated forecaster's bins sit on
the diagonal.

**`--fetch-once` archives what it fetches.** It reaches the real
providers through the real feed and produces genuine observations, and
discarding them because the caller happened to be a diagnostic would put
a hole in a record whose entire value is that it has none. It honours
`--history-path` like everything else, so a check that should not touch
the archive still need not.

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

### 13.1.1 The gestures are asserted, not just looked at

Every other visual question in this project was settled by rendering a
picture and looking at it, and for the gestures that method is not
enough. **A rendering shows that a view was honoured. It cannot show
that the arithmetic behind the gesture is right**, because a graph
zoomed about its centre and a graph zoomed about the cursor produce
pictures that both look entirely reasonable.

So `test/test_view.cpp` asserts the two invariants that make the
gestures feel like anything:

- **Zooming holds the moment under the cursor.** Four steps in and four
  back out, checked to within a pixel's worth of time each way.
- **Dragging keeps the moment that was grabbed under the pointer that
  grabbed it**, and stops when the button is released, so a later hover
  is not a pan.

Both were watched failing. Zooming about the centre instead of the
cursor breaks the first; flipping the sign of the drag breaks the
second. Neither break is visible in a screenshot, which is the whole
argument for the test existing.

It is the only widget test in the suite, so it is the only one that
needs `gui` and `widgets`, and it chooses the offscreen platform in its
own `main` rather than trusting whatever ran it. The handlers are
protected and are reached through a subclass rather than by faking
events through a window system that is not there -- a synthetic click
would be testing Qt.

`plot_rect()` is public for this: the assertions have to be made against
the same rectangle the handlers use, not against a second guess at how
the margins are computed.

### 13.1.2 Pinch, because a phone has no wheel

Zooming was built on the wheel, which is a desktop instrument. A phone
has two fingers and no wheel at all, so without a pinch gesture the zoom
of sec 13 simply does not exist there.

It holds the same invariant the wheel does: **the moment under the
fingers stays under them.** Zooming about the middle of the widget
instead would slide the thing being pinched away from the fingers doing
the pinching -- which feels broken in a way no screenshot can show, and
is the reason sec 13.1.1 asserts the invariant rather than looking at it.

Fingers apart means see more detail, so the span shrinks: the reciprocal
of the scale factor, not the factor.

**Unverified on hardware.** It is written and it compiles, and only a
touchscreen can say whether it feels right.

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

## 14. Stations are discovered, remembered, and pinned

The station was a text field somebody had to know the answer to. That
is a poor way to start, and a worse way to recover: the station this
project ran against for months reports a dead thermometer (sec 12.14),
and finding a replacement meant querying an API by hand.

So stations are DISCOVERED -- from a coordinate, or by searching for a
place -- and then KEPT. The list a reader picks from grows rather than
being re-derived, so a station found on holiday is still offered at
home.

**Three states, and they are not the same idea.**

- **Known.** Heard of, remembered, offered in the list. Costs nothing:
  no requests are made for it.
- **Pinned.** Fetched, and SPARINGLY -- the backfill only, on its
  six-hour interval. Four requests a day each, and it is exactly what
  verification needs, since what a forecast must be scored against is
  history rather than the present moment.
- **Watched.** The one being looked at, fetched at the ordinary
  cadence: observations every ten minutes, current every five,
  forecasts, backfill. Exactly one at a time, and it is a preference
  rather than a fact about the world, so it stays in the INI while the
  station list lives in the archive (sec 12.2).

**The request volume is the reason for the distinction.** The API key
is scraped from Weather Underground's own site and shared with it, so
fetching everything remembered would multiply today's traffic by the
length of a list the user did not think of as a cost. Pinning makes the
cost visible and chosen, and the interface marks the pinned ones so it
is obvious which are spending requests.

**Remembering preserves the pinned flag**, which is why it is an upsert
rather than a replace. Discovery runs again whenever the coordinate
moves, and a rediscovery that reset the flag would unpin a chosen
station silently, at the moment somebody walked somewhere -- surfacing
days later as a station with no history. `first_seen_utc` is preserved
for the same reason: it records when this program first heard of a
station, not when it last saw it.

### 14.1 Two things the discovery endpoints lie about

Both were found by measuring rather than by reading, and both would
have been invisible in code review.

**`units` breaks `/v3/location/near`, and the error blames `format`.**
Every other endpoint in this project takes
`units=m&language=en-US&format=json`, so discovery inherited it and
answered 400:

    {"code":"LOCATION-SERVICES:400",
     "message":"'format' must be specified"}

with `format=json` plainly in the query. Bisected one parameter at a
time: `format` alone is 200, `format`+`language` is 200,
`format`+`units` is 400. **A message that names a parameter which is
present is worse than no message**, because it sends the reader to
check the thing that is right. Discovery sends its own query without
units.

**`updateTimeUtc` is not a heartbeat.** It looks exactly like one, and
a staleness rule was built on it -- drop any station that has not
reported for a day, so that a dead sensor never reaches the list. It
dropped the entire list. Measured: every station the endpoint returns
carries a timestamp about six weeks old, `ISTOCK877` among them, which
was confirmed reporting minutes earlier and holds 504 archived
observations. It is cached registration metadata. Nothing filters on
it now, and the field carries a comment saying why it is ignored rather
than being quietly deleted -- the next reader will have the same idea.

There is no liveness signal in this response. Whether a station is
alive is answered by asking it: `observations/current` returns 204 for
a station that is not reporting and 200 for one that is, which is how
`ISTOCKHO936` was ruled out while looking healthy in every other
respect.

### 14.2 The station is chosen from a list, and pinning is visible

The control was a text box somebody had to know the answer to. It is a
drop-down of what has been discovered now, nearest first, with the
pinned ones at the top and MARKED -- a bullet, which survives a narrow
screen, and bold, which survives a glance.

**Marking is not decoration.** Pinning is what spends requests, and a
cost the reader cannot see is one they did not choose. The same
argument puts the `Pin` checkbox beside the list rather than in a menu:
the state has to be reachable from the thing it describes.

**Still editable.** A station id somebody already knows should not
require finding it on a map first, and discovery only ever finds what
is near a coordinate it has been given. A watched station the list has
never heard of is added to it, which is the ordinary case on a fresh
install where the id came from a setting or the command line.

`Find...` asks for a place, and the answer is a CHOICE rather than the
first match. Searching for Gothenburg returns Gothenburg and then
Shlisselburg, in Russia, so a program that silently took the first
would send somebody four hundred miles east for a typo they did not
make. The chosen coordinate is pinned, because the reader named it: an
unpinned one is replaced by the next coordinate derived from the
station, and the search would appear to do nothing.

The list is rebuilt wholesale on every change rather than patched. It
is short, it changes only when discovery runs or a pin is toggled, and
a partial update is how a control ends up disagreeing with the thing it
describes -- which this project has already paid for twice.

Checked at both widths that matter: 1080 logical pixels, and the 320 of
the Fold's cover screen, where `* ISTOCK877  3.7 km` and the `Pin` and
`Find...` controls all keep their borders.

#### 14.2.1 A label is not an id

The list shows `ISTOCK877  4.0 km`, because the distance is what makes
one of ten choosable. The editable field hands that whole string back,
so committing it stored the label as the station and the next fetch
asked Weather Underground for a station called `ISTOCK767  0.3 km`.
Measured on the device -- the settings file came back holding exactly
that.

Selecting from the list was always correct; it reads `itemData`. It is
the typing path that was wrong, and typing is the path that exists so
that somebody who already knows an id need not find it on a map.

Resolved against the list rather than parsed. Splitting on the spaces
would work until a station id contains one, and this is a project that
has already been bitten by a format assumption holding until it did
not (sec 11.4).

### 14.3 The device's position is for discovery, and nothing else

A fix answers "which stations are near ME". It does NOT move the
forecast: somebody watching a station in Stockholm while standing in
Gothenburg must still be shown Stockholm's weather, and letting a
sensor change the forecast coordinate would be the
two-places-on-one-axis failure of sec 2.6.7 arriving through the
hardware. So a position goes to `discover_stations_at()` and never to
`set_geocode()`.

**Coarse, and once.** The nearest station is hundreds of metres away at
best, so precision buys nothing and `ACCESS_COARSE_LOCATION` is all the
manifest asks for -- a permission that asks for more than it needs is
one a reader is right to refuse. Once rather than continuously, because
watching the position would spend battery re-answering a question whose
answer barely changes.

**Every failure is the same failure from outside.** No positioning
source compiled in, none on the machine, permission refused, location
switched off, or no fix before the deadline: all end as `unavailable`
with a reason. The fallback is identical in each case -- search for a
place by name -- and it is the ONLY route on a desktop, so it is a path
that gets exercised daily rather than one that waits to be found
broken.

A deadline of our own is needed because a source that never answers is
the ordinary indoor case and emits no error while it waits.

**The reason goes on the control that offers the alternative**, not on
the freshness line. That was the first attempt and it was wrong twice
over: the line is rewritten on every fetch, so the message survives
until the next request and then vanishes unread, and it describes the
DATA rather than the device. The `Find...` tooltip carries the reason,
and an empty list gets the placeholder `no location -- use Find...`,
which is what a fresh install on a desktop shows.

#### 14.3.1 The package asks for more than the manifest does

The manifest requests `ACCESS_COARSE_LOCATION` and nothing else. The
built package requests both:

    uses-permission: android.permission.ACCESS_FINE_LOCATION
    uses-permission: android.permission.ACCESS_COARSE_LOCATION
    uses-implied-feature: android.hardware.location

`ACCESS_FINE_LOCATION` comes from
`Qt6Positioning_arm64-v8a-android-dependencies.xml`, which
androiddeployqt reads and injects. **It is visible only in the
artifact**: nothing in this repository asks for precise location, and
the section above would have been an honest description of the source
and a false one of the program somebody installs.

Removing it the documented Android way -- a `tools:node="remove"`
directive for the manifest merger -- is refused before Gradle sees it.
androiddeployqt parses the manifest itself and rejects the namespaced
attribute, so the merger never gets the chance.

Left as it is, and recorded rather than hidden. The alternative is
editing the deployed manifest between androiddeployqt and Gradle, which
would put a text substitution in the build for a permission that
changes nothing about what the program does: positioning is asked for
with `NonSatellitePositioningMethods` either way, and Android's own
dialog lets the reader grant approximate location whatever the manifest
requests.

**And a trap in the manifest itself.** XML comments cannot contain a
double hyphen, and this project's prose uses `--` constantly. Three
rebuilds were spent on

    Error in AndroidManifest.xml: Expected '>', but got ' '

which names neither the comment nor the character. The first version of
the comment happened to have none and built; every later edit added
one. A file that is prose-heavy and XML at the same time needs the
rule stated where the prose is written, which is why it is here.

**Nothing was borrowed from fuzzypickles.** It was worth looking, since
it is the sibling project with location code, but its GPS is `gpsd` on
Linux feeding the daemon's entropy pool; its Android spike lived in an
earlier generation its own notes record as gone; and its `client/geo.h`
is haversine distance and bearing, which this program does not need
because Weather Underground returns `distanceKm` with each station.

#### 14.3.2 A manifest permission is not a granted one

The first Android build asked for a position and never got one, and the
device said why:

    ACCESS_COARSE_LOCATION: granted=false
    ACCESS_FINE_LOCATION:   granted=false
    station: 0 rows

**Nothing had asked.** Declaring a permission in the manifest only makes
it requestable; since Android 6 it must also be granted at run time, and
nothing grants it but a dialog somebody answers. The locator created the
source, requested an update, and the platform refused it without ever
raising the question -- which from the desk is indistinguishable from a
phone that cannot see the sky, because both end in the same
`unavailable`.

`QLocationPermission` at `Approximate` accuracy is asked for before the
source is touched. Approximate rather than Precise so that three places
agree: the manifest, this request, and the
`NonSatellitePositioningMethods` the source is configured with.

Measured on an SM-N960F after the fix:

    ACCESS_COARSE_LOCATION: granted=true
    ACCESS_FINE_LOCATION:   granted=false

    qt.positioning.android: Positioning start
    qt.positioning.android: Single update using network
    qt.positioning.android: Stopping updates

Coarse granted and fine still refused, which is the point of asking for
the one rather than accepting both. `Single update using network` is Qt
honouring the non-satellite request. `Stopping updates` is twenty
seconds later: the deadline of sec 14.3 firing on the ordinary indoor
case, after which the list showed `no location -- use Find...` exactly
as it does on a desktop with no source at all.

**Only a device could have found this**, and only a device with somebody
holding it: the fallback is correct and silent, so a build that never
asks looks identical to a build that asked and was refused. What
separated them was reading the permission state out of `dumpsys
package` rather than watching the program's own behaviour.

### 14.4 Pinned stations are fetched sparingly, and one at a time

A pinned station gets the backfill and nothing else: yesterday's day of
observations, on a six-hour interval, four requests a day. That is the
whole cost of pinning, and it is deliberately the cheapest fetch that
is still useful -- a forecast is scored against HISTORY, so history is
exactly what a station needs to become verifiable while something else
is being watched.

**One at a time, and that is not a performance choice.** These answers
arrive on the same signal as the watched station's and carry nothing
saying whose they are. What makes an answer attributable is that
exactly one is outstanding. Getting this wrong would file one station's
measurements under another's id, which is not a bug that shows up as a
crash -- it shows up months later as a verification record that cannot
be explained.

Three things keep that true, and each was written for a failure this
project has already had:

- **A distinct product.** `observed_pinned` rather than `observed`, so
  the handler can tell them apart at all rather than inferring it from
  state.
- **The store only.** A pinned station's series never reaches the
  composite. It is not this location's weather, and drawing it would
  put another town's measurements on the graph.
- **A failure releases the slot.** Without that, one failed request
  occupies the queue for the life of the process and every other pinned
  station waits behind something that already ended -- the stalled
  socket of sec 2.4, in a smaller room. It is also not reported as a
  band failure: nothing on the display depends on it, and announcing it
  would put another station's trouble on the watched station's status
  line.

The watched station is skipped when the queue is built. It is fetched
properly and far more often, so queueing it would spend a request to
learn what it already knows.

Measured with `ISTOCK877` watched and `ISOLNA31` pinned, after one run:

    ISOLNA31     288 rows   temp 14.0..26.0
    ISTOCK877    504 rows   temp 12.0..26.0

One day for the pinned station, filed under its own id, while the
watched one kept its ordinary two.

### 14.5 Scoring follows the queue, not the watched station

Pinning fetches a station's observations sparingly, four times a day,
and sec 14.4 says why that is worth the requests: forecasts made while
the station was watched can still be scored after the view has moved on
to somewhere else. The Pin control says so in its own tooltip -- "keep
fetching this station's history, so its forecasts can be scored even
while another is being watched."

It was not true. The scoring sweep ran at the end of a fetch round for
`m_station_id` alone:

    if (m_history.is_open() && !m_station_id.isEmpty()) {
            const int checked = m_history.verify(m_station_id);
            m_history.expire(m_station_id, now);
    }

So a pinned station's observations arrived, were archived, and were
never used. Its queue never emptied, and `expire()` never reached it
either, so the rows it could no longer score leaked for ever -- the
exact failure sec 12.6 introduced expire() to prevent, reappearing for
every station except one.

**Nothing could report it.** The fetches succeeded, the rows arrived,
the statistics simply stayed where they were, and a station nobody is
looking at is one nobody is reading numbers off. It is this project's
recurring shape once more: not a check that fails, but work that never
runs.

The sweep asks the queue instead:

    for (const QString &station : m_history.stations_with_pending()) {
            checked += m_history.verify(station);
            m_history.expire(station, now);
    }

Driven by the queue rather than by an inventory of who is interesting,
which reaches three cases with one rule -- the watched station, a pinned
one, and one that is neither, whose queue somebody moved away from. That
third case had no route to being scored or expired under any
station-list rule, because it is on no list. It stops appearing here of
its own accord when its last row is scored or dropped, so nothing has to
remember to take it off.

The clock is read once for the whole sweep. Per station it would let a
sweep crossing a second expire two stations against two different
cutoffs -- a difference nobody could ever reproduce.

`verify_all()` is public for a reason worth stating: the defect lived at
the end of a fetch round, which is the one place a test cannot reach
without the network, and it survived precisely there. The test builds
three stations with identical queues, watches one, pins another,
abandons the third, and requires all twelve rows scored. Against the old
rule it returns 4.

### 14.6 A signal nobody connected, and the gate that finds the next one

Fixing sec 14.5 left a question worth asking of the whole tree, and it
is the one that lens suggests: what else runs for nobody? The cheapest
form of it is to count listeners. Sixteen signals are declared here and
fifteen had one. `verified` had none.

It was emitted for a consumer that was never written, so a program whose
entire purpose is scoring forecasts scored them and told nothing. The
consequence was small and precisely placed: `refresh_status` recomputes
the record note, and it runs on `updated`, which is emitted while a
response is being handled -- whereas scoring happens when the round
settles, after it. The note was therefore always one round behind, and
the round it was behind by is the only one anybody would notice. The
first time anything is ever scored, the verdict goes on reading `record:
none yet` until the next fetch, which is exactly the moment somebody is
watching to see whether it worked.

Connected now, which is what the emit always assumed.

**The instrument is worth more than the defect.** A signal with no
listener cannot be found by testing behaviour: on the common paths a
feature that never runs and a feature whose effect is produced some
other way look identical, and every passing case increases confidence in
the wrong mechanism. It also needs no sibling to compare against -- zero
is wrong on its own terms, which makes it one of the few structural
faults a machine can settle without judgement.

So `tool/signal_listeners.py` counts them on every `make style`, and it
was watched to fail: repointing this connection at `settled` makes it
name `verified` and exit 1. It carries an allow list that is empty, and
should stay empty while every signal this project declares is consumed
inside it.

Two of this project's last three defects were work that never ran rather
than work that ran wrongly -- the scoring sweep of sec 14.5, and this.
Neither was visible in output, both were visible in structure.

### 14.7 A timeout is not a failure, and the wrong word cost an hour

`--fetch-once` reported

    fetch-once: timed out after 30s
    fetch-once: 1 band(s) failed

on a run where FOUR bands were outstanding and none of them had failed.
The count came from the timeout incrementing the same `failures` the
band handlers use, so a single timeout was reported as one failed band,
and the four that were merely still in flight were described by nothing
at all.

Read as written it points at the key scraper -- the component sec 2.2
already documents as fragile and expected to break -- so the wrong cause
was also the believable one. The investigation it started went looking
for a broken extraction pattern. The same command succeeded on the next
run in under a second, which is what a transient hang looks like from
outside.

Now the timeout says what it is, and names every band that had not
answered:

    fetch-once: no answer within 0s from: observed current radar
                nowcast extended hourly
    fetch-once: they had not failed -- they had not answered yet

**The list is deliberately NOT `missing_bands()`**, and getting that
wrong once inside this fix is the part worth recording. That function is
the display's question and a narrower one: it leaves out radar and
extended on purpose, because they enhance bands that already have a
source rather than supplying one, and a complaint about them would mean
nothing to a reader (sec 2.6.6). A diagnostic asking what did not answer
must not inherit that judgement -- the first draft did, and a run where
only the enhancements hung would have printed an empty list and read as
though nothing were outstanding. The fault being fixed, reappearing
inside the fix.

Proved by forcing the path rather than by reading it: a zero-second
budget fires the timer before any reply can arrive, and the message
named all six. With the real budget the same command answers in about
0.33 s -- the key page is 1.6 MB and fetches in 60 ms on this link, so
30 s was never tight and has been left alone. The original hang was
transient, and no timeout would have been a better answer than a correct
report of one.

### 14.8 The old station's measurements went with its coordinate

`set_station` already drops the derived geocode, and sec 2.6.7.4 gives
the reason: a coordinate belonging to the previous station does not
describe this one, and keeping it aims the forecast bands at the old
station's garden while the observed band reads the new one. Two places
on one axis.

Its measurements do not describe this one either, and nothing dropped
them. The observed band stayed in the composite until a fetch for the
new station happened to replace it -- so in the window before that
lands, and permanently where it fails, the graph draws the previous
station's thermometer under the new station's name.

**It is worse than stale for exactly the reason the geocode was.** The
band still carries the old station's fetch time, so sec 2.4's staleness
check reports it healthy, and the one instrument that exists to catch a
band that has stopped being true says nothing. A wrong number that looks
fresh outranks every honest one on the same axis.

Dropped now, alongside the coordinate, and with it the loaded window and
the fetch stamp. Replaced by an empty series rather than removed,
because present-but-empty is what sec 2.6.6 counts as missing: the
display says the observed band is absent, which is true, rather than
saying nothing at all.

**The first version of the fix set that empty band unconditionally and
broke a test that was right.** A feed whose station is set at startup
has never held an observed band, and inventing one to report as missing
is an absence that was not there. The existing assertion -- nothing
asked for a range, so nothing is loaded -- caught it immediately, which
is the case for keeping assertions that look like they state the
obvious.

Found by asking what else is scoped to the watched station while
something else assumes otherwise, which is the same question sec 14.5
answered for scoring. That lens has now produced three defects: the
scoring sweep, the unheard signal, and this.

### 14.9 A freshness record kept per product, asked per station

The feed remembers when it last asked for each product, and `due()`
consults that to decide whether to ask again. Two of those products --
`observed` and `current_station` -- are requests about a STATION, so the
record and the question it answers are keyed differently, and nothing
said so.

`refresh()` hid it. It asks for both unconditionally, without consulting
`due()`, precisely so that changing station fetches at once. But it
declines while a round is outstanding, and changing station is something
somebody does exactly while watching a slow one. Then the next heartbeat
consults `due()`, finds the old station was asked a minute ago, and
declines as well -- so the new station's measurements arrive an interval
late, ruled fresh on the strength of a question about somewhere else.

Reset now where the station changes, beside the geocode and the
backfill, and only for those two: the coordinate bands belong to the
geocode and are already handled where it is dropped.

**The comment in `forget_location_freshness` is where this was hiding,
and it has been corrected rather than left.** It said the two needed no
reset at all, because refresh() asks for them unconditionally. That is
true, and it is not enough -- and a reader arriving with exactly this
question would have been told there was nothing to look for. A rule
stated with a reason that only holds on the common path is worse than
one stated flatly, because the reason is what stops the next person
checking.

The test writes the freshness record directly, which is why the test
class is a friend -- `attempt()` would put a request on the wire, and
what is under test is the bookkeeping either side of one. Same device as
`bbq_wu_key_source`'s, which this tree already uses for the same reason.
Making `due()` public was tried first and withdrawn: it is genuinely
internal, and the seam wanted here is a test's, not an API's.

#### 14.8.1 Fixing one band left the fault in five

Dropping the old station's observations was right and was a third of the
job. The same argument applies to every band the composite holds, and
the version that fixed only `observed` left the identical fault in the
worst place available: `current` is fetched by station id and outranks
everything at the present instant (sec 3.3), so a stale one answers
"what is it doing now" with another station's thermometer. The observed
band at least describes the past.

The forecast bands go too, for a consequential reason rather than the
same one. They are fetched by COORDINATE, and this function has just
dropped the coordinate they were fetched for -- so they describe a place
the feed has stopped claiming. Where the geocode is PINNED it is not
dropped, it was never the station's, and neither are they: those bands
stay.

**That conditional is the part a test can get wrong by agreeing with
it.** A fix that simply emptied the whole composite passes every
assertion about the unpinned case, so the test carries the pinned one as
well, and it was proved by sabotage -- clearing unconditionally makes it
report that the radar band was dropped though its coordinate was pinned.
Without that half the test would have been evidence for a fix that
throws away four bands nobody asked it to.

The lesson is the one sec 14.5 already paid for, arriving through a
different door: the first fix was written for the band that had been
looked at, not for the class the defect belongs to. The question that
finds the rest is not "is this fixed" but "what else is keyed the way
this was".

#### 14.8.2 The complaint outlived what it was about

`band_failed` puts a message on the status line and it is never cleared.
That is deliberate as far as it goes -- a band that fails must not fail
invisibly, and sec 2.4 will not have the difference between a thin graph
and a wrong one living in a terminal nobody reads. What it must not do
is outlive the thing it was about.

`hourly: Connection refused` is a fact about a fetch for the previous
station, or for the coordinate derived from it. Once sec 14.8 drops
every band that describes that place, the message is the last thing left
claiming it -- and read on the status line under the new station's name,
it reports a fault in the station now being watched.

Cleared where the station changes, beside everything else that belonged
to the old one.

**It is not covered by a test, and the reason is worth stating rather
than leaving as an omission.** The state lives in the window, and there
is no test binary for the window layer at all -- reaching it means
linking essentially the whole application, which is a piece of work
rather than an assertion. Moving the field into the feed, where the
station-change tests could reach it, was considered and rejected: the
other assignment to it is a history-open failure, which is not a band
and not the feed's to report.

**The real finding is the gap, not the line.** Every defect this session
found in the window -- a label stored as a station id, a value interface
that aborts on touch, and this -- was found by hand, on a device,
because nothing else could look. The store, the feed and the graph all
have suites. Whether the window earns one is a question for the
copyright holder, since it is a new test binary and a real cost, not a
change to make while passing.

#### 14.8.3 The model was cleared and the screen was not

Dropping the bands from the feed's composite is invisible. The graph
holds a COPY -- `set_composite` takes one by value -- so the model was
right and the display went on drawing the previous station's curves.

Nothing pushed the emptied composite through. The graph learns of a
change in exactly two places, `updated` when a fetch lands and
`view_changed` when somebody drags, and after a station change neither
is guaranteed: the refresh that follows usually succeeds a second later
and hides it, and where it fails -- which is the case the drop exists
for -- neither ever comes.

So the fix of sec 14.8 was correct and inert, and the thing that made it
look like a fix is the same thing that makes this class hard: on the
common path the wrong mechanism produces the right answer. It is the
rule this project has now met from three directions -- a correct
function is not a working feature, and the question to ask of a fix is
not whether it is right but whether anything consumes it.

Pushed explicitly where the station changes, along with the corrected
overlay and the status line, which are derived from the same data.

**Found by continuing to look after the fix was committed.** Nothing
else would have: there is no test that can see the window, so the fix
and its inertness are indistinguishable from every instrument this
project has.

### 14.10 The window has a suite now, and it was sabotaged before it was believed

Every defect this layer produced was found by hand, on a phone: a
display label written to the configuration as a station id, a value
interface that aborted the process on touch, an error message that
outlived the station it described, and a fix to the feed that the graph
never saw. The store, the feed and the graph all had suites. The window,
which is where the parts are joined, had none -- and joining is exactly
what was going wrong.

`test_window` is the twelfth binary and the largest link in the suite,
deliberately: what it exists to check is the WIRING, which is precisely
what a narrower link would stub out. It reaches `watch_station` and the
controls through a `friend`, as `bbq_wu_key_source` and `bbq_wu_feed`
already do, because the public surface here is `begin()`, and `begin()`
fetches.

**Two guards, and both had to be got right before any result meant
anything.**

The window WRITES real configuration -- `watch_station` calls
`bbq_settings::set_station` -- so a run would otherwise rewrite the
station somebody is watching. `QStandardPaths::setTestModeEnabled` was
used first and removed: it protects the real file and it OVERRIDES the
environment, so pairing it with an `XDG_CONFIG_HOME` redirect is not
belt and braces -- test mode wins, and the run leaves a settings file in
`$HOME` on a machine whose owner did not ask for one. The environment is
set before `QApplication`, which is when Qt resolves and caches those
paths.

The assertion that caught it is worth copying: it asks whether the
config location is under `$HOME`, not whether it looks like a test path.
The first version asked the second question, passed, and was wrong --
`~/.qttest/config/test_window` satisfies "contains test" and is still in
somebody's home directory.

And `watch_station` refreshes, which fetches. An application-wide proxy
pointing at a closed port on loopback means a request that escapes
cannot leave the machine. This project scrapes a key it is not licensed
to have; a suite firing at a third party on every run would be wrong
whatever it was measuring.

**Then all four cases were sabotaged, one at a time, and each produced
exactly one failure -- the right one.**

    label resolution removed    -> a_label_is_not_stored_as_a_station_id
    composite push removed      -> changing_station_clears_the_old_curves
    error clear removed         -> changing_station_clears_the_old_error
    pin write removed           -> pinning_marks_the_station_in_the_store

The second is the one this binary was built for. It is the defect of sec
14.8.3, which was committed as a fix, was inert, and was found by
chance; it now names itself in under a second.

### 14.11 The record line has never been drawn, and it was wrong

Verification has been empty on every machine this has ever run on. A
forecast is scored only once the hour it predicted has been observed, so
until this week nothing had a score, and the code that reports one had
never had one to report. The line that says how well a band has done is
the reason the whole store exists, and it had never rendered.

It was wrong. The bias was written as

    note += QStringLiteral("  bias %1 C")
                    .arg(temperature.bias, 0, 'f', 1,
                         QLatin1Char(temperature.bias < 0 ? '-' : '+'));

and `arg`'s fifth parameter is the FILL character -- used to pad to a
field width, which here is zero. So the sign never appeared, and a warm
band printed `bias 1.2 C`: exactly the string the comment three lines
above says must not be produced, since "a band that runs warm and one
that runs cold are different problems and 1.2 says neither".

**A cold band read correctly the whole time**, because its minus comes
from the number rather than from the code, and that is why nothing ever
looked wrong. It is the shape sec 14.5 and sec 14.8.3 both had: the
common case produces the right answer for a reason other than the one
intended.

The sign is written explicitly now, and the test is a PAIR. A single
case cannot ask this question -- the cold one passes whatever the code
does -- and the pair was proved from both sides: without the fix the
warm case fails, and with the sign hardcoded to `+` the cold case fails.

**Seeding a score is the only way to look at this before the weather
obliges**, which is the argument for `set_verification` existing at all.
It is documented as a diagnostic that must never be aimed at the real
archive, and this is what it is for: the first real score will land on a
phone, days from now, with nobody able to check it against anything.

### 14.12 The box named one station while the program read another

`--station` overrides the configuration for a run and deliberately does
not write to it, so that trying a station out leaves the configured one
alone (sec 14.9's neighbour, and the right rule). The station list did
not know that. It selected `bbq_settings::station()` -- the configured
station -- while the feed read the override, so the control whose entire
job is to say which place this is named a place the program was not
reading, with every number beside it computed from another.

Found by looking at a rendered shot rather than by reasoning: the box
said `ISTOCK822` and the record line beside it said `hourly @4d bias
+14.0 C` for `ISTOCK877`. That is what `--shot` is for, and it is the
second time in this project that a picture has answered a question no
amount of reading the code had raised.

Both the list and `watch_station`'s no-op guard key off the feed now.
The guard mattered for the same reason in the other direction: compared
against the configuration, a run under an override would treat choosing
the CONFIGURED station as a no-op -- the one selection that actually
has to move the feed, since that is precisely where the two disagree.
On every ordinary run the two strings are identical.

**The shot also drew the corrected band for the first time.** That
overlay has never appeared on any machine, for the reason sec 14.11
gives: it needs verification rows and there have never been any. Seeding
2 C per lead bucket into a scratch store and rendering it shows the
dashed curve sitting below the raw forecast and diverging with lead,
which is what a bias that grows with distance should look like.

And a correction to a first reading of that picture, kept because the
mistake is instructive: the record line said `+14.0 C, n=220` where the
seed had reported `bias 2.00 C per lead bucket, n=50 each`, and the
first conclusion was that something had polluted the scratch store. It
had not. The seed scales the bias BY bucket -- so the four-day bucket is
2.0 x 7 -- and `n` is `qMax` of the temperature and rain counts, which
is the rain sample. Both numbers were right and the reading was wrong.
Checking the table settled it in one query, where the alarm would have
sent somebody looking for a data-corruption bug that does not exist.

### 4.4 The tray number is outlined, because the panel is not ours

The reading was drawn in near-black, which is right on the light panels
it was written against and all but invisible on a dark one. Measured by
compositing the icon onto the colours a panel actually is: on `#1c1c1c`
the digits disappear completely, and on `#2b2b2b` they are a smudge.

Qt offers no reliable way to ask what is behind a tray icon, and the
answer changes when somebody switches theme without the icon being
redrawn -- so choosing an ink to suit the background is guessing, twice.
A light halo under a dark fill needs no guess: the halo carries the
contrast on a dark panel and the fill carries it on a light one. It is
what map labels do, and for the same reason, since they are drawn over
terrain nobody controls.

Stroked first and filled over the top, so the digits keep the weight the
font gave them -- a stroke is centred on the outline, and filling
afterwards puts back the half that falls inside. Centred on the INK box
rather than the em box, because `drawText`'s AlignCenter centres the
line box, ascent and descent included, and a path placed the same way
sits visibly high.

**The allowance for the halo is half its width, not all of it, and that
was decided by looking.** The first version reserved the full width,
which shrank the font a size at 22 pixels and left the digits thin and
muddy -- worse than the problem in the case that matters most, since 22
is what most panels draw. Reserving the outward half keeps the glyph the
size it was and clips nothing visible.

**`--tray-icon` saved the 44-pixel pixmap alone**, so the size that
ships to a small panel had never been looked at, and a halo that reads
well at 44 can close up a digit's counters at 22. It writes both now.
A diagnostic that shows half of what the program produces invites a
conclusion about the other half.
