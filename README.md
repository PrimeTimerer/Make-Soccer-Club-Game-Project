# Make Soccer Club Game Project

This is a native PC port of **サカつく プロサッカークラブをつくろう！** (PlayStation 3, SEGA, BLJM61064, 1.03 update).
- The game's PPU code is statically recompiled to C++ with [ps3recomp](https://github.com/sp00nznet/ps3recomp).
- RSX graphics are drawn with Direct3D 12.

> **The game is not included.** You need files from your own disc (BLJM61064) and its 1.03 update.
> All rights to the game and to the data and code in it belong to **SEGA**. This repository is a non-profit fan project unrelated to SEGA, and it contains no game data.
> - The recompiled game code (`recompiled/`, `prx_src/`) and the SPU programs (`spu_jobs/*.elf`) are built locally from each user's own game files.
> - The `patches/recompiled-*.patch` files contain a few lines of recompiled code at the places where hooks are inserted.

## Features

- **60 fps at the original game speed:**
  - The match calculation stays at 30 Hz.
  - Player and ball motion are smoothed between match ticks, including the ball's jumps.
- **Launcher (`Sakatsuku Launcher.exe`):**
  - Settings, saves and backups, and cheats.
  - Player and list editors. They read the running game or a save slot you choose.
  - Korean patch.
- **In game (F2, or L3+R3 on a pad):** the cheat menu, with cheats, players and lists. F7 is double speed and F8 is turbo.
- **Korean patch:** the original files stay untouched; the game reads the Korean files from `game\mods\korean` instead. To turn it on, go to launcher settings → Korean patch, and install it from the patch exe.

## Folders

| Folder | Contents |
| --- | --- |
| `game/` (not in the repository) | `EBOOT.ELF` (1.03), `dev_bdvd/` (disc), `dev_hdd0/` (1.03 update), `firmware/` |
| `gamedata/` (not in the repository) | Saves and backups |
| `patches/` | Patches to the `ps3recomp` submodule (`git -C ps3recomp apply ../patches/00NN-*.patch`, in number order) and patches to the recompiled code (`recompiled-*`) |
| `tools/launcher/` | The launcher (C#/WPF, .NET Framework 4) |
| `NOTES.md` | The full record of how the port was made and what was analyzed |

## Build (Windows)

1. Run `git clone --recursive`, then apply the `patches/0001`, `0002`, … patches to `ps3recomp` in order.
2. Build `recompiled/`, `prx_src/` and `spu_jobs/` from your own game files, then run `build_release.bat`.
   - Requires Visual Studio 2022 with clang-cl, Ninja, and Python in `.venv`.
   - The conversion process is recorded in `NOTES.md`.
3. Run `build_launcher.bat`. It only needs the .NET Framework 4 that comes with Windows.
