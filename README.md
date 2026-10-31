# Improved Replay Menu

A replay menu replacement for **Eternal Fighter Zero**, with extra playback
controls when running on **EfzRevival**.

The stock menu shows nine slots on each of nine pages. This mod shows your whole
`replay` folder instead: subfolders, player names, dates, sorting and filters,
in English or Japanese. During a replay it adds rewind, fast forward, round
skipping and a frame counter, and it lets you take over a character and get
back to the recording without saving first.

The built DLL is `ImprovedReplayMenu.dll`. How it works inside is covered in
[docs/HOW_IT_WORKS.md](docs/HOW_IT_WORKS.md). `readme.txt` is the short guide
for players, in English and Japanese.

## Installation

1. **Download the mod:** get the `.zip` from
   [GitHub Releases](https://github.com/Aquat1c/ImprovedReplayMenu/releases).

2. **Install EFZ Mod Manager** if you don't have it already:
   [EFZ Mod Manager spreadsheet](https://docs.google.com/spreadsheets/d/1r0nBAaQczj9K4RG5zAVV4uXperDeoSnXaqQBal2-8Us/edit?usp=sharing)

3. **Unzip it into your game's `mods` folder**, so it looks like this:

   ```text
   EFZ/                          <- your game folder
   |-- efz.exe
   |-- EfzModManager.ini         <- mod enable list (edit this)
   `-- mods/
       `-- ImprovedReplayMenu/
           |-- ImprovedReplayMenu.dll
           |-- readme.txt
           `-- system/
               `-- REPLAY.dat    <- the menu's row artwork
   ```

4. **Enable it** in `EfzModManager.ini` under `[Mods]`, above
   `EnglishTranslation` if you use it:

   ```ini
   ImprovedReplayMenu=1
   ```

To uninstall, set `ImprovedReplayMenu=0` or delete the mod folder.

## Controls

### Replay menu

| Input | Action |
| --- | --- |
| Up / Down | Move through the list |
| Left / Right | Change page |
| Confirm | Play a replay or open a folder |
| Cancel | Go to the parent folder, or leave the menu from the top folder |
| D button | Open or close Options |
| C button | Start recording |

### During a replay

| Key | Tap | Hold |
| --- | --- | --- |
| `-` | One frame back | Rewind, getting faster |
| `P` | One frame forward | Fast forward, getting faster |
| `Backspace` | Restart the current round | |
| `\` | Skip to the next round | |
| `H` | Show or hide the frame counter | |

These are defaults and follow your keyboard layout. Change them under
**Options > Rewind keys**. `P` replaces Revival's next-frame key during replay
playback only.

### Takeover

Press a direction or game button during a replay recorded with Revival to take
control of a character. This is Revival's own feature. To go back to the
recording, tap `-` or Revival's replay keys (Num - and Num + by default). It
also comes back by itself shortly after a KO, when the match ends, or when the
recording runs out. No manual save is needed.

## Settings

Options are saved in `ImprovedReplayMenu.ini` beside the DLL.

```ini
[Settings]
Language=auto          ; auto, en or ja
Sort=newest            ; newest, oldest, player or size
BackgroundScroll=on    ; on or off
FrameCounter=on        ; on or off

[Rewind]
Enabled=1              ; 0 turns rewind off and leaves Revival's replay keys stock
KeyBack=0x0C           ; DIK codes, set from Options > Rewind keys
```

`KeyForward`, `KeyPrevRound`, `KeyNextRound` and `KeyToggleCounter` work the same way as `KeyBack`.
Filters last until the game closes and are not saved.

## Compatibility

- The menu works with vanilla EFZ and with Revival. It reads replays recorded
  by vanilla, old Revival and current Revival.
- Rewind, fast forward and round skipping need EfzRevival 1.02e, 1.02f, 1.02g,
  1.02h, 1.02i or 1.02j. On anything else they turn themselves off and the
  Rewind keys option is hidden.
- Rewinding reaches back about 19,000 frames, the length of Revival's own
  checkpoint ring.
- The mod is built to run alongside In-game Netplay, efz-training-mode and
  DisplayManager. Rewind only acts during replay playback, so Practice and
  netplay sessions never reach it.

## Troubleshooting

| Problem | What to try |
| --- | --- |
| A rewind key does nothing | Another mod may use it. Pick another key in Options > Rewind keys. |
| `NO CKPT` beside the frame counter | There is no saved point to go back to from here. |
| `F8 ON` beside the frame counter | Turn off the game's F8 pause first. |
| A player filter hides `data1-1.rep`-style files | Those names hold no player names. Character filters still work on them. |
| Text looks like a small bitmap font | The overlay text layer isn't running, so the menu fell back to software text. |

For a bug report, use a RelWithDebInfo build: it writes `efz_replay_mod.log` and
`efz_replay_mod_crash.dmp` beside the DLL. Release builds write no log.

## Build

You need Visual Studio with the C++ desktop tools and CMake 3.20 or later. The
mod must be built as 32-bit.

```powershell
cmake -S . -B build -A Win32
cmake --build build --config Release
ctest --test-dir build -C Release
```

The DLL is written to `build\bin\<config>\ImprovedReplayMenu.dll`. To install your
own build, put it in the layout shown under [Installation](#installation), with
`REPLAY.dat` copied from `assets/system/`. Without that file, the mod uses the
row artwork from the game's own replay sheet.

## Credits and licenses

- [Dear ImGui](https://github.com/ocornut/imgui) 1.91.9b and
  [MinHook](https://github.com/TsudaKageyu/minhook) 1.3.3 are in `third_party/`
  under their own licenses.
- The font in `assets/fonts/ITC_Bolt_TTF/` comes with `License.txt`. Fonts made
  from [Web Fonts](http://www.onlinewebfonts.com) is licensed by CC BY 4.0.
- Rewind's design takes ideas from Giuroll, and the key rebinding flow comes
  from efz-training-mode.
