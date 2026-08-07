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
  `QStandardPaths::AppConfigLocation`, which is `~/.config/bbqpredictor/`
  on this platform, in INI through `QSettings`. This is the project's
  first configuration of any kind, so it sets that location for
  everything after it.

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

## 2.9 MET Norway serves the radar band

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

## 2.10 Open-Meteo serves the extended band

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

### 2.10.3 The diagnostic no longer covers everything

`--fetch-once` predates the feed and drives the WU client directly, so
it exercises neither of the other two providers. Its message says so
rather than claiming completeness, but the divergence is real: it is a
check that no longer inspects what the application does.

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

The threshold wants trying rather than asserting; 1.5 times the nominal
step is the starting guess, and this sentence should be replaced with
whatever the real data makes necessary.

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

### 4.1 Open: which desktop

`QSystemTrayIcon` on Linux now means StatusNotifierItem. KDE Plasma
serves it natively; GNOME needs a shell extension, without which the tray
half of the brief is simply absent on that desktop.

**Unresolved, and it needs an answer before the tray work starts**, because
"applet for systray" is half the stated product and the answer decides
whether a fallback presentation is required.

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
  applied by the *generated* Makefile, so `bbqpredictor.pro` is the only
  place saying otherwise takes effect.

### 5.1 Open: packaging

Not chosen. The global guidance prefers `dpkg-buildpackage` driving a
`dh`-based `debian/rules`, and notes that the seven private projects use
five different mechanisms between them -- so picking one here is a
convention change to raise, not a thing to do in passing.

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

## 6. Style

Three rules -- `snake_case`, tabs to indent and spaces to align,
lowercase filenames -- with the detail in `code-style.md` at the repo
root.

Qt's own API is called exactly as it is spelled (`setWindowTitle`,
`paintEvent`); names this project introduces stay `snake_case` with the
`bbq_` prefix where they reach the linker.

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

## 8.1 No remote, deliberately

**This repository is local only, and that is a decision rather than an
oversight.** Recorded here for the same reason the absent licence is:
most of the sibling projects have a `github.com/funklord/<name>` remote,
so a harmonizing pass would otherwise read the absence as drift and
close it. `raidtray` has no remote either.

The reason is specific to this project. The tree contains a working
scraper for Weather Underground's API key and sec 2.2 records, in
writing, that using it violates their terms of service. Keeping that on
one machine and publishing it are different acts, and only the
copyright holder gets to make the second one.

**Adding a remote is theirs to decide, exactly as the licence is.** Do
not add one because the siblings have one.

## 9. What has been decided, and what has not

Settled:

- Qt Widgets, Qt 6, qmake under a top-level Makefile (sec 1)
- The key is scraped, with its consequences as requirements (sec 2)
- Multi-provider from the start, Weather Underground first (sec 2.7)
- MET Norway serves the radar band, as a second band rather than a
  replacement, and silently where it does not reach (sec 2.9)
- Open-Meteo serves the extended band, quarter-hourly for a week
  (sec 2.10)
- Resolution is what qualifies a provider, not convenience (sec 2.8)
- Three bands on one time axis, forecast kept presentation-free (sec 3)
- The sample is a span, not a point; canonical units C, mm/h, epoch UTC
  (sec 3.1)
- Rain is stored as a rate, so one y-axis works; the ICAO path is a
  temperature-only fallback (sec 3.2)
- Precedence is declared, and measured always beats forecast (sec 3.3)
- The composite is a view over the series, never a merge, because
  provenance is required (sec 3.4)
- Never upsample; downsample rain by maximum, never mean (sec 3.5)
- Gaps are drawn as breaks, never interpolated across (sec 3.6)
- Joins disappear through normalisation, never blending; a surviving
  step is information (sec 3.7)
- A current band anchors the present, capped at 15 minutes and ranked
  below the forecasts so the extension can only fill a hole (sec 3.9)
- QPainter over QtCharts, confirmed by building it (sec 3.8)
- Four interpolation methods, chosen live, with the samples markable;
  monotone the default because it cannot overshoot (sec 3.11)
- The readout snaps to real samples; times are in the location's clock,
  and the graph names which clock (sec 3.12)
- The palette, measured from WU's station dashboard (sec 3.8.2)
- The internal time series is ours; every provider translates into it,
  including WU (sec 2.7, sec 3)
- Grilling windows scored and shaded, with the preferences gathered in
  one place and marked as preferences (sec 7)
- No licence (sec 8)
- No remote; local only, and not for a harmonizing pass to close
  (sec 8.1)

- The WU endpoints for all three bands, observed 2026-08-07 (sec 2.6)
- The key comes from an embedded request URL, not the config blob
  (sec 2.6.1)
- The station is user-chosen and pinned, never auto-selected (sec 2.6.5)
- Discovery is an explicit act; a dead station is reported, not
  substituted; the observed band is optional (sec 2.6.6)
- Config is QSettings INI under `QStandardPaths::AppConfigLocation`
  (sec 2.6.6)
- The geocode derives from the station and is cached beside it; an
  explicit override wins and is shown when set (sec 2.6.7)

Open, each needing a decision rather than a drift:

- `--fetch-once` exercises only the Weather Underground bands; the
  other two providers arrive through the feed, which that diagnostic
  predates (sec 2.10.3)
- The observed band's lag against the current reading, which leaves a
  short hole in the recent past that the next refresh fills (sec 3.9.4).
  Understood and accepted rather than open, but worth revisiting if the
  lag proves larger than the 4-to-22 minutes measured
- The gap threshold in sec 3.6, which is a guess until real data makes
  it necessary
- Whether a dark-desktop variant is wanted, given the measured palette
  is fixed and light (sec 3.8.3)
- Which desktop, and therefore whether the tray needs a fallback (sec 4.1)
- Packaging mechanism (sec 5.1)
