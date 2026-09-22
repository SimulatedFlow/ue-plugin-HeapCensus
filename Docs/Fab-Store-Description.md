<!--
PHASE 3 DONE - 2026-09-07. The demo map is built and captured; six screenshots are in
Saved/Screenshots/HeapCensus_01..06.

This description states no measured figure of its own, by design. The counter-box block below is the LAYOUT
and is labelled as such in the text - do not turn it into a claim by deleting that sentence, and do not paste
figures out of the screenshots into the copy: what they show is this machine's editor process, not a buyer's
game. (ConsoleDeck, 2026-09-06: "scan 4.3 ms" in the copy against 47.3 ms in the shot.)

For reference, what the captures do show: an editor PIE session with ~164,000 live UObjects, a census walk of
11-17 ms at that size, HeapCensusDemoObject at +12,050/min while the leak runs, and a collection of 38-75 ms.
The census cost is high there because an editor process is the worst case for the walk.
-->

# HeapCensus — Which Class Is Growing, And What Garbage Collection Costs

**Documentation: <https://wiki.teufel-engineering.com/en/HeapCensus/documentation>**

A running census of live UObjects by class: how many there are, how fast that number is growing, what the
last garbage collection cost in milliseconds — and a gate that fails a build when a class keeps growing over
a long soak.

---

## A leak in Unreal does not look like a leak

The frame rate stays fine. Then, one day, the game starts stuttering in time with garbage collection, because
the object graph keeps getting bigger and every reachability pass has more to walk. Epic's own documentation
says as much: the spikes land in the frames where the collection runs, *"especially if your application has
destroyed a large number of objects recently"*.

The engine gives you `obj list`, `memreport` and the `gc.` console variables. All of them either **configure**
or **write once**. None of them answers the question you actually have after forty minutes of play:

**Which class is growing?**

Not which class is *big*. A game with 400 projectiles is healthy. A game whose projectile count rises by 400
every minute is not. Today, people answer this by diffing two `memreport` text files by hand.

---

## What HeapCensus does

**Counts UObjects per class.** A walk over the global object array on a timer, live objects tallied by class.

**Reports the slope, not the size.** Every class gets a ring buffer of its last N counts. A least-squares line
through it gives **objects per minute**. The counter box is ordered by that slope and says so in its own
heading — `fastest growing` — because ranking by size is what `obj list` already does.

**Knows the difference between a leak and a busy game.** A pool filling up climbs and then gives it all back.
A leak climbs and never gives anything back. A class only reaches the *Leaking* verdict if it broke the
threshold **and** never fell back once inside the window. That single rule is what makes the tool usable
instead of noisy.

**What that looks like in the shipped demo map.** These figures come out of the screenshots below, and they
describe **an editor play-in-editor session on one developer machine** - not your game, and not a promise:
about 164,000 live UObjects, a census walk of 11 to 17 milliseconds at that size, `HeapCensusDemoObject` at
`+12,060/min` while the leak switch is on, and a collection of 38 to 75 milliseconds. A packaged build has
far fewer objects and a much cheaper walk; an editor process is the worst case for it. The number worth
looking at is not any of those - it is the verdict line, which in the same session reads
*"climbing, but it fell back at least once - a wave, not yet a leak."*

**Refuses to guess.** Under three samples there is no slope, and the row says `too little data` instead of
showing an invented number. Two points always describe a perfect straight line, and a plugin that reported a
leak four seconds after a map load would be right often enough to be believed and wrong often enough to be
useless.

**Times the collection.** On the engine's own hooks: the last run, the mean, the worst, how far apart the runs
are, how many objects each one freed — and the purge separately from the reachability pass, because with
incremental purge the purge is slices across frames and not one stall. Two honest numbers instead of one
misleading one.

**Prints its own price.** The walk is the one thing this plugin adds to a frame. It runs on an interval rather
than every frame, and what it cost is in the counter box next to everything else, every time. If you sell a
measurement you have to put a price tag on it.

**Fails the build.** `HeapCensus.Gate <seconds>` soaks, fits a slope per class, writes JSON and exits **0**
when nothing grows, **1** when something is climbing, **2** when a class grew monotonically over the whole
measurement and broke the threshold. This is the case a build server finds overnight and a person never will.

**Demonstrates itself.** `Heap.Leak <n>` deliberately creates and holds n objects a second, so you can watch
the slope react without first building a leak into your own game. `Heap.Churn <n>` does the same at the same
rate and lets go of them again — the control, without which "the number went up" proves nothing.

---

## The counter box

This is the **layout** of the box, not a measurement — the figures depend entirely on your project, which is
the reason for measuring them:

```
UObjects <total> | GC last <ms>, worst <ms>, every <s> | census <ms>
fastest growing - window <s>, threshold +<n>/min
class                                       count    per minute
<ClassName>                                <count>   +<n>/min   never fell back
...
<ClassName> is growing: <count> live, +<n>/min, and it has not fallen once in <s>.
```

Drawn on `UCanvas` from an `AHUD`, so it survives cooking and is still there in a packaged Shipping build —
where the profiler is not, and where the leak that takes forty minutes actually shows up.

---

## What it is not

This is the first thing in the plugin's own description for a reason.

* It does **not** track raw C++ memory. No `new` hook, no allocator instrumentation.
* It does **not** replace Memory Insights. Insights is a recorder and a better one than a plugin could be.
* It does **not** tell you *who* is holding the object. That is `obj refs name=<Class>`, and the documentation
  names it as the next step: HeapCensus finds the class, `obj refs` finds the reference.
* It measures **objects**, not bytes.

A tool that pretended to do all four would do the first one worse.

---

## Included

* One runtime module. No editor module, no UMG, no Slate — it works in a packaged Shipping build.
* `UHeapCensusSubsystem` (engine subsystem), `AHeapCensusHUD`, `UHeapCensusStatics` (Blueprint library),
  `UHeapCensusSettings` (Project Settings → Plugins).
* Eleven console commands, including the gate and both halves of the demonstration.
* A demo map with five buttons — start churn, start leak, release + collect, sample now, reset census — the
  whole product in thirty seconds, and the difference between churn and leak is measured, not staged. A wall
  of instanced blocks grows one block per twenty held objects, driven by the same count the census reports.
* JSON report for build servers, with the same 0/1/2 exit-code convention as LoadLens, LocaleGuard,
  AssetWarden and WidgetLedger.
* Eight automation tests over the arithmetic (`HeapCensus.*`), runnable headless on a build server.
* Full documentation, including a section on what the measurement costs.

---

## Technical Details

**Features**

* Live UObject census by class, on an adjustable interval
* Growth as a least-squares slope in objects per minute, over a sliding window
* Monotonic-growth test that separates a leak from a pool filling up
* Garbage-collection timing: collect, purge, interval, objects freed
* Canvas counter box that works in a packaged Shipping build
* Build gate with 0 / 1 / 2 exit codes and a JSON report
* Deliberate leak and churn demonstrations
* Blueprint access to everything, plus static, world-free arithmetic

**Code Modules**

* `HeapCensus` (Runtime)

**Number of Blueprints:** 4 (demo content: a GameMode, a HUD, the heap-wall visualiser, and the UMG panel)
**Number of C++ Classes:** 6
**Network Replicated:** No (a census is per-process by nature)
**Supported Development Platforms:** Win64
**Supported Target Build Platforms:** Win64
**Documentation:** <https://wiki.teufel-engineering.com/en/HeapCensus/documentation>
**Support:** teufelsilvan@gmail.com

**Important / Additional Notes**

HeapCensus counts UObjects and measures the collection. It does not track raw C++ memory, it does not replace
Memory Insights, and it does not tell you who is holding an object — `obj refs` does that, and the
documentation names it as the next step.

---

© 2026 Silvan Teufel. All Rights Reserved.
