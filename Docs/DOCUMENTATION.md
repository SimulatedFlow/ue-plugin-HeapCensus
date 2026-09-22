# HeapCensus — Documentation

**Which class is growing, and what garbage collection costs.**

Unreal Engine 5.8 · Win64 · one runtime module · no editor module · works in a packaged Shipping build.

Online: <https://wiki.teufel-engineering.com/en/HeapCensus/documentation>
Support: <mailto:teufelsilvan@gmail.com>

---

## Contents

1. [What the plugin does, and what it does not](#1-what-the-plugin-does-and-what-it-does-not)
2. [Supported engine and platforms](#2-supported-engine-and-platforms)
3. [Installation](#3-installation)
4. [Quick start — five minutes](#4-quick-start--five-minutes)
5. [The demo map](#5-the-demo-map)
6. [The counter box, line by line](#6-the-counter-box-line-by-line)
7. [How growth is measured](#7-how-growth-is-measured)
8. [What the collection costs](#8-what-the-collection-costs)
9. [What the measurement costs](#9-what-the-measurement-costs)
10. [Console commands](#10-console-commands)
11. [The gate](#11-the-gate)
12. [Project settings](#12-project-settings)
13. [API overview — classes and types](#13-api-overview--classes-and-types)
14. [Code examples](#14-code-examples)
15. [The report](#15-the-report)
16. [And now? — `obj refs` as the next step](#16-and-now--obj-refs-as-the-next-step)
17. [Tests](#17-tests)
18. [Troubleshooting](#18-troubleshooting)
19. [Version history](#19-version-history)

---

## 1. What the plugin does, and what it does not

### What it does

HeapCensus walks the global UObject array on a timer and counts live objects **per class**. It keeps a ring
buffer of the last N counts for every class, fits a least-squares line through it, and reports the result as
**objects per minute**. Around that it hooks the engine's own garbage-collection delegates and measures what
the collection costs: the last run, the mean, the worst, the gap between runs, the purge that follows, and
how many object slots each run freed.

Everything it knows is on the screen in a Canvas counter box that survives cooking, available in Blueprint,
readable from C++, writable as JSON, and reducible to a build-server exit code.

### What it does *not* do

This section is first for a reason.

* **It does not find leaks in raw C++ memory.** There is no `new` hook, no allocator instrumentation, no
  malloc tracking. A `TArray<uint8>` that grows forever inside a struct that is not a UObject is invisible to
  this plugin. Use `memreport`, LLM or Memory Insights for that.
* **It does not replace Memory Insights.** Insights is a recorder and a better one than anything a plugin
  could be. What Insights is not is a number on the screen while somebody plays the build you will ship, in a
  session nobody planned to profile.
* **It does not tell you who is holding the object.** It tells you *what* is growing and *how fast*. The next
  question — who is keeping it alive — is answered by the engine's own `obj refs`, and
  [section 16](#16-and-now--obj-refs-as-the-next-step) is about exactly that handover.
* **It does not measure bytes.** It measures objects. An object is not a fixed number of bytes, and a plugin
  that multiplied a count by an average would be producing a number that looks like memory and is not.

---

## 2. Supported engine and platforms

| | |
| --- | --- |
| **Engine version** | Unreal Engine **5.8** (`"EngineVersion": "5.8.0"` in the `.uplugin`) |
| **Supported development platforms** | Win64 |
| **Supported target build platforms** | Win64 (`PlatformAllowList: ["Win64"]`) |
| **Module** | `HeapCensus`, type `Runtime`, `LoadingPhase: PreDefault` — one module, no editor module |
| **Build configurations** | Verified for UnrealEditor Development, UnrealGame Development and UnrealGame **Shipping** |
| **Project type** | C++ **and** Blueprint-only projects. Everything is reachable from Blueprint and from the console; no C++ is required to use the plugin. |
| **Public dependencies** | `Core`, `CoreUObject`, `Engine`, `DeveloperSettings` |
| **Private dependencies** | `RenderCore` (the box background), `Json`, `JsonUtilities` (the report) |
| **Not linked** | `UnrealEd`, `UMG`, `Slate` — deliberately, so nothing here is editor-only |
| **Networking** | Not replicated. A census is per-process by nature; run it on the client, on a listen server, or on a dedicated server, and each measures itself. |
| **Third-party code** | None. No external libraries, no plugin dependencies. |

The plugin is written against portable engine API only (`FThreadSafeObjectIterator`, `GUObjectArray`,
`FCoreUObjectDelegates`, `UCanvas`). The `Win64` allow-list is what has been **built and tested**, not a
technical limit; if you need another platform, mail support and it can be added to the allow-list.

---

## 3. Installation

### From Fab

1. Install HeapCensus to your engine version from the Epic Games Launcher / Fab library.
2. Open your project and enable it under **Edit → Plugins → Engine Tools → HeapCensus**.
3. Restart the editor when prompted.

### From a copy of the plugin folder

1. Copy the `HeapCensus` folder into `<YourProject>/Plugins/` so that
   `<YourProject>/Plugins/HeapCensus/HeapCensus.uplugin` exists.
2. Right-click your `.uproject` → **Generate Visual Studio project files** (C++ projects only).
3. Open the project. The editor offers to build the missing module; accept, or build
   `<YourProject>Editor Win64 Development` from your IDE first.
4. Check **Edit → Plugins → Engine Tools** that HeapCensus is enabled.

The census starts itself. The module loads at `PreDefault`, so the very first collections of the session are
timed too — you do not have to place an actor or call an initialiser.

### Using it from your own C++ module

Add the module to your `Build.cs`:

```csharp
PublicDependencyModuleNames.AddRange(new string[] { "HeapCensus" });
```

Then include what you need:

```cpp
#include "HeapCensusSubsystem.h"   // the live census
#include "HeapCensusStatics.h"     // the arithmetic + the Blueprint library
#include "HeapCensusTypes.h"       // FHeapClassEntry, FHeapGCStats, FHeapCensusSummary, the two enums
#include "HeapCensusHUD.h"         // AHeapCensusHUD, if you want the ready-made HUD
```

---

## 4. Quick start — five minutes

1. **Show the box.** Either set your GameMode's **HUD Class** to `HeapCensus HUD` (`AHeapCensusHUD`), or add
   one node to your existing HUD's `Event Receive Draw HUD`: **Draw Counter Box** on the HeapCensus subsystem,
   passing the HUD's Canvas. The second way exists so nobody has to choose between their own HUD and this one.
2. **Press Play.** After the settling window (5 s by default) the box fills in with the object total, the
   collection figures, and the five fastest-growing classes.
3. **Make something leak on purpose.** Open the console and type:

   ```
   Heap.Leak 200
   ```

   Within two or three censuses `HeapCensusDemoObject` climbs to the top of the table with a positive
   `+n/min`, the row turns red, and it is marked *never fell back*.
4. **Prove it was a measurement.** Type:

   ```
   Heap.Churn 200
   ```

   The same objects at the same rate, but released again. The count wobbles and the slope returns to zero.
   If the healthy case also went red, the table would be measuring activity rather than growth.
5. **Clean up.** `Heap.Release` drops the references and asks for a collection. The count falls back, the
   GC figures in the header update with what clearing up cost, and the box turns green again.
6. **Point it at your own game.** `Heap.Top 20` prints the twenty fastest-growing classes to the log. Play
   for ten minutes and run it again.

If step 3 does nothing, the box is hidden (`Heap.Show`), the census interval is set to `0`, or you are in the
editor and the class table is at its cap — see the note in [section 5](#5-the-demo-map).

---

## 5. The demo map

Content path: `/HeapCensus/HeapCensus/Maps/L_HeapCensusDemo`
(on disk: `Plugins/HeapCensus/Content/HeapCensus/Maps/L_HeapCensusDemo.umap`).

Open it, press Play, and use the buttons on the panel. They are the whole product in thirty seconds, and the
difference between them is measured, not staged:

| Button | What happens | What the box shows |
| --- | --- | --- |
| **START LEAK** | Demo objects are created at 200 a second and every one is **kept** in an array. | The count climbs. `HeapCensusDemoObject` moves to the top of the growth table with `+n/min`, the row goes red and is marked *never fell back*. |
| **START CHURN** | The same objects at the same rate, **released** again, with a collection asked for every few seconds. | The count wobbles. The slope stays around zero. Nothing goes red, and the last line reads *nothing is growing*. |
| **RELEASE + COLLECT** | The references are dropped and a collection is asked for. | The count falls back within a census, the GC figures in the header update with what clearing up cost, and the verdict drops from *Leaking* to *a wave, not yet a leak*. |
| **SAMPLE NOW** | One census out of turn — `SampleHeapCensus`. | The header's `census n.n ms` is the price of that walk. |
| **RESET CENSUS** | The window and the collection statistics are thrown away. | The table starts again from *too little data*. |

The only difference between the first two buttons is whether anything keeps a reference. Same class, same
rate, same objects. That is the point: if the healthy case also went red, the growth table would be measuring
activity rather than growth, and it would be worthless.

A wall of instanced blocks in the middle of the map grows **one block per twenty held objects**. It reads
`GetHeapCensusDemoObjectCount` and nothing else, so the wall cannot show a leak the census does not also
report.

### What is in the map

| Asset | What it is |
| --- | --- |
| `Maps/L_HeapCensusDemo` | The map. Its GameMode is `BP_HeapCensusDemoGameMode`; that is the only wiring the counter box needs. |
| `Blueprints/BP_HeapCensusDemoHUD` | An `AHeapCensusHUD` child. The parent draws the counter box; this adds the panel and a cursor, and clears any demonstration left running on the way in and out. |
| `Blueprints/BP_HeapCensusDemoGameMode` | Sets that HUD class. Nothing else. |
| `Blueprints/BP_HeapCensusHeapWall` | The leak, made visible: one instanced block per 20 held objects. |
| `UI/WBP_HeapCensusDemoPanel` | The five buttons. One call into `UHeapCensusStatics` each — `StartHeapCensusLeak`, `StartHeapCensusChurn`, `StopHeapCensusDemo`, `SampleHeapCensus`, `ResetHeapCensus`. |
| `Materials/M_HeapCensusBlock`, `M_HeapCensusFloor` | Two parameter-driven materials, so the map pulls in nothing from outside the plugin. |

The demo content has **zero non-engine dependencies** and lives entirely under the single Fab pack folder
`/HeapCensus/HeapCensus/`.

### Note on the healthy button

Objects nobody references are still in the object array until a collection removes them, and the engine's
default collection interval is a minute. So the churn demonstration asks for a collection every few seconds;
that is the only thing about it that is accelerated, and it is what makes the healthy case read as a wobble
around zero inside a two-minute window instead of a slow ramp between two distant collections. Your game will
show the same shape at its own collection cadence.

### Note on running the demo inside the editor

An editor process has roughly **1,650 distinct UObject classes** alive before a map is even loaded, which is
past the shipped `Max Tracked Classes` default of 1,024. A class that first appears after the table is full is
still counted in the object total, but it gets no row of its own — so `HeapCensusDemoObject` never reaches the
growth table. The counter box says so (*n class(es) counted but not listed - the table is at its cap*), and
the fix is one setting:

```ini
[/Script/HeapCensus.HeapCensusSettings]
MaxTrackedClasses=8192
```

A packaged game is nowhere near the default cap; this is an editor-only concern, and it is the plugin's cap
working as designed rather than a bug.

---

## 6. The counter box, line by line

This is a **real capture** from an editor play-in-editor session with the leak running — an editor process is
the worst case for both the object total and the walk cost:

```
UObjects 173,537 | GC not seen yet | census 13.6 ms
fastest growing - window 46 s, threshold +120/min
class                             count   per minute
HeapCensusDemoObject              9,121   +12,060/min  never fell back
Function                         19,621        0/min
BlueprintFunctionNodeSpawner     16,589        0/min
BlueprintFieldNodeSpawner        13,974        0/min
K2Node_CallFunction              13,694        0/min
2 class(es) hidden by the ignore list - their objects are still in the total above
HeapCensusDemoObject is growing: 9,121 live, +12,060/min, and it has not fallen once in 44 s.
```

Note what that capture demonstrates on its own: the four biggest classes in the list are all far larger than
the leaking one, and every one of them is at `0/min`. Ranked by size, the leak is invisible. Ranked by slope,
it is the first row.

**Line 1 — the header.** Live UObjects (class default objects excluded), then the collection: the last run,
the worst run and the mean gap between runs — or `GC not seen yet` when no collection has happened since the
plugin started. Then `census`, which is what this plugin's own walk cost — see
[section 9](#9-what-the-measurement-costs). The whole line takes the colour of the worst verdict in the table.

**Line 2 — the heading.** `fastest growing`, and it is a promise: this list is ordered by **slope**, not by
size. `obj list` already ranks by size. During the settling window this line says so instead, so a quiet box
is never mistaken for a healthy one.

**Line 3 onwards — the rows.** Class, live count, slope. A row that has fewer than three samples behind it
says `too little data` rather than showing an invented number. A row that has climbed without ever falling
back says `never fell back` — that is the shape of a leak, and it is the difference between a red row and an
amber one.

**The footnotes.** How many classes the ignore list dropped, and how many were counted but not listed because
the table is at its cap. Neither can hide silently.

**The last line.** One sentence naming the thing to go and look at. It is worth more than the table above it.

Position and row count are configurable — see [section 12](#12-project-settings).

---

## 7. How growth is measured

### The window

Every class gets a fixed-size ring buffer — 60 slots by default. At a 2-second census interval that is two
minutes of history, in a few hundred kilobytes that never grow however long the game runs.

A class that appears mid-window starts with a short series and is judged accordingly. A class that drains to
zero has zeros pushed into its ring (so it shows as *shrinking* rather than freezing at its last count), and
once its whole window is zeros it is dropped from the table entirely.

### The slope

A least-squares line through the window, converted to objects per minute:

```
slope per sample = Σ (i − ī)(y_i − ȳ) / Σ (i − ī)²
slope per minute = slope per sample × 60 / interval
```

A line rather than "last minus first", because the last sample can land in the middle of a spawn burst and
would then report a leak that is really a wave. A flat series returns **exactly** zero, not almost zero: a
box full of `+0.02/min` on an idle game reads as noise, and a tool that reads as noise gets switched off.

### The refusal

Below three samples there is no slope. Two points always describe a perfect straight line, and a perfect
straight line through two samples taken two seconds apart is how a tool reports a leak because somebody
opened a menu. Under three samples the verdict is `Ok` and the note says `too little data (2 of 3 samples)`.

### The verdict

| | |
| --- | --- |
| **Ok** | Below the warning fraction of the threshold — or shrinking, or too little data. |
| **Warn** | At or above `threshold × WarnFraction` (60/min with the defaults). Or: past the threshold but the count fell back at some point inside the window. |
| **Leaking** | At or above the threshold (120/min by default) **and** the count never fell back. |

The boundary is **inclusive** and is tested for it: with the defaults, 59.9 is Ok, 60 is Warn, 119.9 is Warn,
120 is Leaking. A threshold somebody typed as "120" has to mean 120, or the number in the settings page and
the number in the report are two different numbers.

That last condition is the one that makes the plugin usable. A pool filling up has a positive slope for as
long as it is filling and is not a leak; a leak climbs and never gives anything back. Requiring the series to
be monotonic is what tells the two apart. Set **Require Monotonic Growth** to false if you would rather see
every climb.

### The settling window

A level coming up creates tens of thousands of objects in a second or two. That is a map load, not growth.
HeapCensus therefore throws its window away and stops sampling for **Settle Seconds** whenever a game world
arrives, and says `settling` in the box while it does. Both hooks are used —
`FCoreUObjectDelegates::PostLoadMapWithWorld` *and* `FWorldDelegates::OnPostWorldInitialization` — because
the first is not broadcast for play-in-editor, which is the most common way anybody will ever run this.

---

## 8. What the collection costs

HeapCensus hangs on three engine delegates and reports **two** figures, because they are two different
things and adding them together would produce a number nobody could act on.

| Figure | Measured between | What it means |
| --- | --- | --- |
| **Collect** (`GC last`) | `GetPreGarbageCollectDelegate` → `GetPostGarbageCollect` | Reachability analysis. This holds the game thread. This is the hitch. |
| **Purge** | `GetPostGarbageCollect` → `GarbageCollectComplete` | Destroying and freeing what was found. With incremental purge on, it runs in slices across several frames — so it is *not* one stall, and the report says when it spanned frames. |

Also recorded: the number of collections, the mean and worst collect time, the gap between runs (how often
the hitch comes back), and how many object-array slots the last run freed — taken from the array's own
claimed-slot count before and after, which costs one subtraction and answers "did that collection actually
get anything back".

> **UE 5.8 note.** The engine's post-collection delegate is `FCoreUObjectDelegates::GetPostGarbageCollect()`.
> There is no `GetPostGarbageCollectDelegate()` in 5.8, despite the symmetry with the pre-collection one.

---

## 9. What the measurement costs

**This is the section a profiling plugin owes you.**

The walk is `O(size of the global object array)`. On a large project that array holds hundreds of thousands
of entries, and walking it is measurable — the capture in [section 6](#6-the-counter-box-line-by-line) shows
**13.6 ms** at 173,537 live objects **in an editor process**, which is the worst case: an editor holds every
Blueprint node spawner, every asset registry entry and every open tool alive at once. Three consequences, all
of them deliberate:

1. **It does not run every frame.** It runs every `SampleIntervalSeconds` — 2 by default. At 60 fps, running
   it per frame would cost thirty times more than it does now and would make this plugin more expensive than
   most of the things it exists to find.
2. **Its own cost is printed in the counter box**, next to the numbers it produced, every single time. If you
   sell a measurement you have to put a price tag on it.
3. **It does not allocate.** The two scratch maps are kept between censuses and reset rather than freed, the
   tally is keyed by `UClass*` so nothing resolves an `FName` or touches a string inside the loop, and class
   names are looked up once per class afterwards — there are three orders of magnitude fewer classes than
   objects. A tool that measures allocation pressure has no business adding to it.

Filtering is done by the iterator's own exclusion flags rather than by an `if` inside the loop: class default
objects are skipped (they are constants and would add a fixed offset to every class), and objects already
marked Garbage are skipped (they are dead, and counting them would make every collection look like a leak
that just repaired itself).

If the census cost bothers you on your project, raise the interval. Set it to `0` to switch automatic
sampling off entirely and drive it yourself with `Heap.Sample` — the collection timing keeps working either
way, because it costs nothing between collections.

---

## 10. Console commands

Eleven commands, plus the gate under a second name. None of them is editor-only: they behave the same in
play-in-editor, in a `-game` run and in a packaged Shipping build, which is where a leak that takes forty
minutes to appear actually appears.

| Command | What it does |
| --- | --- |
| `Heap.Show [0\|1]` | Show the counter box. |
| `Heap.Hide` | Hide it. |
| `Heap.Sample` | Walk the object array now, out of turn, and log what the walk cost. |
| `Heap.Dump [growth\|count\|peak\|name]` | The whole table to the log, plus the collection figures. |
| `Heap.Top <n> [order]` | The top n classes to the log. Default 10, by growth. |
| `Heap.Reset` | Throw the window and the collection statistics away. |
| `Heap.Report [path]` | Write the census as JSON. Default: the project settings path. |
| `Heap.Threshold <n>` | Set the slope at which a class is called Leaking. No argument prints the current one. |
| `Heap.Gate <s> [-noexit]` | The gate. See [section 11](#11-the-gate). Also registered as `HeapCensus.Gate`. |
| `Heap.Leak <n>` | Create and **hold** n demo objects a second. `0` stops it. |
| `Heap.Churn <n>` | Create n demo objects a second and **release** them. The healthy control. |
| `Heap.Release [0\|1]` | Stop the demonstration, drop the references, and collect (default yes). |

`Heap.Leak` exists so you can watch the growth table react without first building a leak into your own game.
`Heap.Churn` exists because without the control, "the number went up" proves nothing.

---

## 11. The gate

```
UnrealEditor-Cmd.exe MyProject -game -nullrhi -ExecCmds="HeapCensus.Gate 3600"
echo %ERRORLEVEL%
```

The gate resets the census, waits out the settling window, measures for the given number of seconds, takes a
final census, writes the report and ends the process with:

| Exit code | Verdict | Means |
| --- | --- | --- |
| **0** | Ok | Nothing is growing past the warning fraction. |
| **1** | Warn | Something is climbing — but it fell back at some point, or it is between the warning fraction and the threshold. |
| **2** | Leaking | A class broke the threshold **and never fell back once** across the whole measurement. |

Those three codes mean the same three things in LoadLens, LocaleGuard, AssetWarden and WidgetLedger, so a
build server that already checks one of them needs no second convention.

`-noexit` measures and reports without ending the process, which is what you want when you are typing this
into the console rather than running it from a build script.

The log lines a build server can grep for:

```
HEAPCENSUS GATE RESULT=Leaking objects=241033 classes=612 worst=HeapCensusDemoObject slope=1204.0/min threshold=120 gc_worst_ms=41.2 exit=2
HEAPCENSUS GATE WORST=HeapCensusDemoObject is growing: 4,812 live, +1,204/min, and it has not fallen once in 3600 s.
```

(The figures in that example are the shape of the line, not a measurement of your project.)

**This is the case a build server should find overnight and a person never will:** a class that gains four
hundred objects a minute for six hours while nobody is watching.

A minimal CI step:

```bat
UnrealEditor-Cmd.exe "%CD%\MyProject.uproject" -game -nullrhi -unattended -nopause ^
    -ExecCmds="HeapCensus.Gate 1800"
if %ERRORLEVEL% GEQ 2 exit /b 1
```

---

## 12. Project settings

**Project Settings → Plugins → HeapCensus.** Stored in `DefaultGame.ini` under
`[/Script/HeapCensus.HeapCensusSettings]`. Changes take effect on the next census, not after the next
restart.

### Census

| Setting | Default | What it does |
| --- | --- | --- |
| Sample Interval Seconds | 2.0 | Seconds between walks. `0` switches automatic sampling off. |
| Window Samples | 60 | Samples kept per class. 60 × 2 s = two minutes of history. |
| Settle Seconds | 5.0 | After a map change, samples are discarded for this long. |
| Max Tracked Classes | 1024 | Cap on distinct rows. Anything beyond is counted but not named. Raise it in the editor — see [section 5](#5-the-demo-map). |
| Ignored Class Names | `Package`, `ObjectRedirector`, `LinkerPlaceholder*` | Never get a row. Case-insensitive; a trailing `*` is a prefix match. |

The ignore list is three entries long on purpose. Every name on it is engine bookkeeping that swings with
package loading and that a project cannot act on. Nothing gameplay-shaped is on it, because a census that
quietly hides classes is worse than no census — and whatever it drops is still inside the object total, with
the number of dropped classes printed in the box.

### Growth

| Setting | Default | What it does |
| --- | --- | --- |
| Growth Threshold Per Minute | 120 | At or above this, a class is Leaking. Two objects a second, sustained. |
| Warn Fraction | 0.5 | The fraction of the threshold at which a class becomes Warn. `1.0` removes the band. |
| Require Monotonic Growth | true | Only classes that never fell back can reach Leaking. |

The useful threshold is the lowest one that does not go off on a healthy build of your game. Start at 120 and
lower it once the project is clean.

### Counter Box

| Setting | Default | What it does |
| --- | --- | --- |
| Show Counter Box | true | Draw the box at all. Same as `Heap.Show` / `Heap.Hide`. |
| Top Class Lines | 5 | How many classes the box lists, fastest-growing first. |
| Counter Box Position | (24, 90) | Top-left corner, in pixels. |

### Report

| Setting | Default | What it does |
| --- | --- | --- |
| Report Path | `Saved/HeapCensus/report.json` | Where `Heap.Report` and the gate write. Relative to the project directory. |
| Log Growing Classes | true | One log line the first time a class breaks the threshold — per class per crossing, never one per census. |

### Demonstration

| Setting | Default | What it does |
| --- | --- | --- |
| Demo Objects Per Second | 200 | Rate for `Heap.Leak` / `Heap.Churn` when no number is given. |
| Demo Payload Kilobytes | 4 | Memory each demo object carries. |

The payload exists so that a leak demonstrated with this plugin also moves the numbers in Task Manager and
Memory Insights — so a sceptical buyer can check this plugin against another one. It changes nothing
HeapCensus itself reports, because HeapCensus counts objects.

### Example `DefaultGame.ini`

```ini
[/Script/HeapCensus.HeapCensusSettings]
SampleIntervalSeconds=2.0
WindowSamples=60
SettleSeconds=5.0
MaxTrackedClasses=8192
GrowthThresholdPerMinute=120.0
WarnFraction=0.5
bRequireMonotonicGrowth=True
bShowCounterBox=True
TopClassLines=5
ReportPath=Saved/HeapCensus/report.json
+IgnoredClassNames=Package
+IgnoredClassNames=ObjectRedirector
+IgnoredClassNames=LinkerPlaceholder*
```

---

## 13. API overview — classes and types

Six C++ classes, all `HEAPCENSUS_API` and all reachable from Blueprint unless noted.

### `UHeapCensusSubsystem : UEngineSubsystem`

The Blueprint-facing face of the census, and the thing that draws the counter box. An **engine** subsystem,
not a world subsystem, and that is the point: a leak survives a map change, and the interesting question is
often exactly whether the count came back down after one.

| Member | Signature | What it does |
| --- | --- | --- |
| `Get()` | `static UHeapCensusSubsystem*` | The one subsystem, or null before the engine is up. |
| `Sample()` | `FHeapCensusSummary Sample()` | Walk now, out of turn, and return the summary. |
| `GetSummary()` | `FHeapCensusSummary GetSummary() const` | The last summary, without walking anything. |
| `GetTop()` | `TArray<FHeapClassEntry> GetTop(int32 N = 5, EHeapSort Sort = Growth) const` | The N classes at the top of the given order. `0` returns all. |
| `GetGCStats()` | `FHeapGCStats GetGCStats() const` | What the collection costs. |
| `GetTotalObjects()` | `int32 GetTotalObjects() const` | Live UObjects at the last census. |
| `IsLeaking()` | `bool IsLeaking() const` | True when at least one class is Leaking. |
| `Reset()` | `void Reset()` | Throw the window and the collection statistics away. |
| `WriteReport()` | `bool WriteReport(const FString& Path)` | Write the JSON. Empty path = the settings path. |
| `DumpToLog()` | `void DumpToLog()` | The whole table to the log. |
| `SetGrowthThreshold()` / `GetGrowthThreshold()` | `void(float)` / `float()` | The threshold, at runtime. |
| `SetShowCounterBox()` / `IsShowingCounterBox()` | `void(bool)` / `bool()` | The box. |
| `DrawCounterBox()` | `void DrawCounterBox(UCanvas* Canvas)` | Draw the box from your own HUD. |
| `OnClassGrowing` | `FHeapCensusClassGrowingSignature` | Fires **once** when a class first reaches Leaking, not once per census. Params: `const FString& ClassName, float SlopePerMinute, EHeapVerdict Verdict`. |

### `UHeapCensusStatics : UBlueprintFunctionLibrary`

**The arithmetic — static, world-free and therefore fully testable.** Fitting a line, ranking by growth rather
than by size, judging a slope against a threshold and refusing to invent a slope from two samples are the four
places this plugin could be quietly wrong, and none of them needs a running game.

| Function | Signature |
| --- | --- |
| `MinimumSamples` | `static int32 MinimumSamples()` — three |
| `Slope` | `static float Slope(const TArray<float>& Samples, float SampleIntervalSeconds = 2.f)` |
| `IsMonotonicallyGrowing` | `static bool IsMonotonicallyGrowing(const TArray<float>& Samples)` |
| `EvaluateGrowth` | `static EHeapVerdict EvaluateGrowth(float SlopePerMinute, float ThresholdPerMinute = 120.f, float WarnFraction = 0.5f)` |
| `EvaluateEntry` | `static EHeapVerdict EvaluateEntry(const FHeapClassEntry&, float Threshold = 120.f, float WarnFraction = 0.5f, bool bRequireMonotonic = true)` |
| `GrowthNote` | `static FString GrowthNote(const FHeapClassEntry&)` |
| `RankByGrowth` | `static TArray<FHeapClassEntry> RankByGrowth(const TArray<FHeapClassEntry>&)` |
| `RankEntries` | `static TArray<FHeapClassEntry> RankEntries(const TArray<FHeapClassEntry>&, EHeapSort = Growth)` |
| `SummarizeWorst` | `static FString SummarizeWorst(const TArray<FHeapClassEntry>&)` |
| `IsClassIgnored` | `static bool IsClassIgnored(const FString& ClassName, const TArray<FString>& IgnoredClassNames)` |
| `VerdictToString` / `VerdictToExitCode` | `static FString(EHeapVerdict)` / `static int32(EHeapVerdict)` |
| `FormatCount` / `FormatSlope` | `static FString(int32)` / `static FString(float)` — locale-free grouping, always-signed slopes |
| `FindWorst` | `static EHeapVerdict FindWorst(const TArray<FHeapClassEntry>&, FString& OutClassName, float& OutSlopePerMinute)` (C++ only) |

Live accessors, for Blueprint convenience: `GetHeapCensus`, `SampleHeapCensus`, `GetHeapCensusSummary`,
`GetTopClasses`, `GetHeapCensusGCStats`, `ResetHeapCensus`, `SetHeapCensusCounterBoxVisible`.

The demonstration: `StartHeapCensusLeak(int32 ObjectsPerSecond = 200)`,
`StartHeapCensusChurn(int32 ObjectsPerSecond = 200)`, `StopHeapCensusDemo(bool bCollectGarbage = true)`,
`GetHeapCensusDemoObjectCount()`.

### `AHeapCensusHUD : AHUD`

A thin `Blueprintable` `AHUD` that calls `DrawCounterBox` from `DrawHUD`. Set it as your GameMode's HUD class,
or ignore it and call `DrawCounterBox` from your own. One property: `bDrawCounterBox` (EditAnywhere,
BlueprintReadWrite).

### `UHeapCensusSettings : UDeveloperSettings`

The project settings of [section 12](#12-project-settings). `Config = Game`, `DefaultConfig`, appears under
**Plugins → HeapCensus**. `UHeapCensusSettings::Get()` never returns null.

### Types

**`EHeapVerdict { Ok, Warn, Leaking }`** — also the three exit codes of the gate.

**`EHeapSort { Growth, Count, Peak, Name }`** — Growth is the default, and it is the whole point.

**`FHeapClassEntry`** — `ClassName`, `Count`, `Peak`, `FirstCount`, `SlopePerMinute`, `SampleCount`,
`WindowSeconds`, `bMonotonic`, `Verdict`.

**`FHeapGCStats`** — `RunCount`, `LastMilliseconds`, `AverageMilliseconds`, `WorstMilliseconds`,
`LastPurgeMilliseconds`, `WorstPurgeMilliseconds`, `bLastPurgeSpannedFrames`, `LastIntervalSeconds`,
`AverageIntervalSeconds`, `SecondsSinceLastRun`, `LastObjectsFreed`, `bRunning`.

**`FHeapCensusSummary`** — `TotalObjects`, `TrackedClasses`, `IgnoredClasses`, `UntrackedClasses`,
`SampleCount`, `SampleIntervalSeconds`, `WindowSeconds`, `GrowthThresholdPerMinute`,
`LastCensusMilliseconds`, `AverageCensusMilliseconds`, `WorstCensusMilliseconds`, `bSettling`,
`SettleSecondsRemaining`, `Verdict`, `WorstClass`, `WorstSlopePerMinute`, `GC`.

---

## 14. Code examples

### Read the census from C++

```cpp
#include "HeapCensusSubsystem.h"
#include "HeapCensusStatics.h"

void AMyGameStateBase::LogHeapSnapshot()
{
    UHeapCensusSubsystem* Census = UHeapCensusSubsystem::Get();
    if (!Census)
    {
        return; // Engine is not up yet.
    }

    const FHeapCensusSummary Summary = Census->GetSummary();

    UE_LOG(LogTemp, Display, TEXT("%s live UObjects in %d classes; last census cost %.2f ms."),
        *UHeapCensusStatics::FormatCount(Summary.TotalObjects),
        Summary.TrackedClasses,
        Summary.LastCensusMilliseconds);

    for (const FHeapClassEntry& Entry : Census->GetTop(5, EHeapSort::Growth))
    {
        UE_LOG(LogTemp, Display, TEXT("  %-40s %8s %12s %s"),
            *Entry.ClassName,
            *UHeapCensusStatics::FormatCount(Entry.Count),
            *UHeapCensusStatics::FormatSlope(Entry.SlopePerMinute),
            Entry.bMonotonic ? TEXT("never fell back") : TEXT(""));
    }
}
```

### React when a class starts leaking

`OnClassGrowing` fires **once** per class per episode — not once per census — so it is safe to do something
expensive in the handler.

```cpp
// MyLeakWatcher.h
UCLASS()
class UMyLeakWatcher : public UObject
{
    GENERATED_BODY()

public:
    void Start();

private:
    UFUNCTION()
    void HandleClassGrowing(const FString& ClassName, float SlopePerMinute, EHeapVerdict Verdict);
};
```

```cpp
// MyLeakWatcher.cpp
#include "HeapCensusSubsystem.h"

void UMyLeakWatcher::Start()
{
    if (UHeapCensusSubsystem* Census = UHeapCensusSubsystem::Get())
    {
        Census->OnClassGrowing.AddDynamic(this, &UMyLeakWatcher::HandleClassGrowing);
    }
}

void UMyLeakWatcher::HandleClassGrowing(const FString& ClassName, float SlopePerMinute, EHeapVerdict Verdict)
{
    // HeapCensus deliberately sends nothing anywhere. This is where you would put a marker in your own
    // telemetry, take a screenshot, or write the report out for the session.
    UE_LOG(LogTemp, Warning, TEXT("HeapCensus: %s is %s at %s"),
        *ClassName,
        *UHeapCensusStatics::VerdictToString(Verdict),
        *UHeapCensusStatics::FormatSlope(SlopePerMinute));

    if (UHeapCensusSubsystem* Census = UHeapCensusSubsystem::Get())
    {
        Census->WriteReport(FString()); // Empty path = the project settings path.
    }
}
```

### Measure one specific thing

The census runs on a timer, but `Sample()` walks on demand — it is here for the moment before and the moment
after something you want to measure. Do **not** call it every frame; see
[section 9](#9-what-the-measurement-costs).

```cpp
UHeapCensusSubsystem* Census = UHeapCensusSubsystem::Get();

const FHeapCensusSummary Before = Census->Sample();
OpenTheInventoryScreen();
CloseTheInventoryScreen();
const FHeapCensusSummary After = Census->Sample();

const int32 Delta = After.TotalObjects - Before.TotalObjects;
UE_LOG(LogTemp, Display, TEXT("Opening and closing the inventory left %d UObjects behind."), Delta);
```

Nothing was collected in between, so a positive delta here is "objects created and not yet collected", not
proof of a leak. Force a collection first if you want the stronger statement:

```cpp
GEngine->ForceGarbageCollection(true);
```

### Draw the box from your own HUD

The box is one call. This is the escape hatch that means nobody has to choose between their own HUD class and
`AHeapCensusHUD`.

```cpp
void AMyHUD::DrawHUD()
{
    Super::DrawHUD();

    if (UHeapCensusSubsystem* Census = UHeapCensusSubsystem::Get())
    {
        Census->DrawCounterBox(Canvas);
    }
}
```

In Blueprint: on `Event Receive Draw HUD`, call **Get Heap Census** → **Draw Counter Box**, and pass the HUD's
`Canvas` (a `Get Canvas` / the HUD's `Canvas` variable).

### Use the arithmetic on your own numbers

Every function in the first half of `UHeapCensusStatics` is static and needs no world, no engine and no
objects — you can feed it your own series.

```cpp
// Objects counted every 2 seconds, oldest first.
const TArray<float> Series = { 100.f, 140.f, 180.f, 220.f, 260.f };

const float PerMinute = UHeapCensusStatics::Slope(Series, 2.0f);          // +1200.0
const bool  bClimbing = UHeapCensusStatics::IsMonotonicallyGrowing(Series); // true

const EHeapVerdict Verdict = UHeapCensusStatics::EvaluateGrowth(PerMinute, /*Threshold*/ 120.f);
// EHeapVerdict::Leaking -> UHeapCensusStatics::VerdictToExitCode(Verdict) == 2
```

### Rank a table yourself

```cpp
TArray<FHeapClassEntry> Entries = UHeapCensusSubsystem::Get()->GetTop(0); // 0 = every class
Entries = UHeapCensusStatics::RankEntries(Entries, EHeapSort::Count);     // biggest first, obj-list style

const FString Headline = UHeapCensusStatics::SummarizeWorst(Entries);
// "HeapCensusDemoObject is growing: 9,121 live, +12,060/min, and it has not fallen once in 44 s."
```

`SummarizeWorst` never returns an empty string — an empty table gets a clean sentence, because a blank line at
the bottom of the counter box looks like a bug and somebody will report it as one.

### Blueprint

Every one of the above is a node. The demo panel's five buttons are one call each:

| Button | Node |
| --- | --- |
| START LEAK | `Start Heap Census Leak` (Objects Per Second = 200) |
| START CHURN | `Start Heap Census Churn` (Objects Per Second = 200) |
| RELEASE + COLLECT | `Stop Heap Census Demo` (Collect Garbage = true) |
| SAMPLE NOW | `Sample Heap Census` |
| RESET CENSUS | `Reset Heap Census` |

For a HUD widget of your own, `Get Heap Census Summary` and `Get Top Classes` are pure nodes — bind them
straight to text with `Format Count` and `Format Slope` so your numbers read the same as the counter box's.

---

## 15. The report

`Saved/HeapCensus/report.json` by default. Written by `Heap.Report` and by every gate run, so a developer and
a build server end up looking at the same document.

```json
{
  "plugin": "HeapCensus",
  "reportVersion": 1,
  "verdict": "Leaking",
  "exitCode": 2,
  "growthThresholdPerMinute": 120.0,
  "requireMonotonicGrowth": true,
  "sampleIntervalSeconds": 2.0,
  "windowSeconds": 120.0,
  "totalObjects": 241033,
  "trackedClasses": 612,
  "censusMillisecondsLast": 3.1,
  "censusMillisecondsWorst": 4.8,
  "worstClass": "HeapCensusDemoObject",
  "worstSlopePerMinute": 1204.0,
  "worstFinding": "HeapCensusDemoObject is growing: ...",
  "nextStep": "obj refs name=<ClassName> - HeapCensus counts and measures; it does not say who is holding the object.",
  "garbageCollection": {
    "runs": 58, "lastMilliseconds": 18.4, "worstMilliseconds": 41.2,
    "averageIntervalSeconds": 62.0, "lastObjectsFreed": 12043,
    "lastPurgeMilliseconds": 6.1, "lastPurgeSpannedFrames": true
  },
  "classes": [ { "class": "...", "count": 4812, "slopePerMinute": 1204.0,
                 "monotonic": true, "verdict": "Leaking", "note": "" } ]
}
```

The values above are shape, not measurements. Diffing two reports from two builds is the cheapest regression
test for growth there is.

---

## 16. And now? — `obj refs` as the next step

HeapCensus stops one question short of the answer, and it says so on purpose.

It tells you **what** is growing and **how fast**. It does not tell you **who** is holding it. That is a
different problem — it needs a reference graph walk, and the engine already ships a good one:

```
obj refs name=HeapCensusDemoObject
```

That prints the reference chain keeping an instance alive, from the root down. Run it while the class is at
the top of HeapCensus's growth table and you go from *"something is leaking"* to *"this UPROPERTY on this
actor is holding them"* in one command.

The usual workflow:

1. **HeapCensus** names the class and proves it is growing rather than merely large.
2. **`obj refs name=<Class>`** names the reference chain that is keeping instances alive.
3. **`gc.` console variables** and Memory Insights if what you have turns out to be a collection-tuning
   problem rather than a leak.

A tool that pretended to do all three would do the first one worse.

---

## 17. Tests

Eight automation tests under `HeapCensus.*`, over the arithmetic rather than over a running game — so they run
headless on a build server in under a second. Session Frontend → Automation, or:

```
UnrealEditor-Cmd.exe MyProject -ExecCmds="Automation RunTests HeapCensus" -unattended -nopause -testexit="Automation Test Queue Empty"
```

| Test | What it pins down |
| --- | --- |
| `SlopeIsExactlyZeroOnAFlatSeries` | A flat series is exactly 0, not almost 0. A symmetric spike is still 0. |
| `SlopeOnARisingSeriesIsObjectsPerMinute` | +10 per 2 s sample is +300/min. The same series at 1 s is +600/min. Falling series go negative. |
| `RankByGrowthSortsBySlopeNotByCountAndIsStableOnTies` | The small fast class outranks the huge flat one — and the same table sorted by count gives the opposite answer. Ties are total. |
| `EvaluateGrowthHitsTheThresholdExactly` | 120 is Leaking, 119.99 is Warn, 60 is Warn, 59.99 is Ok. Exit codes 0/1/2. |
| `IgnoredClassesNeverAppear` | Exact, case-insensitive and prefix matching — and `Package` does **not** swallow `PackageMapClient`. A blank row matches nothing. |
| `TooShortAWindowSaysSoInsteadOfInventingASlope` | Two samples produce no slope and no verdict; the note says `too little data (2 of 3 samples)`. |
| `AClassThatFellBackIsAWaveNotALeak` | A plateau is still monotonic; one dip is not. The same slope is Leaking or Warn depending on it. |
| `FormattingIsStableAndNeverEmpty` | Locale-free grouping, always-signed slopes, and never an empty sentence. |

---

## 18. Troubleshooting

**The box says "settling" and never fills.** Settle Seconds is high, or something re-initialises a game world
repeatedly. The window is thrown away every time one arrives.

**Every class shows `too little data`.** Fewer than three censuses have been taken since the last map change.
At the default interval that is six seconds.

**A class I know is leaking never appears in the table.** The table is at its cap. The box says so — *n
class(es) counted but not listed*. Raise **Max Tracked Classes**; in an editor process you want 8192. See
[section 5](#5-the-demo-map).

**Nothing is ever red.** Either nothing is leaking (likely) or the threshold is too high for your project.
Verify with `Heap.Leak 300`; if the demo class goes red, the plugin is working and your threshold is simply
above your project's real growth.

**A class I know is churning shows a positive slope but only Warn.** It fell back at some point inside the
window, so it is a wave. That is the intended answer. Turn off Require Monotonic Growth to see every climb.

**`GC not seen yet` in the header.** No collection has happened since the plugin started. In the editor that
can take a minute; `Heap.Release` forces one.

**The census cost looks high.** It is proportional to your live object count, and it is honest. An editor
process is the worst case — a packaged game is a fraction of it. Raise the sample interval, or set it to 0 and
sample on demand.

**Nothing appears in a packaged build.** Check that your GameMode's HUD class is `AHeapCensusHUD`, or that
your own HUD calls `DrawCounterBox`. The box is Canvas-drawn precisely so that it *does* work in a packaged
Shipping build.

**A demonstration is still running after I stopped play.** The census is an **engine** subsystem, so
`Heap.Leak` survives Stop-PIE. `Heap.Release` clears it; the demo HUD does that automatically on BeginPlay and
EndPlay.

---

## 19. Version history

| Version | Engine | Notes |
| --- | --- | --- |
| 1.0.0 | 5.8 | First release. Census, growth slope, GC timing, counter box, gate, JSON report, demo map, eight tests. |

---

© 2026 Silvan Teufel. All Rights Reserved.
Support: <mailto:teufelsilvan@gmail.com>
