# HeapCensus

**Which class is growing, and what garbage collection costs.**

A running census of live UObjects by class: how many there are, how fast that number is growing, what the
last collection cost in milliseconds — and a gate that fails a build when a class keeps growing over a long
soak.

Unreal Engine 5.8 · Win64 · one runtime module · no editor module · works in a packaged Shipping build.

---

## What it is not

HeapCensus does not track raw C++ memory — there is no `new`, no allocator hook — and it does not replace
Memory Insights. It counts **UObjects** and it measures the **collection**. It also does not tell you *who*
is holding an object; the engine's `obj refs name=<ClassName>` does that, and the documentation names it as
the next step.

That boundary is the first line of the store description too, on purpose. A tool that is honest about its
edges is a tool people keep.

## Why

A leak in Unreal does not look like a leak. The frame rate stays fine. Then, one day, the game starts
stuttering in time with garbage collection, because the object graph keeps getting bigger and every
reachability pass has more to walk. Epic's own documentation says as much: the spikes land in the frames
where the collection runs, *"especially if your application has destroyed a large number of objects
recently"*.

The engine gives you `obj list`, `memreport` and the `gc.` console variables. All of them either **configure**
or **write once**. None of them answers the question you actually have after forty minutes of play: *which
class is growing?* Today, people answer it by diffing two `memreport` text files by hand.

## The three questions

1. **How many UObjects are alive, broken down by class?**
2. **Which class is growing?** Not which is *big* — a game with 400 projectiles is healthy — but which gains
   objects every minute, fitted as a slope over a sliding window.
3. **What does the collection cost?** The last run, the average, the worst, the gap between runs, and how
   many objects each one freed.

## Quick start

1. Enable the plugin. Nothing else is required — the census starts itself.
2. Set your GameMode's HUD class to `AHeapCensusHUD` (or call `UHeapCensusSubsystem::DrawCounterBox` from
   your own `AHUD::DrawHUD` — one node, and nobody has to give up their own HUD).
3. Play. The counter box shows the totals and the five fastest-growing classes.
4. Type `Heap.Leak 200` in the console and watch a class climb the table. `Heap.Release` puts it back.

## The counter box

```
UObjects 214,882 | GC last 18.4 ms, worst 41.2 ms, every 62 s | census 3.1 ms
fastest growing - window 120 s, threshold +120/min
class                                           count   per minute
HeapCensusDemoObject                            4,812    +1,204/min  never fell back
...
```

The layout above is the format, not a measurement. The real figures depend on your project — which is the
point of measuring them.

Note the last number in the header: `census 3.1 ms` is what **this plugin's own walk over the object array**
cost. If you sell a measurement you have to put a price tag on it, and that is why the walk runs on an
interval (2 s by default) rather than every frame.

## Console

| Command | What it does |
| --- | --- |
| `Heap.Show` / `Heap.Hide` | Show or hide the counter box. |
| `Heap.Sample` | Walk the object array now, and print what the walk cost. |
| `Heap.Dump [growth\|count\|peak\|name]` | The whole table to the log. |
| `Heap.Top <n> [order]` | The top n classes to the log. |
| `Heap.Reset` | Throw the window and the collection statistics away. |
| `Heap.Report [path]` | Write the census as JSON. |
| `Heap.Threshold <n>` | The slope at which a class is called Leaking. |
| `Heap.Gate <seconds> [-noexit]` | Soak, write the report, exit 0 / 1 / 2. Also `HeapCensus.Gate`. |
| `Heap.Leak <n>` | Deliberately create and **hold** n objects a second. |
| `Heap.Churn <n>` | The same objects at the same rate, **released** again. The healthy control. |
| `Heap.Release [0\|1]` | Stop the demonstration, drop the references, collect. |

## The gate

```
UnrealEditor-Cmd.exe MyProject -game -ExecCmds="HeapCensus.Gate 3600"
```

Measures for an hour, fits a slope per class, writes `Saved/HeapCensus/report.json` and ends the process
with:

* **0** — nothing is growing.
* **1** — something is climbing, but it fell back at some point inside the soak. A wave, not yet a leak.
* **2** — a class broke the threshold **and never fell back once** across the whole measurement.

Those three exit codes mean the same three things in LoadLens, LocaleGuard, AssetWarden and WidgetLedger, so
a build server that already checks one of them needs no second convention.

## Blueprint

`UHeapCensusStatics` exposes the whole surface, and the arithmetic is static and world-free: `Slope`,
`RankByGrowth`, `EvaluateGrowth`, `EvaluateEntry`, `SummarizeWorst`. `OnClassGrowing` fires once per class,
the first time it reaches the Leaking verdict — not once per census.

## Settings

**Project Settings → Plugins → HeapCensus.** Sample interval, window size, settling window, growth threshold,
warning fraction, ignore list, counter box, report path, demonstration rate.

## Documentation

Full documentation, including the demo map, what the measurement costs and what to do next:
<https://wiki.teufel-engineering.com/en/HeapCensus/documentation>

---

© 2026 Silvan Teufel. All Rights Reserved.

<!-- SF-STORE-BLOCK:BEGIN -->
## 🛒 Source-available — see before you buy

This repository contains the **full source** of a commercial Unreal Engine plugin. It is **source-available, not open source**: read it, evaluate it, then buy a license to use it. See **the Fab Content License Agreement / Unreal Engine EULA (purchase required)**.

**Get it / Buy:**
- **Buy on Fab** (this plugin): https://www.fab.com/listings/c0c4b79d-7e1d-44b5-89f6-f85823800994
- Fab store — all our UE5 plugins: https://www.fab.com/sellers/Silvan%20Teufel

### 📬 **Free UE5 Snippet-Pack**

10 ready-to-use C++/Blueprint building blocks (subsystems, versioned saves, async nodes, editor tooling) — MIT licensed. Get it by joining the newsletter — plus a heads-up when something new ships. Double opt-in, unsubscribe in one click, no address sharing.

👉 **[Get the free pack](https://silvan.teufel-engineering.com/newsletter/plugins/?q=gh)**

_© 2026 Silvan Teufel. All rights reserved._
<!-- SF-STORE-BLOCK:END -->
