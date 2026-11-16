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
- **In game (F2, or L3+R3 on a pad):** the cheat menu, with cheats, players, lists, training slots, staff and team colors. F6 forces a skill play of your team in a match, F7 is double speed and F8 is turbo.
- **Korean patch:** the original files stay untouched; the game reads the Korean files from `game\mods\korean` instead. To turn it on, go to launcher settings → Korean patch, and install it from the patch exe.

## Folders

| Folder | Contents |
| --- | --- |
| `game/` (not in the repository) | `EBOOT.ELF` (1.03), `dev_bdvd/` (disc), `dev_hdd0/` (1.03 update), `firmware/` |
| `gamedata/` (not in the repository) | Saves and backups |
| `patches/` | Patches to the `ps3recomp` submodule (`git -C ps3recomp apply ../patches/00NN-*.patch`, in number order, on the pinned v0.18.0 commit) and patches to the recompiled code (`recompiled-*`) |
| `tools/launcher/` | The launcher (C#/WPF, .NET Framework 4) |
| `NOTES.md` | The full record of how the port was made and what was analyzed |

## Download

Ready-to-run builds are on the [Releases](https://github.com/PrimeTimerer/Make-Soccer-Club-Game-Project/releases) page: `Sakatsuku Launcher.exe` and `build-release\SakatsukuRecomp.exe` in one zip. The zip contains no game data.

**What you need:** your own disc (BLJM61064, as a disc folder or a decrypted ISO), the 1.03 update PKG, the PS3 firmware (`PS3UPDAT.PUP`, from PlayStation's PS3 system software update page), and [RPCS3](https://rpcs3.net/download). RPCS3 decrypts the update's EBOOT and two firmware modules. The port never ships or uses Sony keys.

1. In RPCS3, install the firmware and the 1.03 update, and add the disc to its game list. The launcher's **게임 파일** page also has buttons for these steps.
2. Unzip the release anywhere, run `Sakatsuku Launcher.exe` and open **게임 파일**. The launcher finds RPCS3 by itself; if it does not, pick its folder. Then press **자동 준비**. The launcher:
   - runs RPCS3's `--decrypt` on copies of the files;
   - checks every result against the files the port was built from (SHA-256);
   - relocates the firmware modules;
   - copies the 1.03 update into `game\dev_hdd0`;
   - uses the disc where it is.
   RPCS3's own folders are not changed.
3. Press **플레이** on the home page.

Without RPCS3, already decrypted files (`EBOOT.ELF`, `libsre.prx`, `libspurs_jq.prx`) can be picked by hand on the same page.

**Updates:** the launcher checks GitHub once a day. When a newer release exists, it downloads it in the background without asking and applies it the next time the launcher opens, before any window appears. A switch in **도구 → 포트 업데이트** turns this off; the same card can also install at once. An update replaces only the app files. `game\`, `gamedata\` (saves), `launcher.ini` and your cheat settings stay as they are, and the previous game exe is kept as `build-release\SakatsukuRecomp.prev.exe`.

## Build (Windows)

1. Run `git clone --recursive`, then apply the `patches/0001`, `0002`, … patches to `ps3recomp` in order. There is no 0020: the cheat engine is `saka_hooks.c` in this repository.
2. Build `recompiled/`, `prx_src/` and `spu_jobs/` from your own game files, then configure and build `build-release/` with CMake (Release, Ninja, clang-cl). `PORT_SPU_INPUTS` lists `game/EBOOT.ELF`, `game/firmware/libsre.prx`, `game/firmware/libspurs_jq.prx` and the `spu_jobs/cri_*.elf` files.
   - Requires Visual Studio 2022 with clang-cl, Ninja, and Python in `.venv`.
   - The conversion process is recorded in `NOTES.md`.
3. Build `Sakatsuku Launcher.exe` from `tools/launcher/*.cs` with the C# compiler of the .NET Framework 4 that comes with Windows (`csc.exe /target:winexe`, WPF, `Theme.xaml` as a resource, `icon.ico` as the icon).
4. `tools/make_release.ps1` makes the release zip.

## License

- The port's own code and files are under the [MIT License](LICENSE). Anyone may use, change and share them, in mods too, as long as the copyright notice stays.
- `third_party/imgui` (Dear ImGui) and the `ps3recomp` submodule keep their own MIT licenses.
- `cheats/BLJM61064.txt` is converted from a VitaCheat list for the PS Vita version (`PCSG00198.psv`). The original codes are the work of their authors.
- The license does not cover the game or anything taken from it; those belong to SEGA. This includes the recompiled lines in `patches/recompiled-*.patch` and the recompiled game code inside the release exe.
