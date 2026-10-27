# Moon Sweepers ChunkLoader

A mod for [Moon Sweepers](https://store.steampowered.com/app/5335510/) that keeps your whole base running while you are far away, and spreads dust capsules evenly across all Capsule Catchers.

## Features

- **Far-away machines keep working.** The game only fully simulates machines, conveyor belts and docks within about 425 m of a player. Robots (MK robots, J.O.O.N.A.S.) further away fall back to a coarse simulation at 50 % efficiency. The mod places invisible anchors at every group of machines and belts and at every active robot, so the game treats those areas as if a player were standing there.
- **Fair capsule distribution.** The game sends every launched capsule to the nearest Capsule Catcher. Each catcher only takes one capsule every 9 seconds, so the nearest one queues up while the others sit idle. With the mod, each capsule goes to the catcher that can catch it first (flight time + queue). A filling store counts as extra waiting time. Full catchers are skipped, just like in the base game.

## Installation

1. Download the latest release and unzip it.
2. Close the game.
3. Copy `version.dll` and `ChunkLoader.ini` into the game folder:
   `C:\Program Files (x86)\Steam\steamapps\common\Moon Sweepers`
   (Steam: right-click the game → Manage → Browse local files)
4. Start the game. A `ChunkLoader.log` file appears in the same folder. It should say `Hook ... installiert`.

Press **F8** in game to turn the anchors on or off.

**Uninstall:** delete `version.dll` (and `ChunkLoader.ini` / `ChunkLoader.log`).

### Steam Deck / Linux (Proton)

Proton uses its own built-in `version.dll` and ignores the one in the game folder unless you tell it otherwise.

1. Switch to Desktop Mode.
2. In Steam, right-click Moon Sweepers → Manage → Browse local files. The game folder is usually
   `~/.local/share/Steam/steamapps/common/Moon Sweepers`
   (on an SD card: `/run/media/<card>/steamapps/common/Moon Sweepers`).
3. Copy `version.dll` and `ChunkLoader.ini` into that folder.
4. Right-click Moon Sweepers → Properties → General → Launch Options and enter:
   ```
   WINEDLLOVERRIDES="version=n,b" %command%
   ```
5. Start the game (Game Mode is fine). `ChunkLoader.log` should now appear in the game folder.

**Is it running?** There is no in-game overlay. Open `ChunkLoader.log` (the messages are in German):

| Log line | Meaning |
|---|---|
| `Hook auf MoonWorld.Update installiert ... Bereit.` | Mod loaded |
| `Hook auf MoonWorld.FindCatcher installiert` | Capsule balancing active |
| `X Anker aktiv (aus Y Gebaeuden, Z Baendern) + N Roboter.` | Anchors placed, base kept loaded |

No log file at all means the DLL was not loaded. Check the launch option.

The Deck has no F8 key. Either map F8 to a button in Steam Input or just leave `Enabled=1` in `ChunkLoader.ini`.

Back up your save before first use:
`%USERPROFILE%\AppData\LocalLow\DragDropGames\MoonSweepers`

## Settings (`ChunkLoader.ini`)

| Key | Default | Meaning |
|---|---|---|
| `Enabled` | `1` | Anchors on/off (also toggled with the toggle key) |
| `CellSize` | `300` | Machines closer than this many metres share one anchor |
| `MaxAnchors` | `64` | Maximum number of anchors (more = more loaded, more CPU) |
| `IncludeBelts` | `1` | Also anchor conveyor belts |
| `FollowRobots` | `1` | Keep the area around every active robot loaded |
| `RefreshSeconds` | `5` | How often anchors are recalculated |
| `ToggleKey` | `119` | Virtual key code of the toggle key (119 = F8) |
| `BalanceCapsules` | `1` | Spread capsules across all Capsule Catchers |
| `FillPenaltySeconds` | `60` | Extra wait a completely full catcher store counts as (0 = queue only) |

If the game stutters, lower `MaxAnchors` or set `FollowRobots=0`.

## Notes

- Made for single player or the host of a multiplayer game.
- Tested with the October 2026 Steam build (Unity 6000.6.0f1, IL2CPP).
- After a game update the mod may stop working. It then does nothing and writes the reason to `ChunkLoader.log`.
- Unofficial fan mod, not affiliated with the developers of Moon Sweepers.

## How it works

The mod is a `version.dll` proxy. Windows loads it from the game folder, and it forwards all real `version.dll` functions to `C:\Windows\System32\version.dll`. It does nothing in any other program (Unity's crash handler also loads `version.dll`).

A background thread waits for `GameAssembly.dll`, then uses the public IL2CPP C API to look up the game's classes, fields and methods by name. It installs two small inline hooks:

- `MoonWorld.Update`: every few seconds, clusters all machines, belt ends and robot positions into cells. It then moves one invisible `GameObject` to each cell and appends them to `MoonWorld.observers`. Robots are never added as observers themselves, because the game treats observers as obstacles and the robots would keep dodging their own collider.
- `MoonWorld.FindCatcher`: picks the Capsule Catcher with the earliest catch time instead of the nearest one, using the game's own `CatcherFull`, `CatcherMouth`, `CatchFreeAt`, `CatcherKilograms` and `MoonLaunchCapsule.FlightSeconds`.

All game calls happen on the main thread inside the hooked functions. Errors are caught and logged with module offsets and a short call stack.

## Building from source

Requires Visual Studio 2022 (or newer) with the "Desktop development with C++" workload.

```bat
build.bat
```

The result is `dist\version.dll`. All source code is in `src\ChunkLoader.cpp`.
