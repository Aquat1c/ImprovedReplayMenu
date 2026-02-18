# How Improved Replay Menu works

A tour of what the mod does inside the game, for curious users and modders.
No memory addresses - where the code needs them, the pointers below lead to the
files that own them. For install and everyday use, see the
[README](../README.md).

Code references look like `PrepareReplayPath()`
(`src/replay/hooks/replay_flow.cpp`) - function or type first, then the file
that owns it. Line numbers move; names do not.

## The short version

The stock replay screen shows fixed `dataX-Y.rep` slots. This mod keeps that
screen running but feeds it from its own file browser, draws the names on top,
and tricks the game into opening whichever file you picked. Playback itself
stays the game's and Revival's. On supported Revival builds a second part adds
rewind by restoring Revival's own checkpoints and re-simulating the frames in
between with rendering and sound off.

## Repository map

| Area | What lives there |
| --- | --- |
| `src/replay/core/` | File browser, `.rep` parsing, name formats, settings, UI strings, safe patching |
| `src/replay/hooks/menu_hooks.cpp` | Hook installation, the replay screen's render and update, the file-open redirect, the frame counter |
| `src/replay/hooks/replay_flow.cpp` | Menu behaviour: input, Options pages, row motion, launching, recording, folder playback |
| `src/replay/hooks/rewind.cpp` | Rewind, fast forward, round jumps, takeover return |
| `src/replay/render/` | The overlay text layer, the software text fallback, surface locking |
| `include/compat/` | The overlay reset cooperation API shared with other mods |
| `src/crash_handler.cpp`, `src/logger.cpp` | Crash dumps and the log |
| `third_party/` | Dear ImGui and MinHook, compiled into the DLL |
| `assets/fonts/` | The fonts embedded through `src/resources/replay_resources.rc` |
| `tests/` | Browser, replay length and key capture tests |

## Starting up without getting in the way

Every mod's startup thread begins at the same moment. In-game Netplay patches
Revival by stopping all other threads and gives up for good if one cannot be
stopped, so `WaitOutOtherModsStartup()` (`src/dllmain.cpp`) sleeps until the
game's main loop is running (up to ten seconds) plus one more second, before
any logging or hooks start. That extra second is a margin rather than a signal:
no mod announces when it has finished starting.

`InstallHooks()` (`src/replay/hooks/menu_hooks.cpp`) then makes only plain byte
writes through `ApplyPatch()` (`src/replay/core/patch_utils.cpp`): the replay
screen's render and update vtable slots, the battle screen's update slot, and
one call inside the replay renderer. Failure is handled per site. Losing render
or update undoes everything; losing the background call only leaves a static
background; losing the battle slot only stops folder playback after one replay.

The hooks that freeze threads wait for the replay screen. On its first frame
`HookedUpdateReplayScreenLogicImplInner()` (`src/replay/hooks/menu_hooks.cpp`)
installs the present, rewind, file-open and EndScene hooks from the game thread.

Two helpers keep patches safe next to other mods:

- `Install()` (`include/replay/core/hook_install.h`) rebuilds a detour's
  trampoline and retries if another mod changes the same entry mid-install.
- `WriteProcessMemoryLocal()` (`src/replay/core/patch_utils.cpp`) never restores
  a page's old protection, since that could fault another mod writing to the
  same page. If another mod re-protects the page mid-write, it unprotects and
  retries. `ApplyPatch()` also refuses a site whose bytes are not what it
  expected.

## Reading replay files

All replays start with the same header, so `ReplayFormat`
(`include/replay/core/replay_file_metadata.h`) is worked out from the end of the
file by `MeasureReplayLength()` (`src/replay/core/replay_file_metadata.cpp`):

| Format | Written by | Length |
| --- | --- | --- |
| `NativeTape` | Vanilla or very old Revival | Estimated |
| `EmptyExtension` | Old Revival: a footer and an extension saving no inputs | Estimated |
| `RevivalPairs` | Revival, saving both players' inputs | Exact |

A Revival file ends in a trailer whose fields must agree exactly with the file
size. If they do and it holds inputs, the length is half the input count. If
not, the length is estimated by counting the HP marker words each non-paused
frame stores. An empty extension also has a 480 or 240-byte footer that must be
left out of that count, because it contains the same marker values.

`ParseReplayHeaderMetadata()` reads the character, stage and palette words, and
refuses a file holding only a header, since there is no match in it.
`IsLikelyValidCharacterId()` accepts ids 0 to 24. The ids in a replay header are
not the id order of the character objects. `kCharacterNames`
(`src/replay/core/replay_name_format.cpp`) holds the mapping, which was measured
from named replays.

Names come in three shapes, handled by `TryParseConcreteReplayName()` and
`TryParsePlainReplayName()`:

- `YYYYMMDD-HHMMSS P1 (Char) vs P2 (Char)`, or `YYYYMMDD-HHMM` from older
  netplay builds
- `P1 vs P2`, with an optional ` (n)` counter
- The game's own `data1-1` names, which hold no players

`LocalDateKey()` (`src/replay/core/replay_browser.cpp`) falls back to the file's
modified time when the name has no date.

## The file browser

Folders with thousands of replays stay quick because nothing opens a file until
it has to. `ReplayBrowser::ScanFolder()` (`src/replay/core/replay_browser.cpp`)
takes sizes and dates from the directory listing and the rest from the name.
`ReplayBrowser::EnsureVisibleReplayMetadataLoaded()` then reads headers only for
rows on screen, once each. A row therefore isn't known to be playable until it
has been shown, and `PrepareReplayPath()` checks a file again before launching.

`ReplayBrowser::SortEntries()` applies `ReplaySortOrder`, with unknown dates and
unnamed players last. `ReplayBrowser::ApplyFilter()` keeps the full list aside
while a filter is on. One character matches either side, two match that exact
matchup either way round, and player names match exactly. A character filter
reads a header only when the file name gave no characters, on the game thread,
so a very large folder of such files can pause the first time it is filtered.

## The replay screen

`HookedRenderReplayScreenImplInner()` (`src/replay/hooks/menu_hooks.cpp`) wraps
vanilla's renderer. For the length of the call it hides vanilla's character
labels, forces the file-list state and writes the mod's row motion into
vanilla's own animation values, then puts everything back. Vanilla still blits
the bars. `ApplyReplayUiRowLaneTrim()` edits the loaded sprite sheet to hide
unused rows and vanilla's old option bars. `HookedReplayBackgroundBlit()` draws
the background as two wrapped slices for the scrolling effect.

Each update runs input in a fixed order, in
`HookedUpdateReplayScreenLogicImplInner()`: the Esc check, the game's input
poll, `TickMenuMotion()`, then `OnSub76F060HandleListInput()`
(`src/replay/hooks/replay_flow.cpp`) for player 1, and for player 2 only if
player 1 did nothing. The D button uses its own rising edge with a cooldown, so
two players sharing a key or a pulsing input layer cannot toggle the panel
twice.

### Row motion

`replay::motion` (`include/replay/core/menu_motion.h`) uses vanilla's confirm
animation, a cosine over an angle from 0 to 180:

- `RowDrop()` moves the unselected rows down off the screen.
- `SelectedRise()` moves the selected row up. Vanilla stops it at the top row,
  above a short panel. This mod's panel is taller and narrower than a row, so
  the row carries on off the screen.
- `PanelOffset()` brings the Options panel up from below.

`TickMenuMotion()` runs the angle at 6 degrees a tick, a third of vanilla's 1.5
seconds. Opening a folder swaps the list at 180, while no rows are on screen, so
the load is hidden, and then the motion reverses. List input is off while it
runs. Page turns are instant, as they are in vanilla.

### Options and settings

`OptionsItem` (`include/replay/hooks/replay_flow.h`) lists the panel's rows, with
Rewind keys last so it can drop off when rewind is unavailable. `FilterPageRow`
lists the Sort/Filter page. Settings go through `IniPath()` and `Load()`
(`src/replay/core/settings.cpp`), which replaces invalid or duplicate rewind keys
with the defaults. `StepKeyCapture()` (`include/replay/core/key_capture.h`) waits
for all keys to be released and takes the next press. `Tr()`
(`src/replay/core/i18n.cpp`) picks English or Japanese.

## Opening the file you picked

The game can only open `replay\dataX-Y.rep`, so every launch asks for one fixed
slot and swaps the path underneath:

1. `PrepareReplayPath()` (`src/replay/hooks/replay_flow.cpp`) checks the file and
   sets the game's slot to `kLaunchReplaySlot`. It avoids `data1-1`, which the
   ReplayOpener mod intercepts by exact name.
2. `HookedOpenFileReaderImplInner()` (`src/replay/hooks/menu_hooks.cpp`) arms a
   thread-local redirect for that one call, and refuses rather than playing the
   vanilla slot file if the CreateFile hooks are missing.
3. `HookedCreateFileW()` and `HookedCreateFileA()` open your file by its UTF-16
   path, so Japanese names work.

Revival creates the vanilla file when its first read probe fails.
`IsTruncatingOpen()` refuses such opens during a redirect, since otherwise they
would wipe either your replay or the real slot file.

`StartFolderPlayback()` queues the shown replays, `ContinueFolderPlayback()`
checks each in its turn, and `OnReplayBattleUpdateResult()` chains them by
rewriting the battle's next screen to the loading screen.
`TryAllocateRecordSlot()` picks a slot for recording. It reuses missing or
too-small files and those with invalid characters, keeps files it cannot parse,
and overwrites `data1-1` only when nothing else qualifies.

## Drawing text

There are two text paths. `IsAvailable()` (`src/replay/render/d3d9_overlay.cpp`)
picks one every frame.

**The overlay**, used normally. The menu submits `TextItem`s
(`include/replay/render/d3d9_overlay.h`) through a Begin, Submit, Commit protocol
and `HookedEndScene()` draws them with a private ImGui context, selected by
`EfzScopedImGuiContext` (`include/compat/overlay_context_scope.h`). It draws only
into the game's 640x480 target, which skips display-mod passes. It holds no
device objects outside the replay menu. `AddFontSize()` bakes each font size
from the embedded font and merges Japanese glyphs from a system font.

`ReadPaletteFade()` and `SubmitFaded()` (`src/replay/hooks/menu_hooks.cpp`) read
the game's fades back from the live palette, since the overlay sits outside the
8-bit picture. `TextScrollOffset()` scrolls text too wide for its lane, except
unselected list rows, which `DrawItems()` cuts instead.

**The software fallback**, used if EndScene stops firing, for example because
another mod unhooked it. `DrawTextCentered5x7()`
(`src/replay/render/software_font.cpp`) writes palette bytes into the surface
locked by `AcquireMenuDrawSurfaceLock()` (`src/replay/render/draw_surface.cpp`).
Text too wide for the lane is shortened there rather than scrolled.

### Device resets

A plain D3D9 device cannot reset while overlay objects exist, and mods reset it
in different ways. `HookedReset()` releases this mod's objects and always calls
through, never refusing or delaying. Mods that want to coordinate can use
`EFZ_QueryOverlayResetApiV1()`, which returns `EfzOverlayResetApiV1`
(`include/compat/overlay_reset_api.h`). `EfzOverlayResetGate`
(`include/compat/overlay_reset_gate.h`) serializes it with a try-acquire, because
D3D9's Reset can re-enter through window messages.

## Rewind

### Refusing what it doesn't recognise

`kProfiles` (`src/replay/hooks/rewind.cpp`) describes eight EfzRevival builds.
`Install()` identifies one from the DLL's timestamp, then checks its size,
vtable entries and function signatures with `MatchSig()`, and refuses on any
mismatch. That includes a function another mod has already detoured. It tries
once per process. After a refusal, `IsRewindAvailable()` is false and Revival's
keys stay stock.

`TickMain()` passes straight through unless the session is Revival's replay
session and `IsReplayPlayback()` holds, which is why Practice, netplay and
recording never reach this code.

### Seeking

Revival already saves a checkpoint every 64 frames in a ring.
`PickSlot()` uses that ring read-only, and `SlotUsable()` checks a slot from its
own contents, so no separate bookkeeping can go stale. `SeekCore()` loads the
nearest usable checkpoint through Revival's own load and re-simulates up to the
target. If no slot qualifies it returns false with nothing touched.

`EnterHidden()` makes those ticks invisible and harmless. It swaps in a keyboard
device reporting nothing, blanks the game-key bindings so a held pad button
can't start takeover, and turns off rendering. `HookedPlay()` drops sound.
`HiddenStop()` ends the loop on a fade, takeover or pending exit. The end-of-file
flag is left for a visible tick to consume, because folder chaining has to see
it. `Seek()` always finishes with exactly one visible tick.

`SeekRefusal()` and `TrySeekBack()` produce the on-screen `F8 ON` and `NO CKPT`
tags.

### Holding keys

`HoldSpeed()` doubles every half second up to 8x. Rewinding seeks that many
frames per tick, halving the step if a seek is refused. `FastForwardBatch()` runs
hidden ticks plus one visible tick, for 2x to 16x, since visible ticks cannot
beat vsync.

### Rounds

`ObserveRound()` records a round start when the intro byte falls to 0.
`PrevRoundTarget()` returns the start of the current round, or the round before
within 60 frames of that start, so repeated presses can leave a round.
`NextRound()` jumps to a known later start. Otherwise it scans ahead with hidden
ticks, spread over main-loop iterations by `ScanIteration()` so the window stays
responsive. `ScanStop()` ends the scan early enough to keep the checkpoint it
started from, and any result but a landing returns there.

### Takeover

Stock Revival can only leave takeover by loading a save, and its load clears
takeover even from an empty slot. That feeds the recording into a match that has
already changed, which is the desync. `ShouldDropRevivalKey()` drops Revival's
ring keys while in takeover and `TakeoverStep()` does the return itself: it seeks
back to the frame where the recording left off, and the replay plays on.

It runs on the rewind key, on Revival's ring keys, after a KO has cleared or run
for 90 frames, when the recording runs out of inputs, or when the match ends. A
failed seek shows its tag and stays in takeover.

### When something goes wrong

`GuardedMain()` catches a fault during a replay tick, so a dump is still written,
and rewind turns itself off. `RestoreAfterFault()` undoes hidden mode and key
masking, and if a seek was mid-flight it leaves the half-restored match by the
stock exit. Everything outside replay playback runs outside this guard, so a
crash on another screen behaves as it would without the mod.

## Crash safety and logging

`VectoredExceptionHandlerProc()` (`src/crash_handler.cpp`) logs serious faults
and never swallows them. `BuildDumpPath()` keeps one dump beside the DLL,
overwritten each time. In RelWithDebInfo it also dumps the first fault even if
something downstream catches it.

`kEnableFileLogging` (`src/logger.cpp`) is on only in RelWithDebInfo. `LogVerbose`
(`include/logger.h`) compiles out unless `EFZ_REPLAY_LOG_VERBOSE=1` is defined,
because flushing per-frame lines hitches the menu.

## Building and testing

```powershell
cmake -S . -B build -A Win32
cmake --build build --config RelWithDebInfo
ctest --test-dir build -C RelWithDebInfo
```

The tests in `tests/` cover what can run without the game: folder navigation
with Unicode paths, sorting and filtering, name parsing and the motion maths in
`replay_browser_tests`, length and format detection in `replay_length_tests`, and
tap, hold and rebinding in `key_capture_tests`. Hooks and rendering need the game
and have no automated tests.
