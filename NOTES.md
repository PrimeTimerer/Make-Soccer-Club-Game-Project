# Sakatsuku PS3 (BLJM61064, 1.03) — ps3recomp port notes

1.03 runtime contract (boot mode, VFS, DLC = not used):
`docs\PORT_1.03_RUNTIME.md` (moved from `..\Fight Emulations\Saka Tsuku - Pro Soccer Club o Tsukurou (Japan)\PC PORT` on 2026-11-09).
Folder tidy-up of 2026-11-09:
- `docs\` holds the runtime contract and the merge manifest.
- `tools\ps3\` holds the PKG/SELF extraction scripts (git-ignored, Sony keys).
- `originals\` holds the owner's ISO, the update PKG and the Korean patch exe (git-ignored).
- `make_release.bat` makes `dist\`.
- Old logs and builds moved to `Desktop\_사카츠쿠_삭제대기` (`_이동기록.txt` lists where each came from).

## Layout
| Path | What |
|---|---|
| `ps3recomp/` | git submodule, pinned to v0.18.0 (4e8f3ca) |
| `game/` | not in git. `EBOOT.ELF` (1.03, sha256 a5144796…), `dev_bdvd/` (original disc), `dev_hdd0/` (1.03 patch) |
| `out/` | ppu_loader output (functions/imports/image/loader json) |
| `recompiled/` | lifted C++ (105 chunks, `--chunk-lines 100000`) |
| `CMakeLists.txt`, `main.cpp`, `stubs.cpp` | from `ps3recomp/templates/project`. Project name SakatsukuRecomp; PS3RECOMP_DIR defaults to `./ps3recomp` |
| `build.bat`, `run.bat` | build (clang-cl, Ninja, no build type → template -O1, `-j 24`); run with the 1.03 VFS |

## Commands and timings (32 cores, 64 GB, 2026-11-06)
```
.venv\Scripts\python -X utf8 ps3recomp\tools\ppu_loader.py game\EBOOT.ELF -o out\                     (2 s)
.venv\Scripts\python -X utf8 ps3recomp\tools\ppu_lifter.py game\EBOOT.ELF --functions out\EBOOT.functions.json ^
    --hle-stubs out\EBOOT.imports.json --output recompiled\ --chunk-lines 100000                       (~2 min)
build.bat                                                                                              (full build ~1 min)
run.bat   (env: PS3_VFS_ROOT=game\dev_bdvd, PS3_HDD0_ROOT=game\dev_hdd0)
```
- Always run Python with `-X utf8` or `PYTHONUTF8=1`. The cp949 default breaks the lifter.
- Quiet test: Start-Process -WindowStyle Hidden kept the D3D12 window hidden. Use `PS3_MUTE=1`, `PS3_FSLOG=1`, and `LD_FRAME_DUMP=<dir>` with `LD_FRAME_DUMP_EVERY=N`. Stop the exe afterwards.

## Lift facts
- Functions: 12,433 OPD entries, 31,859 lifted. 332 firmware imports in 28 libraries.
- Entry: OPD 0x9D85C0, code 0x690640, TOC 0x9E1848.
- 20 "no continuation … emitting halt" warnings, mostly at the end of the text segment (0x9BA7E8).

## Run 1 (60 s, hidden, muted)
Boot reaches the following, then stops:
- sysmodule loads, SPURS attribute
- pad (XInput)
- `pxd_lib initialize()`
- GCM init and the D3D12 backend (AMD Radeon 8060S, 1280x720)
- video out, tiles
- net/https/ssl/http and sceNp init (offline)
- `cellGame BootCheck` → `ContentPermit` (/dev_bdvd)

0 frames were dumped.

## Run 2–4 (2026-11-06): LLE SPURS → title screen
- **LLE:** libsre and libspurs_jq from the owner's FW 4.93 (`game/firmware/*.sprx`) were decrypted with `tools\ps3\self_decrypt.py` (was `..\PC PORT\tools\`). They were relocated with `lift_prx.py` (libsre 0x30000000, libspurs_jq 0x30100000) and lifted with `--raw --symbol-prefix <m>_ --hle-stubs` (stubs from `tools/prx_hle_stubs.py`) into `prx_src/<m>/`. `stubs.cpp ps3_load_prx_modules()` loads `game/firmware/<m>_image.bin` through `prx_loader`, and game imports then resolve via `prx_resolve_export`.
- `PORT_SPU_INPUTS` = EBOOT plus both PRX files, giving 6 lifted SPU images. The SPURS kernel runs on 6 SPUs.
- The game reads its text tables, `sb2/bg_top_menu.par`, `stream/bgm_title.adx` and `sb/top_menu.par`. The **title screen renders** and animates ("PRESS START BUTTON").
- It draws into a 1280x720 surface at 0x00F00000 while the registered display buffers are 0x0 and 0x3C0000. **`RT_DISPLAY_BY_SIZE=1` is required** (set in run.bat and quiet_run.ps1).
- Frame capture: `tools\quiet_run.ps1 -Tag X -Seconds 90 -Cap "1000:4:300"` writes `logs\cap_X\frame_NN\backbuffer.bmp`. LD_FRAME_DUMP does not work on D3D12.
- CMake default build type is **Debug** (ps3recomp default), so lifted code runs at -O0 and the D3D12 debug layer is on. That gives about 16–20 fps and a 1-minute full build. An optimized build is still to do.
- Still unresolved: sceNpTusCreateTitleCtx (0x7CAF58EE) and sceNpTusDestroyTitleCtx (0x2E162A62).
- cellSaveData uses root `./gamedata/dev_hdd0/home/00000001/savedata` (the cellFs default mapping) instead of PS3_HDD0_ROOT. Fix it before testing saves.

## Run 5 (2026-11-06): 1.03 patch boot
- `stubs.cpp saka_register_patch_boot()` runs in `ps3_load_prx_modules` before `ppu_hle_init`, so its ctx handlers win over the HLE table.
  - It calls `cellGame_init_from_paramsfo(<PS3_HDD0_ROOT>/game/BLJM61064/PARAM.SFO)`, which sets the title id to BLJM61064 (category GD, game type stays DISC).
  - It wraps BootCheck (adds attributes PATCH and RPCS3 size values), adds PatchCheck (CELL_OK, sysSizeKB 0), and wraps ContentPermit: after a PatchCheck it returns `/dev_hdd0/game/BLJM61064[/USRDIR]`. It also wraps DataCheck to clear the pending patch permit.
- The order is now identical to RPCS3 1.03: BootCheck → ContentPermit(bdvd) → PatchCheck → ContentPermit(hdd0) → DataCheck "/", "BLJM61064", "NPJB00414" → 25 edat checks.
- The title renders. The `patch/dlc` archive opens are not reached yet; they need input past PRESS START. RPCS3 opened them after save load and entering the game.

## Owner live check (2026-11-06, run.bat, Debug build)
- The intro passes faster than expected and the game reaches the first menu. It is slow but works.
- **No music.**
- **DualSense with Steam running is not detected.** On Windows cellPad reads only XInput and the keyboard. The SDL2 backend (`PS3RECOMP_PAD_USE_SDL2`, DualSense via HIDAPI) is built only off Windows.
- Keyboard mapping in `libs/input/cellPad.c`: Enter=START, Tab=SELECT, Z=×(CROSS), X=○(CIRCLE, the confirm button in JP games), arrows=D-pad, A/S/Q/W/1/2 = the other buttons. `PAD_NO_KEYBOARD` turns it off.
- Scripted input for hidden runs: `PAD_SCRIPT="t:mask,t:mask,..."` (seconds:mask). Masks: START 0x8, UP 0x10, DOWN 0x40, CIRCLE 0x2000, CROSS 0x4000.
- Audio diagnostics: `AUDIO_PEAK=1` logs the peak of each mixed block, and `AUDIO_WAV=<file>` dumps the final mix as raw f32le stereo 48 kHz.
- The D3D12 debug layer is on in Debug builds or when `D3D12_DBG` is set.
- **Owner's goal: 60 fps with no errors.**

## DualSense via SDL2 (2026-11-06)
- `third_party/SDL2` holds SDL2-devel-2.32.10-VC.zip from libsdl-org/SDL release-2.32.10. Its sha256 af347939…8a6e matches the GitHub digest. The folder is not in git.
- `CMakeLists.txt`: when `third_party/SDL2/lib/x64/SDL2.lib` exists, `ps3recomp_runtime` gets `PS3RECOMP_PAD_USE_SDL2` and `SDL_MAIN_HANDLED`. The flat headers are copied to `build/sdl2_include/SDL2/` (cellPad includes `<SDL2/SDL.h>`), SDL2.lib is linked, and SDL2.dll is copied beside the exe.
- Submodule patch `patches/0001-cellpad-keyboard-fallback-for-any-windows-backend.patch`, already applied in `ps3recomp/` (re-apply with `git -C ps3recomp apply ../patches/0001-*.patch` after a submodule update): it includes windows.h on any Windows build and moves `pad_poll_keyboard` out of the XInput-only block. Upstream never built SDL2 on Windows.
- Run 6 (hidden): `[cellPad] SDL controller connected on port 0: DualSense Wireless Controller` with Steam running. Boot is unchanged. The keyboard fallback is used only while port 0 has no pad.

### Blockers (Phase 1)
1. **SPURS v2 APIs are not in the ps3recomp HLE.** The 8 unresolved NIDs (names from the RPCS3 log) are:
   - cellSpursInitializeWithAttribute2 (0x30AA96C4)
   - cellSpursAttributeEnableSystemWorkload (0x9DCBCB5D)
   - _cellSpursTasksetAttribute2Initialize (0xC2ACDF43)
   - cellSpursCreateTaskset2 (0x4A6465E3)
   - cellSpursCreateTask2WithBinInfo (0xE4944A1C)
   - _cellSpursCreateJobQueueWithJobDescriptorPool (0x983BE7FC)
   - _cellSpursJobQueuePort2CopyPushJobBody (0xC39173A4)
   - cellSpursJobQueuePort2Sync (0xFACB3CED)

   The main thread then polls `timer_usleep` at LR 0x006CD504. RPCS3 runs libsre and libspurs_jq as LLE for this game.
2. **cellGame (1.03 contract).**
   - BootCheck reports titleId `BLES00000` (default; PARAM.SFO not read) and type DISC with no PATCH attribute.
   - `cellGamePatchCheck` is not implemented upstream (only listed in `tools/nid_from_protos.json`).
3. **RSX FIFO:** "bad word" at io 0xE60 followed by a jump-park resync (watch this).

## Frame rate: 30 fps exact, 60 fps investigation (2026-11-06)
- Host clock (main.cpp): vblank ticks from QueryPerformanceCounter at exactly 60 Hz. Default mode measures 30.0 fps (150 flips per 5 s). `SAKA_FPS=60` ticks at 120 Hz, so the game's own interval-2 wait gives 60 fps pacing. `SAKA_VBLANK_HZ` overrides.
- Frame loop (func_0069E4FC):
  1. Build frame N+1.
  2. Vblank wait (0x0069E704) until `count - last >= interval`. `count` is 0x116A928 (the vblank handler 0x0069D4B0 increments it); `last` is 0x116A92C.
  3. Release the commands and wait at lr 0x0069E83C (usleep 250 us) until the RSX has finished frame N.
  4. Flip.
  5. `last = count` (0x0069E8A4).
  At 120 Hz, steps 3–4 must fit in one tick (8.3 ms), otherwise a tick is lost.
- PS3_FTRACE (see `tools/ftrace_report.py`) found the cap:
  - The RSX walker ends ~99.9% of its passes parked on a self-JUMP.
  - That park is patched only by an **SPU DMA PUT** (SPURS SPU 0x2004), 6–22 ms after the release (p50 13 ms).
  - The command-buffer-building job runs in the **SPU interpreter**: the kernel is lifted (image 274060898), but SMC detection hands the job queue PM and job code to the interpreter.
  - Not the cause: Present (0.34 ms), the message pump, GPU wait, RSX fence pacing (only 8 SET_REFERENCE in a run).
- Runtime patch 0002 (`patches/0002-spu-dcache-remember-misses.patch`) makes the dispatch cache remember misses outside resident code slots, and grows it from 512 to 2048 entries. Results:
  - SPU job p50 13.3 → 10.0 ms; 60 fps mode 31.8 → 37.6 fps.
  - 30 fps mode unchanged at 30.0.
  - `SPU_DCACHE_NEG=0` turns it off.
- Patch 0003 is diagnostics only and is idle unless PS3_FTRACE is set:
  - `PS3_FT_DRAIN_END`: why each pass stopped (1 = park, 2 = flip), plus put/get.
  - `PS3_FT_PARK_WRITE`: an SPU PUT over the parked word.
  - main.cpp adds `PS3_FT_PRESENT` and `PS3_FT_PUMP`.
- **Game logic is frame-locked:**
  - Nothing reads the vblank delta for game time.
  - The timebase is read only for the once-a-second FPS statistic (0x0069E5B8).
  - Boot's fixed waits shrink with the frame rate in 60 fps mode (41 frames: 1.37 s at 30 fps, 1.02 s at ~40 fps).
  - So 60 rendered frames per second means **double game speed**. Real 60 fps needs the per-frame time steps patched, or host-side frame interpolation. Waiting on the owner's decision.
- `saka_hooks.c` usleep→FIFO kick is now opt-in (`SAKA_USLEEP_KICK=1`): it made no measurable difference.

## SPU: lifted RSX command-buffer task (2026-11-06)
- **Root cause:** the slow per-frame SPU work is a SPURS **taskset task**, not a job.
  - Its ELF is at EA 0x00976100 in the EBOOT. Text goes to LS 0x3000–0xC310, entry 0x3050, lifted image 344610659.
  - The LLE libsre taskset policy (EA 0x30023680 → LS 0xA00, 0x1E40 bytes, not lifted) DMAs the task text into LS 0x3000 and branches to it.
  - Nothing marked the lifted image resident, so the task ran in the interpreter. `SAKA_PARK_LSDUMP` showed this (local store and the last 64 GETs at the park write).
- **Fix:** `saka_hooks.c: saka_register_spu_task_regions()`. It runs after the ELF load.
  - It checks the in-memory ELF's FNV-1a-64 against the lifted image.
  - It then registers the task text as a verified streamed region (`spu_overlay_register_verified_region`, keyed by the source EA).
- **Default:** only bit 0 (the RSX task). `SAKA_SPU_TASK_REGIONS=0` turns it off; `SAKA_SPU_TASK_MASK` selects tasks.

| Measured (hidden runs) | Interpreted | Lifted |
|---|---|---|
| Title, 30 fps mode: RSX parked per frame | 25.3 ms | 4.4 ms |
| Title, 30 fps mode: worst frame | 69.9 ms | 43.6 ms |
| Post-title "体験版 save data" dialog: parked per frame | 10.2 ms | 0.8 ms |
| Title, `SAKA_FPS=60` | ~35–38 fps | 59.8 fps (game logic still frame-locked, see above) |

  Both screens render the same as before (captures checked); 30.0 fps in both cases on the dialog.
- **Not usable yet:** the other three EBOOT tasks (bits 1–3: ELFs 0x0097F700, 0x00980F80, 0x00982680).
  - Lifting them stalls the boot after 5 file opens (normal is 752).
  - Likely cause: a lifted task's syscall or exit into the still-interpreted policy.
  - Proper fix: lift the taskset policy as a verified region and register its stack-reset PCs, as the Yakuza port does (`tools/prepare_yakuza_macos.py`).
- **Not checked:** menus past the first dialog, gameplay, matches.
- **Launchers:** `run.bat` and `run_logged.bat` now start `build-release\` when it exists, otherwise the Debug `build\`. Backups are in `logs\*.bat.bak`.

## 3D: the secretary scene and the face editor (2026-11-06)
**Navigation used:** `PAD_SCRIPT` with START every 2 s from 26 to 44 s, then ○ every 2 s. That path is title → 体験版 save dialog → new club → secretary select (`sb3/edit_hisyo.par`) → help → manager face edit. The game reads its installed data from `game/dev_hdd0/.../USRDIR/data`.

**What was broken, in order:**
1. **Stall at the secretary scene.**
   - The present thread spent 78% of its time in D3DCOMPILER_47 (FXC, run synchronously). No vblank ticks and no flips for 30 s or more.
   - Fix: `d3d_compile_cached` in `rsx_d3d12_backend.c` caches compiled bytecode keyed by hash(target, HLSL text). It keeps them in memory and on disk in `shader_cache_d3d12\` (`RSX_D3D12_SHADER_DISK_CACHE=0` turns the disk part off, `RSX_D3D12_SHADER_CACHE_DIR` moves it).
   - A `[d3d12-shader]` line every 2 s reports compiles, disk hits and skipped repeats. Single shaders take up to ~650 ms to compile the first time.
2. **4.7 fps in the 3D scenes and no 3D models.**
   - `CreateGraphicsPipelineState` returned E_INVALIDARG: "Root Signature doesn't match Pixel Shader: sampler s13 not bound" (found with `D3D12_DBG=1`; the FP PSO FAIL path now prints the debug-layer messages).
   - The game's 3D shaders sample **texture unit 13**, but the backend bound only units 0–3. Every such draw was dropped and retried each frame, about 430 times a frame.
   - Fix: `D3D_TEX_UNITS 16`. Draw records, cur_texs, the per-draw SRV window, the root signature (16 SRVs + 16 sampler tables), samplers and the bind filter all cover 16 units.
   - The `vp_record_cb` texscale loop stays at 4 units: the b1 layout puts alphatest right after it.
3. **FP PSO cache.**
   - It is now 4096 entries with a hash index (`vp_fp_find`). When full, the whole cache is retired, and PSOs are released two frames later (`vp_fp_release_grave`). The old one-by-one eviction freed a PSO the frame being recorded could still use.
   - `VP_FP_CACHE_LINEAR=1` restores the linear search.

**Result (hidden run):**
- 0 PSO failures; 30 fps through the secretary, help and face-edit screens.
- The 3D secretary is visible; the sample faces render.

**Still wrong:**
- The 3D head in the FACE VIEWER is a mesh of long stretched spikes (`logs/cap_tex16/frame_13`). Likely candidates: vertex attribute decoding, VP relative addressing (bones/morphs), or SPU-built geometry.
- A comparison with `SAKA_SPU_TASK_REGIONS=0` did not capture the head, so the lifted gs_task is not yet ruled out.
- Matches not reached yet.

`patches/0004-d3d12-16-texture-units-pso-index-shader-cache.patch` holds the backend diff.

## 3D shading fixes, name input (2026-11-06, after the owner's screenshots)
- **Fragment constants and alpha test** (`rsx_d3d12_backend.c`):
  - When the newer FP decompiler was adopted, the D3D12 path kept calling plain `rsx_fp_decompile`. That bakes constants into the HLSL as literals, while the PSO cache is keyed on the code alone, so every draw sharing a program used the first draw's constants. Alpha test was never applied.
  - Now: `rsx_fp_decompile_buffered_ex(..., cube_mask, ...)`. b1 = `PSConstants { fp_constants[N]; fp_alpha; }`, filled per draw by `rsx_fp_collect_constants` (the decompiler's own walk), with `fp_alpha.x` = ALPHA_REF/255.
  - `rsx_fp_apply_alpha_test_buffered` adds the discard; its comparison is part of the PSO key (`dr_alpha_key`, `akey`).
  - Cube units are declared TextureCube by the decompiler. The old text patch matched nothing any more.
- **Depth on display targets:** `RSX_DISPLAY_ZETA=1` (opt-in, the default is unchanged) gives display draws the guest zeta surface. It is reverted to opt-in because the title never CLEAR_SURFACEs its 3D zeta (0x012C0000): it resets depth with an ALWAYS/write background quad. With a persistent surface the scene went black.
- **Capture manifest:** now prints the zeta offset, `zs`, alpha_ctl and cull per draw, and the clear mask/zeta per clear.
- **Secretary greeting frame:**
  - op000–101: shadow pass into the 1024² RT (zeta 0x02F80000).
  - op106: background quad (ALWAYS + write).
  - op107–250: opaque model (zs F).
  - op251–293: hair pass 2 (zs D + alpha GREATER 1/255).
  - op322: depth clear of the UI zeta (0x00B40000), then the UI.
  - FPs sample t13 = the shadow map with PCF and write MRT.
- **Hair-over-face is still open:**
  - Not depth: `DEPTH_OFF=1` looks clearly different, so depth works partly.
  - Not culling/near-plane clipping as far as checked (the VS remaps z by viewport scale/offset).
  - Vertex attribute decode looks right.
  - Suspect: skinning/morph in the VP path (the face-editor head spikes point the same way) or hair opacity channels.
- **Name input (OSK):**
  - The stub returned at once with no sysutil events, so the game waited for OSKDIALOG_FINISHED on a black screen.
  - Now (`patches/0005`): LoadAsync queues LOADED (0x0502) and opens a native Unicode/IME input window on a host thread. FINISHED (0x0503) is queued from `cellSysutilCheckCallback` → `cellOskDialog_tick` on the guest thread. UnloadAsync writes the result (OK/CANCELED) and queues UNLOADED (0x0504).
  - `PS3_OSK_TEXT=<ascii>` answers headless.
  - Not yet exercised live: the scripted ○ path loops inside the face editor.
- **Name input, part 2:** the owner's run still went black.
  - The log from that point shows the game never reached `cellOskDialogLoadAsync`. First it calls three unresolved NIDs:
    - `SJISstoUTF8s` (0x33435818);
    - `UTF8stoUTF16s` (0xF7681B9A);
    - `cellOskDialogSetKeyLayoutOption` (0xB53C54FA).
  - The names were found by hashing NIDs: SHA-1(name + suffix), first 4 bytes LE. The EBOOT imports exactly 4 cellL10n functions; the other two are `UTF16toUTF8` and `UTF8toSJIS`.
  - `saka_l10n.cpp` (port code, registered via `ps3_hle_register_ctx` from main) implements all 5:
    - conversions via Windows code page 932 / CP_UTF8;
    - semantics and result codes per RPCS3 cellL10n.cpp/.h (ConversionOK 0, SRCIllegal 1, DSTExhausted 2, ConverterUnknown 3);
    - the first 8 calls print `[l10n]`.
  - Not exercised headless: the scripted path cannot leave the face editor.
- **Name input, part 3** (the owner's run was still black):
  - **The real blocker:** ps3recomp's `cellOskDialogLoadAsync` returned CELL_OSK_ERROR_NOT_INITIALIZED, before its first log line, unless `cellOskDialogInit` had been called. That function is not in the SDK (the HLE's own test entry point), so the title never calls it.
  - **Now, following RPCS3** (`patches/0005`):
    - no init requirement; BUSY (0x8002B105) while a dialog is loaded;
    - LOADED goes out at once and FINISHED after the window closes;
    - Abort leaves the dialog loaded, with result ABORT and then FINISHED;
    - UnloadAsync and GetInputText copy at most numCharsResultString units plus a 0, and no longer overwrite numCharsResultString.
  - **`saka_l10n.cpp`, matched to RPCS3:**
    - `SJISstoUTF8s`: Win32 flags 0 and length -1, so it reads to the NUL, does not reject invalid bytes, and writes no terminator.
    - `UTF8stoUTF16s`: walks exactly *src_len bytes. The game's len=1024 cap=16 call ends in DSTExhausted with 16 units, the same as on RPCS3.

## Owner's 21:00 run (screenshot + 37 s recording + logs), 2026-11-06
- **Name input, part 4.** LoadAsync now runs, but every open logged `entered (0 chars)` straight after it and the game went on. Frames were taken from the recording with WinRT `MediaComposition` (no ffmpeg needed); a scan of every frame finds no input window at all.
  - That message comes from either `CreateWindowExW` failing (answer = initial text) or an IDOK within about two frames. Both now log (`patches/0005`): `input window at (x,y)` or `CreateWindowExW failed (error N)`, every key with its repeat bit and ms since open, and `OK/Cancel pressed after N ms` / `window closed after N ms`.
  - Fixes in the same change: the box is centred on the `ps3recomp_d3d12` window and clamped into a monitor's work area (the first visible window of the process can be a minimised console at -32000); an Enter/Esc that is an auto-repeat or arrives in the first 300 ms is dropped; focus goes back to the game window afterwards.
  - Not yet checked live; needs the owner's run.
- **Face-editor head: fixed the spikes.** The heads and bodies are indexed TRIANGLE_STRIPs that use primitive restart (0xFFFF), sent as several DRAW_INDEX_ARRAY batches of at most 256 indices in one BEGIN/END.
  - The EBOOT calls `cellGcmSetRestartIndexEnable`/`SetRestartIndex` (headers 0x00041DAC / 0x00041DB0 written at 0x006CE6E0 / 0x006CE774). The legacy D3D12 path never decoded those methods, so the sentinel was read as vertex 65535, and each batch started a new strip (two joining triangles lost).
  - Now: `rsx_state.restart_index_enable/restart_index` (`patches/0006`), and `upload_strip_vp_indexed` carries the strip state over contiguous batches of the same BEGIN/END and cuts at the restart index (`patches/0004`). `RSX_NO_RESTART=1` restores the old reading.
  - Hidden run `logs/cap_strip1` (same PAD_SCRIPT as cap_tex16): `primitive restart 0xFFFF honoured`, 30.0 fps; the FACE VIEWER head (`frame_01`) has its proper shape: hair, ears, jaw, no spikes.
  - **Still wrong:** no eyes or eyebrows, flat skin; the face viewer is sometimes empty (recording 16 s / 20 s); the passport face thumbnail is very dark.
- Logs otherwise clean: shaders all from the disk cache, 0 failures; known leftovers are NP TUS NIDs 0x7CAF58EE / 0x2E162A62 and `cellSaveData stat callback returned error -4` (3×).
- **Name input, part 5 (owner's 21:44 run): the real cause.** The new log says `CreateWindowExW failed (error 1158)` = ERROR_NO_MORE_USER_HANDLES.
  - `GetGuiResources(GR_USEROBJECTS)` on a hidden run: about 4,900 after 10 s and the 10,000 cap after ~20 s, with only 7 windows. Disabling SDL joystick drivers one at a time: only `SDL_JOYSTICK_RAWINPUT=0` stops it (11 objects, flat for 30 s).
  - SDL's RAWINPUT driver takes WM_INPUT on a helper window of the thread that ran `SDL_InitSubSystem`. Nothing pumps that thread's messages (no SDL video), so each unread DualSense report kept a raw-input USER handle.
  - Fix (`patches/0001`, cellPad.c `pad_init_backend`): `SDL_SetHint(SDL_HINT_JOYSTICK_RAWINPUT, "0")`. The DualSense is still found (HIDAPI). `SDL_JOYSTICK_RAWINPUT=1` in the environment turns it back on.
  - The same exhaustion would break any later window, IME or cursor use, not only the name box.

## Speed: why everything waits, and the load turbo (2026-11-06, after the owner's "everything is slow")
- **Owner's 22:39 run:** the name box now works (`aa aa` reached the passport). The "crash" entering the first match is a window close during match loading (`[quit] requested (window closed)`; last opens `3d/player/*`, `png_uniform_rom.par`, then PNG decodes and `ffmt worker` threads). It is not a fault. The next run's `[perf]` lines will show whether it was stuck or only slow.
- **Measured:** our load pace equals RPCS3's (11-minute RPCS3 log: sunnyfog→player_feature 3.35 s vs ours 3.82 s; player_feature→title 10.6 s vs 10.1 s).
  - The game is frame-locked. Its pxd loader takes one step per frame: the main thread posts the `pxd::async_file_access` semaphore at 0x006DC7BC / 0x006DC95C / 0x006DCAFC (all just around the flip).
  - The file work takes ~1 ms. The main thread then sleeps ~30 ms in the vblank wait (`SEMTID=1` + `SAKA_FLIPLOG=1` timeline). So each file costs one 33 ms frame: console pace, not a port slowdown.
  - The face editor also streams parts from open archives with the UI on screen (loader busy for 16 s with 0 file opens).
- **Load turbo** (main.cpp frame clock, saka_hooks.c semaphore-post hook):
  - Trigger: at least 9 loader steps in 0.5 s, a step in the last 0.15 s, and the last frame has ≤ 48 draws (a fade, black or loading screen; `SAKA_LOAD_TURBO_DRAWS`).
  - While on, vblank ticks 8× faster (`SAKA_LOAD_TURBO=N`, 1 = off), and `rsx_d3d12_set_present_nowait(1)` presents with sync interval 0.
  - The game then runs at 130–360 fps until the load ends. Hidden boot: title at ~16 s, was 22.9 s.
  - Without the draw limit, interactive screens ran at 160–300 fps (the face editor). That is why the limit exists.
  - What is left before the title is ~5 s of logo/attention display with no loads (fixed, frame-counted).
- **SPU tasks:** lifting sl_lz_decode (mask 0x4) or sl_memcpy (0x8) boots fine; sl_decoder (0x2) stalls the boot (5 opens). The heavy-file load gaps are the same with 0x5 and 0x9, so these loads are frame-paced, not decode-bound. The default stays 0x1.
- **Still to look at:** 6 SPU threads at ~100% CPU each (interpreted taskset PM idle loop, LS 0x0C00–0x1400), and two presents per guest frame.
- **Diagnostics added:**
  - `[perf] t= fps= turbo= draws= loader=` once a second (guest frames; `SAKA_PERF_LOG=0` off);
  - `[fs] open ... t= tid=`;
  - OSK lines with `t=`;
  - `SEMTID=1` waits/posts with `t=`/`lr=` (`patches/0007`);
  - `SAKA_FLIPLOG=1`.
- **Owner's 23:22 run** (continued from the 22:38 save in `gamedata/.../BLJM61064-SAVEDATA`, backed up to `logs/save_backup_2238/`):
  - two freezes: 2 s with 474 draws in a help scene that rendered black except one stretched skin sliver (the owner's screenshot), and a final one at the main menu (fps 0 for 3+ s; then the window was closed);
  - no log lines during either, no GPU errors, shader compiles negligible.
  - A scripted run does not reproduce them (it cannot leave the face editor).
  - Hypothesis, unproven: an interpreted SPU job (sl_decoder, the task whose lift stalls the boot) holds the SPUs, so gs_task cannot build the frame.
- **Diagnostics for the next owner run:**
  - `[freeze]`: no guest frame for 1.2 s → `ppu_report_guest_lrs()` (every guest thread's lr/r3/sp), repeated every 3 s, then `[freeze] over after N s`.
  - **F12** in the game window: rsxcap of the next frame into `logs\cap_live\frame_50+` (run_logged.bat sets `RSX_CAP_DIR`). Checked with a posted WM_KEYDOWN on a hidden run.
- **Owner's 23:46 run: GPU driver timeout (TDR).** The AMD bug-report dialog appeared. The System log shows LiveKernelEvent 141 (VIDEO_ENGINE_TIMEOUT) ×9 at 23:48:15, then AppHangB1 for SakatsukuRecomp.exe.
  - Timeline:
    1. Match loading ran under turbo at ~180 fps.
    2. The match scene drew 700–1400 draws at only 10–15 fps.
    3. A black loading screen flipped at ~924/s.
    4. The log ends: the frame clock blocked. The backend's bounded GPU waits printed nothing, so the block was probably in Present.
  - The in-frame-clock `[freeze]` detector could not report, because it ran on the blocked thread. It is now a separate `freeze_watchdog` thread, which also says whether the frame clock is STUCK.
- **Fix: VP branch base and bits in the D3D12 path.**
  - BRA/BRI/CAL targets in VP microcode are absolute slots, and BRB/CLB test SET_TRANSFORM_BRANCH_BITS. `rsx_live_draw.c` and `rsx_draw_engine.c` call `rsx_vp_set_branch_base(start)` / `rsx_vp_set_branch_bits()`; `rsx_d3d12_backend.c vp_get_vs` called neither.
  - A branching program above slot 0 jumped to blocks that do not exist. Each vertex spun the 16384-pass `_pc` loop without running its code: garbage positions and enough GPU time for a TDR. Bits were always 0.
  - Now both are set, and both key the VS cache. `[VP] branching program: N instrs at slot S, bits B` is logged (first 24); `VP_BRANCH_DUMP=<dir>` writes their HLSL.
  - The scripted scenes' 8 branching VPs (50–90 instrs) are all at slot 0, forward-only, and end with `_pc = -1`: unchanged by this. The help and match scenes are unverified.
- **Present under turbo:** at most one present per 4 ms (frames are still rendered). A failed Present logs its HRESULT and GetDeviceRemovedReason.
- **Owner's 00:0x run, the match again:** `Present failed 0x887A0005, removed reason 0x887A0006` (DEVICE_HUNG). `[freeze]` reported 2.6 s with 1289 draws. All branching VPs were at slot 0, so the branch-base fix was not it. Logs kept as `logs/tdr2_*.txt`.
  - Just before the hang: 4 new 320×180 RTs (post-FX chain). The F12 capture (`logs/cap_live/frame_50`, manifest only, taken after removal) shows:
    - op580 with garbage surface state: rt=0xC9001C9D, mrt 0xC8000001/0xC9000001, zeta 0xC8020001, viewport 65024×51200. That is a desynced command stream, not a real surface.
    - post-FX passes binding their own target as a texture (op587, op589, op592).
- **Judgment against the owner's YKTK3 guide (`Desktop\리컴프_지침서.md`):**
  - The guide's lessons: reuse a proven renderer (PCSX2 GS); never grow your own. AOT-compile the coprocessor (VU1, 12→58 fields/s). Find "compiled but wrong" bugs with an interpreter-vs-compiled verify mode. Ask for live checks first. Give the player speed keys (F7 200%, F8 turbo).
  - Here we have been growing ps3recomp's legacy D3D12 backend fix by fix, and SPU work is still partly interpreted.
  - The garbage commands come from either the lifted gs_task (it writes part of each frame's RSX command buffer) or the renderer's parsing.
  - `run_logged_spu_interp.bat` (gs_task interpreted, `SAKA_SPU_TASK_REGIONS=0`, logs `spuinterp_*.txt`) splits the two with one live run.
- **References: working PS3 recomp ports on GitHub (2026-11-07):**
  - Killzone 2 *Project Pyrrhus* (first level playable, Windows D3D12, ~23–32 fps) and Killzone 3 *Project Helghast* (levels at 16–22 fps), both undecuple, both on ps3recomp.
    - Helghast's `port/patches` has ~150 runtime patches. The renderer ones are almost all **`live-draw-*`** (2003–2048: two frames in flight, full sampler state, stencil/depth-bias/MRT, 3D and float textures, guest-write validation and batched writeback, conditional render, PSO preload and async PSO, texture refresh in place, vertex payload cache).
    - Its SPU/GCM ones include `1007-gcm-walker-waits-at-fifo-park`, `1087-gcm-park-wake-on-spu-put`, `1101-gcm-spu-put-offset-table-miss`, `1037-spurs-linger-after-busy`, `1030-spurs-taskset-scan-under-lockline-lock` and taskset2 support.
  - inFAMOUS 2 (thiagofraga761): ps3recomp draw engine + Vulkan.
    - Every SPU image is lifted (policy module, job modules); idle SPUs poll 200 µs and then sleep.
    - A fence-queue release/acquire fix between the FIFO walker and `CellGcmControl.ref` cured a freeze.
  - **Conclusion:** the proven Windows path is ps3recomp's **live-draw** renderer (`RSX_LIVE_DRAW=1`), not the legacy `rsx_d3d12_backend` draw recording this port has been extending.
    - Live-draw already handles primitive restart and the VP branch base/bits.
    - Live-draw showed nothing for Sakatsuku at first only because it presents the registered display buffers, and the game flips a 1280×720 surface at 0x00F00000.
    - Plan, pending the owner's OK: make live-draw display that surface, then port Helghast's relevant live-draw and GCM/SPU patches. Keep the legacy path as the fallback switch.

## Renderer switched to live-draw; native-speed loader pump (2026-11-07, owner: "전환해")
- **Why live-draw never worked before:** main.cpp never initialised it.
  - It now does it the way `runtime/ppu/tests/boot_main.cpp` does: `rsx_null_backend_init` opens the window, `rsx_live_draw_init(hwnd, 1280, 720, rsx_live_guest_ptr)`, `rsx_null_backend_suppress_present(1)`.
  - Flips call `rsx_live_draw_present(cellGcmGetCurrentDisplayBufferId())`, and the null backend pumps messages.
  - Its present falls back to the current 1280×720 colour target when a flip names no registered buffer. That is exactly Sakatsuku's 0x00F00000 case.
- **Result (hidden runs, `logs/frames_live*`):**
  - Boot logos and the 3D title menu are correct.
  - **The secretary select renders correctly**: face, eyes, brows, lips, skin shading, hair behind the face, necklace (`frame_004560`). The legacy path drew hair over an eyeless face.
  - 0 dropped draws in 905k groups; 30 fps in the face path.
  - Still wrong: the face editor's FACE VIEWER stays empty, and F12 capture is legacy-only.
- **Launchers:** `run.bat` / `run_logged.bat` default to `RSX_LIVE_DRAW=1`. Set `RSX_LIVE_DRAW=0` (or run `run_legacy_renderer.bat`) for the old path. Backups: `logs/main.cpp.pre_live`, `logs/SakatsukuRecomp.exe.pre_live`.
- **Load turbo off by default** (`SAKA_LOAD_TURBO` default 1). The owner wants loads fast natively, not by running frames faster.
- **Loader pump (main.cpp `saka_install_loader_pump`):**
  - `0x005D59E4` calls three pxd loader state machines through pointers: `func_002E0030`, `func_002E6EE8`, `func_002E7AC8`. `SAKA_LOADER_CHAIN=1` names them.
  - `ppu_register_function` overrides their table entries; only indirect calls are affected.
  - Each wrapper calls the loader and drains the trampoline. If the call posted a loader step, it waits for the async file thread to come back to its semaphore (`saka_semaphore_wait` counts waits on the step semaphore), restores r0–r12 and lr, and calls again.
  - The budget is 15 ms per guest frame, shared by all loaders (`SAKA_LOADER_PUMP_MS`). `SAKA_LOADER_PUMP=0` turns it off. `[perf] pump+=` counts the extra calls.
  - Live-draw boot results:
    - per-file gap 67 ms → 16–38 ms;
    - first open → player_feature 12.9 s → 8.7 s.
    - The ~11–12 s of logo/attention display before the title has no loads and is unchanged.
- **Owner's first live-draw match (00:38 recording + `logs/match1_*`): the match now opens and plays** (no TDR; the clock ran to 04:02). Problems:
  - **Players drawn as white X shapes, black stadium, black intro flyover.** Live stats: `vtex[enabled=818376 binds=13236 unsupported=791904]`. The engine takes 2D vertex textures only (W16Z16Y16X16 / W32Z32Y32X32 / X32 / Y16X16 float).
    - The X shapes look like the bind pose, i.e. the skinning palette was not read.
    - Fix in `rsx_dispatch.c rsx_dsp_get_vertex_texture`: dimension 1 (1D) is treated as 2D with height 1. The first 8 unsupported vertex textures are now always logged (`[vtex] enabled but unsupported ...`).
    - Not yet verified (needs the match).
  - **3–4 s freezes, and skills slow to appear.** They line up with first-time shader compiles (`[shader-disk-cache] misses vs=32 ps=35` in the session), with the main thread in its vblank wait (0x0069E754).
    - Timed with fxc on dumped HLSL (`LD_HLSL_DUMP`): decompiled VS ≈ 0.52 s at /O1 and 0.25–0.27 s at /Od; PS ≈ 0.07 s either way.
    - The live engine now compiles VS with `D3DCOMPILE_SKIP_OPTIMIZATION` (`RSX_VS_COMPILE_OPT=1` restores). The disk cache (`scratch/rsx_shader_cache_v2`) makes repeats free.
    - The real fix is async PSO builds, as in Killzone's `2048-async-pso-builds`. Not done yet.
  - Match fps is 19–27 with 2000–6000 live draws a frame. This is performance work for later (Helghast's vertex payload cache, PSO preload, two frames in flight).
  - Patch `0008-live-draw-vtex-1d-and-fast-vs-compile.patch`.
- **Owner's 00:53 match:** skills now appear without freezes (shader cache all hits: `hits{vs=32 ps=51} misses 0`). Players and stadium are unchanged.
  - The 1D fix took effect (`vtex unsupported=0`, was 791904), but `vtex[cached=4 uploads=4 refresh=0]`: the 4 vertex textures were uploaded once and their guest memory never changed.
  - So the palettes are GPU-written (render targets), and guest memory holds stale data.
  - Now a vertex texture whose (location, offset) matches a live render surface binds that surface's SRV (`[vtex] unit ... -> render surface[n]`, first 8 logged; `vtex[... surf=N]` in the stats).
  - Surfaces used as textures are transitioned RT → PIXEL|NON_PIXEL_SHADER_RESOURCE and back, so vertex shaders may read them.
  - Title, secretary and help unchanged (hidden run `vsurf1`). The match is unverified.
  - Also noted in the match stats: `textures[... rejectZ=257483]`, i.e. depth-texture reads of zeta buffers with no recorded write (shadow/AO candidates for the black stadium).
- **Owner's 01:09 match:** players still white X shapes. `vtex[cached=1 uploads=1 refresh=0 surf=0]`: one static vertex texture, never refreshed, no surface alias. VTF is not the cause after all. `textures[rejectZ=393658]`.
- **Live-mode F12 capture:**
  - main.cpp opens `SAKA_CAPTURE_GATE` for 0.6 s on F12 (only while the game window is in the foreground).
  - run_logged.bat points the engine's `LD_SURF_DUMP_GATE` (every render surface, once, as PPM) and `RSX_DRAW_CSV_GATE` (per-draw CSV rows while the gate exists) at the same file, under `logs\cap_live\`.
  - Checked by creating the gate file during a hidden run: 6 surface dumps and 3103 CSV rows.
- **REA (rizin) on EBOOT.ELF (sha256 a5144796…), per the owner's request:**
  - Exactly four embedded ELF32-BE images, at file 0x966100 / 0x96F700 / 0x970F80 / 0x972680 (`/x 7f454c46010201`, ev_bee61a…). These are the known SPURS tasks gs_task, sl_decoder, sl_lz_decode, sl_memcpy.
  - No EDGE geometry, job-chain, SPURS-job or skinning strings (ev_fd4fba…).
  - So the match's 3D depends on the RSX renderer plus those four tasks. gs_task (lifted) builds part of every frame's RSX command and data stream.
  - The YKTK3 lesson "compiled but wrong → compare with the interpreter" applies, and this runtime has no SPU verify mode.
  - A/B launcher: `run_logged_spu_interp.bat` (`SAKA_SPU_TASK_REGIONS=0`, otherwise run_logged.bat, F12 capture included). If the match renders right with gs_task interpreted, the lifted gs_task is at fault and needs a lifted-vs-interpreted comparison.
- **ROOT CAUSE of the missing 3D (found 2026-11-07 01:3x): scaled resolves into a non-display surface were dropped.**
  - Draw CSV over the face editor: all 7,718 large 3D draws executed (0 dropped) into **0x02120000, 2560×1440** (2×2 supersampled; viewport 1280×720, zeta 0x012C0000). The shadow pass goes to 0x03380000.
  - No texture ever reads that surface. The game resolves it with scaled NV3089 blits (ds=dt=0x200000, in two tiles of 1022 + 258 px) into its 1280×720 working surface **0x00F00000**, then composites the UI there.
  - `rsx_live_draw_resolve_blit` only accepted a registered display buffer as the destination and returned 0 otherwise. Every resolve was skipped.
    - The secretary select only appeared through the `note_resolve` full-screen scanout fallback.
    - The face viewer, player-card heads and match scene never reached the screen.
  - Fix: when no display buffer contains the destination, `surface_containing()` finds the live surface (`[live-draw] resolve into surface[..]`, first 8 logged), and the same k×k `ld_downsample` / scaled path runs.
  - Hidden face run `resolve1`: the FACE VIEWER head is correct (eyes, brows, skin shading), the secretary greeting and help scenes are fine. There are a few black specks on the hair.

## Sound: fixed (2026-11-07)
- Middleware: CRI ADX2 for PS3 (Atom 1.30, AtomEx 1.27, ASR 1.03, built Apr 2013). Its SPU code is **SPURS jobs** embedded in the EBOOT (strings at file 0x97FBF0–0x9A7E20: AtomDecoder, AsrVoice, DspPack1/2, DspPitchShifter, HcaMixer). They run through the LLE libspurs_jq ("CriAtom JobQueue", workload 3).
- Output path:
  - CRI Server Manager thread (guest tid 13) runs func_0071F024, the PCM packet loop.
  - It calls the feed func_0071EB88 only through a pointer. Arguments: r3 = obj, r4 = 6 channels, r5 = samples, r6 = planar float channel pointers.
  - The feed fills a 256-sample × 8-channel staging buffer.
  - func_0071EA60 then writes that buffer into the cellAudio ring:
    - GetPortConfig, then a memcpy to portAddr + w×0x2000.
    - The write index w is at [obj+0x200C] and the lead at [obj+0x2008]; it writes only while (w − r) mod 8 < lead, where w == r means "full".
  - cellAudioAddData (NID 0xDAB029AA, not registered) is used only when lead ≤ 1, so it is never called.
- **Root cause 1 (patch 0009, ppu_hle.cpp):**
  - The HLE→PRX bridge moves r1 down 0x1000 (FLOW_REENTRY_ISO) but did not carry the caller's parameter area.
  - So every PRX export with more than 8 arguments read garbage stack arguments.
  - `_cellSpursCreateJobQueueWithJobDescriptorPool` (12 args) then failed with CELL_SPURS_JOB_ERROR_ALIGN (0x80410A10), so CRI's job queue was never created. Port2Create returned 0x80410A0F, pushes 0x80410A02, and the PCM was silent.
  - Fix: copy 0x30..0x170(r1) into the isolated frame. `PRX_STACKARGS_COPY=0` turns it off.
- **Root cause 2 (patch 0009, sys_memory.c):**
  - sys_memory_get_page_attribute reported access_right 0x8 (PPU only) for every page. libspurs_jq requires the SPU_THR bit, so the next failure was CELL_SPURS_JOB_ERROR_INVAL.
  - Fix: match RPCS3 — 0x8 for the 0xD stack region, 0xF (ANY) for everything else.
- **Dropouts (patch 0009, cellAudio.c + main.cpp):**
  - CRI's jobs run in the SPU interpreter, so its server cycle is sometimes late (gaps up to 25–36 ms).
  - Each late cycle cost a full 8-block lap of silence (42.7 ms), 12 times in 21 s.
  - Fixes:
    - `cellAudio_set_starve_check()`: when the block to read is the title's next unwritten block, the mixer holds that period and publishes r−1, so the producer can write it.
    - main.cpp raises the game's lead from 5 to 7.
  - Switches: `SAKA_AUDIO_LEAD` (0 = as shipped), `SAKA_AUDIO_HOLD=0`.
  - Hidden run snd10: the title BGM played 26.4 s with no silent block and no clicks. Boot is unchanged (30 fps, no freeze).
- Remaining risk: under heavy SPU load (a match), the interpreted CRI jobs may fall behind. The fix for that is lifting the CRI job binaries.
- **Owner's match test (13:04 run, interpreted jobs):**
  - Sound came out of the DualSense, because Windows had made the pad the default device.
  - Match audio was 40–50% silent (about 90 holds/s): the interpreted CRI jobs delivered half the needed rate.
  - The match stood still about 40 s at the kick-off after a goal while frames continued, almost certainly waiting on that sound.
  - The match ran 19–29 fps (about 23), so it played at about 77% speed because game logic is frame-locked.
  - Shader/PSO steps were negligible: 6 steps of 20–66 ms in the whole run.
- **Output device (cellAudio.c, patch 0009):** controller endpoints (Wireless Controller / DualSense / DUALSHOCK) are skipped in favour of the first other active endpoint. `PS3_AUDIO_DEVICE=<part of name>` picks one by name. The log says `output device: Speakers(Realtek(R) Audio) (chosen over the Windows default)`.
- **CRI jobs lifted (2026-11-07):**
  - The six programs are found by their "JOBCRT Ver14" header:
    - AtomDecoder 0x982E00
    - AsrVoice 0x992C80
    - DspPack1 0x99A080
    - DspPack2 0x99E280
    - DspPitchShifter 0x9A5700
    - HcaMixer 0x9B2E00
  - The jq policy streams each program to LS 0x7C00 and enters it at +0x10.
  - Each CRT relocates itself: the words listed by `ila $r5,<table>` get the load base added. That is only the header words +0x3C/+0x40, plus AtomDecoder's codec function-pointer table. Code is never patched.
  - `tools/gen_cri_spu_jobs.py game/EBOOT.ELF` writes `spu_jobs/cri_*.elf` (base 0x7C00, entry 0x7C10, the relocated code pointers as extra entries). It cuts the image at the first detected function that runs into undecodable words; AtomDecoder's float tables made the pipeline refuse every image until then. It also writes `spu_jobs/cri_jobs.inc`.
  - build(_release).bat add the ELFs to PORT_SPU_INPUTS.
  - saka_hooks.c `saka_register_cri_job_regions()` registers each code range as a verified region keyed by its EBOOT address. The compared bytes skip +0x3C..+0x5F. A fingerprint check falls back to the interpreter on a different EBOOT. `SAKA_CRI_JOBS=0` turns it off.
  - Result (hidden run cri1):
    - The CRI server runs a steady 60 cycles/s (avg 16.7 ms, max 19–24 ms; interpreted: erratic, gaps up to 36 ms).
    - Title music is 0% silent with 0 holds.
    - The mix is **bit-identical** to the interpreted run (correlation 1.000000, max diff 0).
- New diagnostics:
  - `[perf] ... present=avg/max ms` (live-engine present time per second).
  - `[slow]` lines for texture/shader/PSO steps over 20 ms (`LD_SLOW_MS`) and presents over 80 ms.
  - `SPU_GET_TRACE=<lo>-<hi>` (patch 0010) shows where DMA GETs from an EA range land in LS.
  - run_logged.bat sets `AUDIO_RATE=1`.
- Diagnostics (all opt-in):
  - `SAKA_AUDIO_PROBE=1`: feed calls, input peak, ring writes, and server-cycle gaps.
  - `SAKA_SPURS_DUMP=<s>`: the workload table.
  - `SAKA_JQ_TRACE=1`: libspurs_jq / libsre calls with arguments and results.
  - Script: scratchpad `sndtest.ps1`.
- Exe backups: `logs/SakatsukuRecomp.exe.pre_audio`, `.pre_stackargs`. Source backups: `logs/ppu_hle.cpp.pre_stackargs`, `logs/sys_memory.c.pre_access`.
- Loading leads from the owner's 01:41 run:
  - Loads of 41–50 s.
  - 4–5 s freezes with "frame clock STUCK" on new screens, very likely synchronous shader/PSO compiles in live-draw.
  - A 2.6 s freeze with the main thread at 0x006B1BD4.

## Match speed (2026-11-07 afternoon)
- **Hidden match benchmark:** scratchpad `matchbench.ps1` (-Tag, -MatchSeconds, -Env, -Shots).
  - It runs on a fresh copy of the owner's save (`PS3_GAMEDATA_ROOT`), with closed-loop input through `PAD_FILE`, keyed on file-open markers.
  - Path: title → START → DOWN → ○ (LOAD GAME) → ○ (ロード) → slot ○ → LEFT ○ (YES; NO is preselected) → ○ until `mainmenu.arch` (office) → ○ until `match_before_all.arch` (locker room) → ○ 試合開始 → ○ 試合を見る → `3d/audience/audience.par`.
  - It reaches the match in about 90 s.
- **Tools:**
  - `SAKA_HOST_SAMPLE=<start>:<secs>:<file>` samples the frame-clock thread's host RIP (main.cpp).
  - The `/MAP` linker map (build-release/SakatsukuRecomp.map) plus `tools/host_sample_report.py` give per-function shares, DLL totals and the nearest ntdll/kernelbase export.
  - `[perf]` now has `present=avg/max wait: gpu=ms/count kick=ms sites=a/b/c`.
  - `[ld-flush]` prints every 5 s: flush reasons and program-memo hit/miss counts.
- **Finding:** the frame-clock thread runs both the FIFO drain and the present; it was the match's critical path (drain 29 ms + present 7 ms per 48 ms frame, 21 fps).
- **Fixes (patch 0011, plus main.cpp):**
  1. texture_content_hash uses four FNV lanes (12% → 6%).
  2. nv3089_blit skips the guest-memory copy when the GPU resolve succeeded; the source exists only on the GPU (8.5% → about 5%). `NV3089_CPU_RESOLVE=1` keeps it.
  3. Program memo (vertex: size/inputs/TXL/hash; fragment: size + PSO key), verified by memcmp of the bytes; about 10% removed. `LD_NO_PROGRAM_MEMO=1` turns it off.
  4. **Double present:**
     - main.cpp presented again when the guest's flip REQUEST count moved, before the drain reached the flip.
     - That showed a half-drawn frame (the match intro's black flashes) and doubled the synchronous GPU waits.
     - It is now only a fallback, used when no FIFO-flip present has happened for 100 ms. `SAKA_PRESENT_ON_REQUEST=1` restores the old behaviour.
- **Result (hidden match, same save):** 21 → 27–30 fps; 30 presents/s instead of about 60; GPU waits about 160 → 90 ms/s. The remaining dips (19–23 fps) show the RSX thread waiting on the guest (kick waits up).
- **Open:**
  - Vertex fetch/decode on the CPU (about 20% of the thread).
  - The synchronous flush on every present: the CPU waits for the GPU each frame. Double-buffering would overlap them.
  - One run hit a GPU-heavy scene: 16 new pixel shaders, 3–5 fps, presents of about 100 ms.

## Boot and loading speed (2026-11-07 evening)
- **Boot logos:** after the boot loads (≈9 s), the notice/logo screens run about 12 s on frame counters, with nothing loading; START does not skip them.
  - `main.cpp` fast-forwards the vblank clock ×16 (`SAKA_BOOT_FF`, 0 = off) from the open of `sb/attention.par` to the open of `sb2/bg_top_menu.par` (title). It uses the new runtime hook `g_ppu_fs_open_hook` (patch 0012).
  - The live renderer shows at most about 60 frames/s without vsync meanwhile (`rsx_live_draw_set_present_nowait`).
  - Title now opens at 6.5 s instead of 21.5 s.
- **Loader anatomy:**
  - Manager `0x005D59E4` walks the loader tasks each frame and re-calls a task's state machine (`0x002E0030` / `6EE8` / `7AC8`, state at [task+4]) while the state changes. States 3 and 0xD are retried with usleep(1 ms), up to 10 times.
  - **State 9** waits for an async operation's done flag (byte 1 of [task+0x14], written by the completion callback `0x002E1460`) and is checked once a frame.
  - Each file has three phases:
    1. 0x101, on the main thread, synchronous inside `func_006DB7DC`.
    2. 0x104, the read, on `pxd::async_file_access(1)`.
    3. 0x103, on the main thread inside `func_006DBA28`.
  - `main.cpp` saka_loader_pump: in state 9 it waits for the flag within the shared 15 ms frame budget and steps again at once.
  - Logs: `[pump]` (stop reasons, task states), `[loadstep]` (file-thread time per step), `SAKA_LOADER_CB_TRACE=1|1a` (every completion callback), `SAKA_DECODE_TRACE=1` (decode task create/join).
- **The real cost was decompression:**
  - The read phase calls `func_006DF39C`, which sends every compressed file (LZ: a control byte plus literals such as "DDS |" and "STSBS") to the **sl_decoder** SPURS task (ELF 0x0097F700).
  - That task runs in the SPU interpreter (join 28–48 ms avg, up to 135 ms) and was 30.35 s of the benchmark route.
  - Lifting sl_decoder still stalls the boot (5 file opens): the taskset policy hand-off problem.
  - But `func_006DF39C` has its own **PPU decoder** (`loc_006DF49C`, taken when no taskset exists).
  - `recompiled/ppu_recomp_020.cpp` now jumps there when `saka_ppu_decode_forced()` (saka_hooks.c) returns true; `SAKA_PPU_DECODE=0` restores the SPU task. This is `patches/recompiled-0001-ppu-decode-path.patch`, to re-apply after any re-lift.
  - Result: read phase 30.35 s → **0.64 s** (median 1.0 ms). Title, load screen, office, locker room and match textures/UI verified on screenshots.
- **Benchmark route** (driver delays included): title 26 → 9 s; match loaded 88 → 58–66 s.
- Exe backups: `logs/SakatsukuRecomp.exe.pre_bootff`, `.pre_pump9`, `.pre_ppudecode`.

## Frame drops and 60 fps (2026-11-07 night, owner: "사양이 높은데 프레임드랍" / "원래 속도인데 60fps")
- **PSO disk cache** (patch 0013): `ID3D12PipelineLibrary` in `<shader dir>\pso_library_v1.bin`, saved every 10 s when dirty and at shutdown; `LD_NO_PSO_LIBRARY=1` = off. Second run: present max 751 → 7.5 ms.
- **vblank on its own timer thread** (`main.cpp` `vblank_thread`, `SAKA_VBLANK_THREAD=0` = old):
  - The drain loop used to tick vblank between drains. A ~20 ms match-frame drain held the 60 Hz beat back, then fired the missed ticks in a burst.
  - The game waits for two ticks per frame (`0x0069E4FC`), so every late beat cost frames.
  - Match: mostly 24–27 → 30 fps. `[perf] vbl=<ticks>/<max gap>` now shows 60/17.0–17.2 ms.
- **Vertex path** (patch 0013):
  - Compact payloads used `rsx_vertex_fetch_one` per vertex (format switch per component). They now use `rsx_vertex_fetch_many` (`LD_FETCH_ONE=1` = old), with a new CMP32 case.
  - `rsx_vertex_remap_build` uses a stamped direct map when all refs share one base_index and the ids span ≤ 256K (hash path otherwise). `topology_vertex_index` is inlined.
  - Checked by `tests/test_vertex_fetch_plan.c` (6000 random plans, 0 failures) and a remap property test (20000 random draws, 0 failures).
- **NV309E swizzled pyramid copy:** per-axis Morton tables instead of a per-pixel bit loop (`NV309E_SWZ_LOOP=1` = old). Offsets checked identical on 3.4M (x, y, l2w, l2h) cases. It did not run in the benchmark match.
- `[nv3089]` stats every 5 s (`NV3089_STATS=0` = quiet). The match does 2 GPU resolves plus 2 per-pixel CPU blits of 460.8 kpx per frame; the GPU resolve was not available for the latter.
- **Benchmark match now** (vb4):
  - About 1,700–3,300 draws: 30 fps. The render thread idles 35–50%.
  - About 5,100 draws: 24–26 fps (was 19–22). The render thread is saturated there.
  - Profile: waits 39%, fetch_many 10.6%, nv3089_blit 7.0% (the per-pixel blits above), texture_content_hash 6.7%, sink_end 5%, amdxc64 4.7%, fetch_batches_hoisted 4.6%, remap 3.3%.
- **60 fps at the original speed:**
  - The game logic is fixed at 30 ticks/s. Init calls `SetInterval(2)` once (`0x00194500` → `0x0069BEE0`, clamp 1..10, word `0x00EA40C0`).
  - The frame loop waits for `count - last >= interval` vblanks (`0x116A928`/`0x116A92C`) with no elapsed-time compensation. Nothing else reads the interval.
  - Interval 1, or `SAKA_FPS=60` (120 Hz beat), therefore runs everything at 2× speed.
  - Original speed + 60 fps needs generated in-between frames. Two ways:
    - Driver frame generation: AMD Fluid Motion Frames. The owner's GPU is a Radeon 8060S on a 2560×1600 180 Hz display. AFMF needs (borderless) fullscreen, which the port lacks: the swapchain is a fixed 1280×720 `CopyResource` present, and there is no fullscreen toggle.
    - In-renderer interpolation of transforms between frames: a multi-day project.
- Exe backups: `logs/SakatsukuRecomp.exe.pre_vbthread`. Source backups: `logs/*.pre_fetchmany`, `logs/cellGcmSys.c.pre_swz`.

## RPCS3 60 fps patch survey (2026-11-07 night, owner: "파이트 나이트 같은 게임들의 60fps패치 참고")
- Source: RPCS3's patch file (rpcs3.net/compatibility?patch&api=v1&v=1.2, 52,446 lines, 2,298 patches, 606 fps-related). It has no Sakatsuku patch and no soccer game patch.
- **Type A, limiter removal:** the engine already advances by real elapsed time.
  - Fight Night Champion "Unlock FPS" (BLUS30608 PPU-17e5fb03…): `be32 0x00752048 = 0x4e800020` (blr: the limiter function returns at once) and `bef32 0x001401d4 = 1000.0` (FPS cap).
  - Most "Unlock FPS" entries (Unreal Engine 3 titles) are 1–3 such lines. Their notes say "maximum framerate is half the Vblank frequency".
- **Type B, vblank-timed engines:** RPCS3 vblank set to 120 Hz plus a small fix (JoJo ASB/EoH, Army of Two, Formula One CE, Iron Man). Leftovers include halved input windows and 2× UI.
- **Type C, fixed-step engines.** SEGA's Ryu ga Gotoku Kenzan! is the reference (author Pakundo, v1.7):
  - Core: the vsync interval goes 2 → 1 (`li r0,1` at two sites). A code cave computes ratio = display fps / 30 into new globals (int and float at 0x10568F40/44/48).
  - About 19 modules (fighter, yact, camera, ui, script, world, particles, sound, cloth, clock30, fclock, npctask, effects, logical60…) plus 13 minigame sets. They patch hundreds of per-frame updates through code caves.
  - Float steps are divided by the ratio. Integer counters move only on every ratio-th frame.
  - About 220 KB of patch text.
  - Partial patches such as Yakuza 3 (3 lines) leave "some stuff 2× speed".
- **Sakatsuku is type C:**
  - It calls SetInterval(2) once and its frame loop never measures elapsed time.
  - The EBOOT holds no 1/30 or 1/60 float constant in .rodata/.data, but 1.0f appears 6,718 times: frame-unit steps.
  - 60.0f at 0x00EAEF88 is read by the engine timer module (func_006EFEF8, 006F1108, 006F1294, 006F15A0).
  - `SAKA_FPS=60` (the type-B trick) gave 2× speed.
  - A type-C conversion would be Kenzan-sized. It also risks changing match results if the simulation draws random numbers per tick (an inference, not checked).

## 60 ticks, Kenzan-style: phase 1 (2026-11-07 night, owner chose "켄잔식 로직 보정")
- The owner asked why AFMF was recommended. Answer: it was only the quickest way to 60 fps on their own AMD PC; it is AMD-only. A port for every PC needs an in-game method.
- **`SAKA_LOGIC60=1`** (default off; saka_hooks.c `saka_logic60`; `patches/recompiled-0002-logic60.patch`):
  - Init's `SetInterval(2)` becomes 1 (ppu_recomp_004 / 024, call site 0x001944F8).
  - The frame loop's vblank wait also runs at interval 1 (ppu_recomp_019, loc_0069E704). Unpatched, interval 1 skips the wait and runs uncapped.
  - The game ticks once per 60 Hz vblank. The vblank handler 0x0069D4B0 (count +1, lwcond signal, user callback at [0x00EA40BC]) keeps its rate.
  - Verified: vbl 60/17 ms; match 41–49 fps, render-thread bound. Everything frame-stepped runs at 2× speed until halved.
- **Rendering needs at 60 ticks** (SAKA_FPS=60 run, 2× speed):
  - Match: about 1,800 draws 56–59 fps, 2,200–3,300 draws 42–49 fps, 4,500+ draws 36–37 fps. The render thread is about 90% busy, so it needs about 1.4–1.7× more speed.
  - One FIFO desync appeared under that load: "FIFO JUMP to unmapped IO 0x1D20E65C", drain stopped for about 2.5 s. To investigate.
- **`PPU_STEP_CENSUS=1`** (patch 0014, ppu_loader.cpp):
  - Every guest store takes the hooked path (the env is added to the inline-store disable list).
  - A 4-byte store whose delta repeats, on the next frame, the delta the same function wrote to that word counts as a step: int |d| ≤ 64, or float.
  - Every `PPU_STEP_CENSUS_SEC` it prints `[step-census]` rankings, top `PPU_STEP_CENSUS_TOP`. The game runs at about 13–15 fps meanwhile.
- **Route census** (logs/cen2_err.txt; boot → title → load → office → locker → match): 42 distinct functions at ≥ 0.5 constant steps/frame. Top in the match:
  - `0x006035A8` 40/frame (int -36, float -0.0013; 60k stores/frame).
  - `0x00698A40` float +10.
  - `0x001F3668` int +1, float +10.
  - `0x005B0924` float -0.66.
  - `0x005BED40` float +1.0 (frame clock).
  - `0x002D1408` int/float +10.
  - `0x005B02C8` float -63.7.
  - `0x00257974`, `0x005E09F8` int +1.
  - `0x005B09B0` int +16.
  - `0x001F2568`, `0x001F7F60` float +0.330314.
  - `0x001B143C` float **+0.0333333** (a seconds clock: dt = 1/30 computed at run time).
  - `0x005AFB38` float +0.03125.
  - Engine frame counters `0x006BF3D0`, `0x006B0550`, `0x0069E4FC` int +1.
- **Not covered by the census:** smoothing (x += (t-x)*k), velocity integration with changing v, and logic that runs less than once per 2 frames. Find these by A/B observation.
- **Next:** renderer to 60 in typical match scenes (GPU sync pipelining, texture-hash skip, the per-pixel nv3089 blits, vertex path). Then halve the steps function by function, verifying each at 60 against 30.

## 60 ticks, phase 2: renderer (2026-11-07 night, owner: "렌더러 가속 진행")
- All in `patches/0015-frame-pipelining-texture-write-watch-ring-end.patch` (rsx_live_draw.c/.h, cellGcmSys.c, main.cpp). Every change has an off switch.
- **Frame pipelining** (`LD_PIPELINE=0` = old synchronous present):
  - Two frame contexts (command allocator, upload/vb/ib/cb/ps_cb rings, shader-visible SRV/sampler heaps) are swapped into `g.*` at each present.
  - The present only submits; it waits for the context's previous frame. Retired resources are released once their frame's fence passes.
  - Every other flush (ring overflow, readback, guest reference, movie) still waits for the whole queue.
  - Present went from 3.5–4.2 to 0.2–0.4 ms; GPU waits 0–5 ms/s.
- **Depth resolve copy skipped** (`NV3089_CPU_DEPTH=1` = keep):
  - The second scaled NV3089 pair in the match, 0x012C0000 → 0x00B40000, resolves the 2560x1440 **depth** buffer (zetatrack #4 → #3).
  - It has no GPU path, so it fell to the per-pixel CPU copy of stale guest bytes, ~7% of the thread.
  - `rsx_live_draw_is_gpu_depth()` (a zeta bound in the last 2 frames) now skips it. `[nv3089]` counts it as `depth-skip`.
  - **Open (correctness):** the 1280x720 passes that bind zeta 0x00B40000 never get the resolved depth. A GPU depth downsample would fix that; it needs a visual check.
- **Vertex constants:** each draw copied all 512 constants (8 KB) into the upload ring, 24–40 MB a frame.
  - The VP memo now records the constant range the program reads (`ld_vp_const_range`: D1.const_src, D3.index_const, reg_type 3). Only that range is copied; relative addressing copies all 512.
  - `LD_CB_FULL=1` = always 512.
- **Texture write watch** (`LD_TEX_WRITE_WATCH=0` / `PS3_VM_WRITE_WATCH=0` = hash every frame):
  - main.cpp reserves guest memory with MEM_WRITE_WATCH (CPU dirty bits, no faults).
  - At a texture's first bind in a frame, `GetWriteWatch(RESET)` over its pages feeds a per-page generation table. The texture is re-hashed only if one of its pages was written since its last hash. This is the same decision point as the old hash.
  - Safety net: a full hash every 128 skips. A change it finds is refreshed and logged `[ww-miss]`.
  - Match result: 99.2% of hashes skipped, 0 misses, 0 failures. `[ww]` prints checks/skips every 5 s.
- **Command ring end (freeze fix)** (`GCM_CTX_END_FULL=1` = old end):
  - Our HLE `cellGcmSetupContext` set `end = begin + cmdSize`. libgcm/RPCS3 keep the last word for the wrap JUMP.
  - Sakatsuku's ring is IO 0..0x700000 with fragment programs stored right after it.
  - When the game filled the ring to the brim (`current == end`), the recycle wrote the JUMP onto the first FP word. The game re-patches FP constants in place, so the JUMP was overwritten.
  - The walker then ran into shader code ("FIFO JUMP/CALL to unmapped IO", `get=0x00700004`), and the callback waited out its 2 s ("fifo recycle: drain stalled ... 2002 ms"). These are the 1–2 s freezes, in 30-tick runs too.
  - `end` is now `begin + cmdSize - 4`. 60-tick match, 100 s: 201 wraps, 0 crossings, 0 stalls.
  - `GCM_RING_DBG=1` dumps the decode run-up if the walker ever steps past the ring end.
- **Still open:** one "FIFO JUMP to unmapped IO 0x1F60C000" in 100 s, during a loading transition. The walker read inline NOP-skip data / a hole (`0xFFFFFFF5`) at 0x65794C. It resynced and rescued 9 fences (no hang).
- **Results (60 ticks, same save, fps by draw count, before → after):**
  - 2,000–2,500: 50 → 54.
  - 2,500–3,000: 44 → 51.
  - 3,000–3,500: 43 → 51.
  - 4,500–5,000: 41 → 45.
  - Under 2,000 draws: 58–60.
- **Final clean run (pl4: 60 ticks, all fixes, 100 s of match):**
  - Under 2,000 draws: 57 fps. 2,000–2,500: 56. 2,500–3,000: 54. 3,000–3,500: 54. 4,000–4,500: 51.
  - 0 freezes, 0 recycle stalls, 0 ww misses, 0 D3D errors. One rare walker resync (`CALL to unmapped IO`, get 0x1250 past a fresh put 0x468 just after a wrap), no stall.
  - Suspect for that resync: two PUT writers (PPU callback + the SPU command task) leaving PUT briefly stale or non-monotonic. Not chased.
- **The render thread is now idle 18–47% of each second** (kick 180–470 ms/s) at 51–57 fps. The guest's own frame work is the next limit in typical scenes.
  - Guest sampler (`PS3_SAMPLE=2 PS3_SAMPLE_MAIN=1`, gs1): the main PPU thread is never blocked in the OS (ntdll=0). It computes or spin-waits in user mode. Per-function names from that sampler are not reliable (its own notes: module granularity only).
  - Host profile of the busy part: `rsx_vertex_fetch_many` 21%, `sink_end_impl` 10%, memcpy 10%, driver 8%, `fetch_batches_hoisted` 7%, FIFO walk 7%, remap 6%.
- **30-tick check** (v30a, shots): match 30 fps flat (56/62 s), office/match visuals correct, 0 stalls / bad branches / ww misses / D3D errors.
- Exe backups: `logs/SakatsukuRecomp.exe.pre_pipeline` (before all of the above). Source backups: `logs/rsx_live_draw.c.pre_pipeline`, `logs/cellGcmSys.c.pre_depthskip`, `logs/main.cpp.pre_ww`.

## 60 ticks, phase 3: step correction, chunk 1 (2026-11-07, owner: "스텝 보정 먼저")
- **Census gate** (patch 0014 refreshed): `PPU_STEP_CENSUS_GATE=<file>` counts only while the file exists, from empty tables each time it opens.
  - matchbench `BENCH_MATCH_GATE=<file>` creates the file at "match loading done".
  - Without it the 60-tick route failed at the title menu: the census slows the game, and menu key-repeat is counted in ticks.
- **Compare tool:** scratchpad `cencmp.py <log30> <log60>`. Per function: example float/int step in each run, the 60/30 ratio, a verdict (time-based / FIXED per tick).
- **Engine timer = the built-in Kenzan ratio.** The timer object at `[0x00E93E70]`, updated each tick by `func_00634CF8` (the main loop `func_00188FB4` calls it; duplicate entry `func_00634CF0`):
  - `this[0xC0] = this[0xC8 + this[0xC4]*4]` — vblanks per tick (2), from a mode table.
  - `func_006352E8(this)` = dt: `this[0x150]` (measured, clamped 33,333–100,000 µs) when real-time mode `this[0x14C]` is set, else `this[0xC0] / 60.0`. 21 call sites.
  - Factor table `0x00E93F74[this[0xC0]]` = {0, 0.5, 1.0, 1.5, 2.0}. It scales three speeds at `this[0x160..]` and their squares at `this[0x16C..]`.
  - The frame clock `func_005BED40` also reads `this[0xC0]` directly.
  - The game keeps 2 even when the vblank interval is 1, so at 60 ticks everything ran 2×.
- **Fix** (`patches/recompiled-0003-logic60-engine-timer.patch`; ppu_recomp_018/029; only under `SAKA_LOGIC60=1`):
  - `this[0xC0] = 1`.
  - Measured-time minimum 16,667 µs instead of 33,333 µs.
- **Census after the fix** (cen30m vs cen60t, match):
  - Halved: frame clock 1 → 0.5; dt clock 1/30 → 1/60; `0x005AFB38`, `0x0067B3D0`, `0x00705990`, `0x001ADF38`, `0x00486888`, `0x007397A0`; `0x005B0924`, `0x005B02C8` (signed).
  - Still per tick:
    - `0x00698A40`: animation time += f1, with f1 = 1.0 from the caller through `func_00601F44`.
    - +10 steps: `0x002D1408`, `0x001F3668`, `0x001F7F60`, `0x007928BC`, `0x00246A50`, `0x0017E1D8`, `0x0020D340`; int +10 `0x001C9708`, `0x001FEE88`.
    - +225: `0x001E9A80`.
    - Fixed 1/30: `0x0071EB88`. Plus `0x005B8178` and int frame counters.
  - The +10/+225 steps write the match's own data (0x1008B000–0x100D5000): the match engine keeps its own fixed per-tick time units and does not use the engine timer.
- **End-to-end** (real time from match load to the locker room): 30 ticks 74.6 s; 60 ticks before the fix 48.1 s (1.55×); after the fix **68.2 s** (1.09×).
  - Intro screens took the same real time in both modes.
  - The HUD clock ran ahead: "1st 37:40" at 60 ticks vs "17:30" at 30 ticks, at about the same real time after load. The **match simulation still steps per tick**.
- **Open decision (owner):** the match simulation.
  - (A) Run its update every other tick, so results and balance stay identical. Positions update 30/s unless the presentation interpolates them.
  - (B) Halve its time units. Smoother, but results can differ, and per-tick AI/probability checks must all be found.

## 60 ticks, phase 3 chunk 2: match simulation kept at 30 Hz (2026-11-07, owner: "30Hz 유지(결과 동일)")
- **Call chains:** `PPU_STEP_CENSUS_CHAIN=<fn,...>` prints the guest chain the first 3 times a listed function steps. It walks the PPU back chain: `[sp]` = caller sp, saved LR at caller sp + 0x10. Patch 0014 refreshed.
- **Structure found:**
  - `func_00634CF8` is the engine's **task manager** (it also updates the timer). Each tick it walks four task lists: task `update` (vtable +0x10), `func_0064B99C(task)` (the animation pass, `0x00698A40` under it), `func_0064BA94(task)`. Then `func_006442D0(2)` and `func_006450EC` loops.
  - The **match scene task** update is `func_0065DF60`, a state machine on `this[0xCC]`. States 2, 3 and 9 call the **match tick** `func_001F65AC(this+0xA8)`, which calls `func_00288948(engine, 0)`: non-zero while running, 0 at the end (state 8). The +10 match step (`0x001F3668`) hangs under it.
- **Gate** (`patches/recompiled-0004-logic60-match30.patch`; ppu_recomp_005/018/029, saka_hooks.c; only under `SAKA_LOGIC60=1`; `SAKA_LOGIC60_MATCH30=0` = every tick, for A/B):
  - The task manager counts ticks (`saka_logic60_tick`).
  - `func_001F65AC` skips every odd tick and returns 1 ("still running").
  - On the others it runs with the timer showing its original 30 Hz values (`saka_logic60_sim_begin/end`: interval 2, factors `this[0x160+4i] = this[0x178+4i]`, squares, measured times ×2), then restores them.
- **Same-tick shots:** `SAKA_SIMTICK_SHOT=<n,...>` with `LD_SURF_DUMP` / `LD_SURF_DUMP_GATE` dumps surfaces at match ticks n, n+2, n+4. Compare tool: scratchpad `simcmp.py`.
  - Tick counts do not align across modes: the match tick also runs during the intro, which is timed in real time.
- **Results identical (evidence):** at match clock "1st 17:30", the original 30-tick run and the 60-tick run with the 30 Hz match show the same event (player 4 Watanabe Kazuhito, "ベルベットパスLV2", centre circle) and practically the same minimap layout. The ungated 60-tick run at a similar real time was at 37:40 with a different event (yellow card, No. 22).
  - Camera framing differs slightly: the camera is presentation, running at 60 Hz with dt 1/60.
  - Comparison sheet: scratchpad `cmp_1730.jpg`.
- **Speed:** first half (match load → locker room at half-time) 85.9 s at 60 ticks vs 74.6 s at 30.
  - Mean fps in the match was 53.4 vs 29.4. A fixed-step game runs at that ratio, so normalised that is about 76 s vs 73 s, within about 5%.
  - The remaining slowness is the frame rate: below 60 fps the 60-tick mode runs proportionally slow.
- **Open:**
  1. A steady 60 fps in the match (the guest-side limit from phase 2).
  2. Per-tick steps in other tasks and screens (menus/UI +10 steps, `0x00698A40` animation f1 = 1.0, fixed 1/30 at `0x0071EB88`, int frame counters).
  3. Player/ball positions move 30 times a second (match state). Fully smooth movement needs interpolation in the presentation.

## 60 ticks, phase 4: game-thread performance (2026-11-07, owner: "60fps 성능 확보")
- **New tools** (patch 0016):
  - `SAKA_HOST_SAMPLE_THREAD=main` samples the game's main PPU thread (the boot thread) instead of the frame clock. Lifted functions resolve through the linker map like runtime ones.
  - While that thread is in a DLL it also records the guest `lr` (`# lr <addr> <samples>` lines).
  - `SPU_COH_STATS=1` prints the reserved-line write rate and the hottest 64 KB regions every 5 s.
- **Main thread at 60 ticks, before:** VMX float helpers ~28% (`vmx_vmaddfp` 18.2% alone: scalar per element, byte-wise big-endian, CRT software `fmaf`); `Sleep` 17%; PPU↔SPU coherence ~10%; ucrtbase 5%.
- **Fix 1 — VMX fast paths** (`ppu_vmx.h`; `PPU_VMX_FAST=0` = scalar only):
  - vmaddfp, vaddfp, vsubfp, vrefp, vrsqrtefp and vcfsx take four lanes in SSE2: byte swap by shuffles, NJ flush by masks.
  - vmaddfp uses hardware FMA3 only when CPUID and XGETBV report it, otherwise `fmaf`. Same correctly rounded IEEE results.
  - Any NaN input or result takes the old exact scalar code (`*_exact`), which keeps the PowerPC NaN rules.
  - Result: ~28% → ~10%.
- **Fix 2 — lwcond wakeups** (`ppu_sysprx.cpp`; `PS3_LWCOND_POLL=1` = old fixed sleep):
  - `sys_lwcond_wait` was release, `Sleep(1)`, reacquire, with no-op signals. The frame loop's vblank wait (lock `0x7B8BC0` / wait `0x7B8CD8` / unlock `0x7B8BF8`; handler signal `0x7B9138`) lost ~1–4 ms of every 16.7 ms frame.
  - Signals now bump a sequence and wake all waiters (SRW lock + condition variable). A wait returns at once if a signal arrived since this thread's last wait, otherwise sleeps until a signal (max 1 ms, as before).
  - The vblank thread calls `ppu_lwcond_wake_all()` each beat.
  - Every return is still a possible spurious wakeup the game re-checks, as the old poll always was.
- **Result (60 ticks, match, fps by draw count, ms1 → ms4):**
  - 1,500–2,000: 57.2 → 59.6. 2,000–2,500: 51.7 → 58.7. 2,500–3,000: 50.0 → 60.0. 3,000–3,500: 52.1 → 58.4.
  - 3,500+ draws: 44–51, render-thread bound.
  - The main thread now idles ~24% in the vblank wait.
- **Main thread now:** VMX ~10%, coherence ~11%, wait points `0x0069E754` (vblank, idle), `0x006B1BD4` (indirect callback), `0x0069E83C` (`func_006C6A18`).
- **Coherence (open, optional):** ~950,000 PPU writes/s hit SPU-marked lines.
  - The SPURS line `0x17D00080` (~143k/s).
  - A 2 MB buffer `0x1A100000`–`0x1A300000`, all 16,405 marked lines, written ~700k/s.
  - Each write takes lock + PPU-reservation walk + SPU walk. A lock-free fast path is possible only with a PUTLLC-in-progress marker (store, fence, check reservations/marker/watches, else re-store under the lock). It is delicate: not done.
- **Checked and not it:** the mailbox write `Sleep(1)` (no waits happened). `cellGcmSetWaitFlip` is not imported by this game (it uses `cellGcmGetFlipStatus` + its own loop); a pump/event change there was reverted.
- **Build time:** a full release rebuild (all 105 lifted files) takes 3.6 min on this machine; incremental builds take 1–2 min.
- **End-to-end (m60c, all changes):**
  - 60-tick first half **75.9 s** vs 74.5 s at 30 ticks (98% of original speed), mean 58.5 fps; 0 stalls/freezes/D3D errors.
  - Before this phase: 85.9 s (0.87×).
- **30-tick check (v30b, shots):** first half 74.5 s (was 74.6), 57/61 s at 30 fps, 0 stalls/bad branches/ww misses/D3D errors. The 100 s shot is the identical "1st 17:30, No. 4 Watanabe, Velvet Pass Lv2" scene, so the VMX fast paths and lwcond change leave the match unchanged.

## 60 ticks, phase 5: heavy-scene renderer (2026-11-07, owner: "무거운 장면 렌더러")
- **Changes** (patch 0017):
  - **FIFO walker live fast path** (`cellGcmSys.c`). With live draw on, no draw engine and no per-word diagnostic env set, a contiguous run on subchannel 0/1 (3D) that stays inside one 1 MB IO page translates its IO offset once and feeds `rsx_live_draw_method` + `gcm_3d_method` per word, without the per-word diagnostic checks.
  - In live mode the legacy `rsx_state` gets only the methods with effects outside it (labels, semaphores, references), as with the draw engine. `GCM_LIVE_LEGACY_STATE=1` feeds it every word again.
  - **SIMD vertex fetch** (`rsx_vertex_compact.c`). SSE2 decoders for float, half, u8, s16 and cmp32 attributes in `rsx_vertex_fetch_many`. Results match the scalar code: the same half flush-to-zero rebuild (not F16C) and the same divisors. `LD_FETCH_SCALAR=1` = scalar.
  - **Redundant D3D12 state cache** (`rsx_live_draw.c`, `g_ls`). Skips render-target, heap, PSO, root-signature, viewport, scissor, stencil-ref and topology calls whose values are unchanged.
    - **Opt-in** (`LD_STATE_CACHE=1`). It gave no measured gain (driver share of the render thread 6.1% either way).
    - Its only screenshot run (rt3) had a black frame at the match intro (083 s). It was not investigated; with the cache off the same moments render correctly.
- **Result, 60 ticks (rt4, no shots):** 0 D3D errors, 0 write-watch misses, 0 bad branches, 0 dropped draws.
  - Light and medium match scenes: 56–60 fps (1,500–3,800 draws per frame).
  - The heaviest scene is the player entrance ("VS" scoreboard, crowd), ~5,100 draws per frame. It runs at **42–45 fps**, against 44–47 before this phase (m60c, rt1). **No measurable gain.**
  - The earlier "4,000–4,499 draws: 47.5 → 51.7" (rt2) was a lighter variant of that scene (4,000–4,500 draws), not a speed-up. That scene's draw count varies run to run (4,000–9,000).
- **Why the heavy scene stays slow:** there the render (frame-clock) thread is ~94% busy. It waits for guest commands only 50–64 ms per second, against 380–470 ms in light scenes.
  - That is ~22 ms of work per frame against a 16.7 ms budget; it needs ~30% less.
  - The cost follows draw packets (~5,100 a frame), not D3D12 draws (~1,000 a frame after batching in both scene types).
  - The GPU is not the limit: the render thread spends 0 ms waiting for the GPU there.
- **Checks with shots (current build, cache off):**
  - s60a, 60 ticks: the entrance scene renders correctly at 080 s.
  - v30d, 30 ticks: formation screen at 084 s; the 100 s shot is the identical "1st 17:30, No. 4 Watanabe, Velvet Pass Lv2" scene.
  - v30c failed earlier on menu input: the office ○ opened club info. That was an input-timing flake, not rendering.
- **Next options for the heavy scene:**
  - A persistent vertex cache: reuse converted vertices across frames while the write watch shows the guest pages unchanged. First measure how much of that scene's vertex data is unchanged frame to frame.
  - Or split the render thread (FIFO and method decode on one thread, vertex conversion and D3D12 recording on another).
- **Vertex reuse census** (2026-11-07, owner: "정점 재사용 측정"; `LD_VTX_REUSE_STATS=1`, `rsx_live_draw.c` `ld_vreuse_note`; diagnostic only, off by default).
  - **What it measures:** for each compact-path group, whether a cache would have it already. The key is the layout, attribute sources and formats, and the index source and batches. A hit needs that key seen before, in this frame or an earlier one, with none of its source pages written since (the write watch's generation table).
  - Each hit is checked against a hash of the converted output.
  - It prints once a second: `[vreuse]` shares as "% of groups / % of vertices" and the hoisted fetch's ms per frame.
  - Its overhead (per-group GetWriteWatch, output hashing) halves fps, so only its ratios are meaningful.
  - **Result (vr1, 60 ticks). 0 false hits and 0 "dirty" groups everywhere:**

    | Scene | Draws | Groups/frame | Vertices converted/frame | Hit (groups / vertices) | Same-frame repeats | No single source span | Fetch alone |
    |---|---|---|---|---|---|---|---|
    | Player entrance | ~5,100 | 1,000–1,130 | 474k–545k | 87–90% / 75% | ~50% / 46% | 4% / 25%, all with unchanged output | 8.4–9.9 ms/frame |
    | In play | 2,300–2,900 | — | ~200k | 78–87% / 97–100% | — | — | 3.3–5.0 ms/frame |
    | Before the entrance | 1,700–2,000 | — | — | 91–95% / 98–100% | — | — | 2.8–3.9 ms/frame |

  - In the entrance scene, a "no single source span" group has an attribute range that no one `guest_ptr` covers, probably IO-mapped main memory across 1 MB IO pages.
  - In play, the "new" groups (13–22% of groups, 0–2% of vertices) mostly have an output already seen under another key.
  - **Conclusion:** the converted vertex data is static.
    - A cache keyed and validated this way covers ~75% of the entrance scene's vertices as is, and ~100% once split spans are handled.
    - The entrance scene's fetch is ~9 of its ~22 ms per frame. Upload copies and remap come on top of that.
  - **Cache design notes:**
    - Harvest each touched source region once per frame, not per group: per-group GetWriteWatch syscalls are what made the census slow.
    - Check spans against the generation table by reads only.
    - Keep the "same output" hash only for diagnostics.

## 60 ticks, phase 5 chunk 2: persistent vertex cache (2026-11-08, owner: "진행" after "캐시 구현", cap 1 GB)
- **What** (patch 0018; `rsx_live_draw.c` `ld_vc_*`; on by default, `LD_VTX_CACHE=0` = fetch every group as before; cap `LD_VTX_CACHE_MB`, default 1024):
  - A compact-path group's converted vertices and final indices are kept and copied straight into the upload ring on later uses. This skips index reading, remap, fetch/convert and index generation.
  - **Key:** primitive, layout, each used attribute's descriptor, divider mask, data base offset, defaults of used attributes with no array, index array, base index, restart settings, and the group's batches.
  - **Validity:** every use harvests the group's merged source ranges (attribute bytes its indices reach, plus index bytes) with GetWriteWatch. It is a miss if any page was written since the entry was made, or if the IO map generation changed (`cellGcm_iomap_generation`, bumped in `populate_offset_table`/`clear_offset_table`).
  - **Admission:** on the second sighting. Keys not seen again within 120 frames are dropped, and data unused for 3600 frames is dropped. Over the cap, least recently used data goes first.
  - **GPU safety:** data is always copied into the frame's ring, so dropping an entry never races the GPU.
  - **Safety net:** each entry in use is rebuilt and compared once every 128 frames. A difference is logged as `[vc-miss]` and refreshed.
  - `[vcache]` prints hits, misses by reason, MB and lookup time every 5 s.
- **Result (60 ticks, vc1 vs rt4):**
  - Player entrance: **42–44 → 57–61 fps**. The render thread's idle time rose from 50–64 to 216–273 ms/s.
  - Match seconds with 1,500+ draws: mean **57.2 → 59.4 fps**; seconds at 58+ fps: 44/61 → 53/59.
  - Hits are 82–94% of lookups in a match.
  - Data is 41–62 MB, and the IO map never changed.
- **Correctness:**
  - 0 `[vc-miss]` in ~10,000 safety-net rebuilds (vc1, s60b, v30e). 0 D3D errors, write-watch misses, bad branches and dropped draws.
  - v30e (30 ticks, cache on): the 100 s shot is the identical "1st 17:30, No. 4 Watanabe, Velvet Pass Lv2" scene. s60b (60 ticks): the match renders normally.
  - Two black shots (s60b 083 s, v30e 121 s) are game fades. Dense shots of +56…+70 s after match load (d30on vs d30off) show the same HALF TIME → half-time cutscene → fade → locker room sequence in both. The cutscene variant is chosen at random per run.
    - The fade frame (black with the locker-room UI coming in) also appears in the cache-less v30b.
    - The 60-tick 083 s black is the entrance-to-formation fade; rt3 had it without the vertex cache.
- **Costs and limits:**
  - Lookups cost 2.4–4.1 ms per frame in a match, mostly the per-group GetWriteWatch syscalls. They are still well below the fetch they replace (~9 ms in the entrance scene).
  - Possible next step: harvest each region once per FIFO epoch (new PUT, semaphore wait, or this thread's own guest writes). That needs every render-thread write to guest memory enumerated.
  - Menus hit 0%: their vertex pages are rewritten every frame. Overhead there is ≤1 ms per frame at full idle.
- **Default build check (vc2, no env):** `[vcache] vertex cache: on`.
  - Entrance scene at 5,043–6,656 draws: 56–61 fps.
  - Match mean 59.5 fps, 57 of 62 seconds at 58+.
  - 0 errors and 0 `[vc-miss]`.

## 60 ticks: what the 30 Hz match tick moves (2026-11-08, owner: "선수 위치 보간")
- **Tool:** `SAKA_POS_SCAN=<match tick>` at 60 ticks (main.cpp `saka_pos_scan_tick`, called from `saka_logic60_tick`; diagnostic only).
  - From that match tick it copies the guest's committed memory below 0x40000000 on each of `SAKA_POS_SCAN_N` frames (default 9), noting which frames ran the match tick.
  - It writes to `logs/posscan.txt` every float that changed only across frames that ran the tick.
  - Analysis: scratchpad `posscan3.py`.
- **Result (ps3, ticks 1200–1204):**
  - 53,046 floats change only with the match tick. Not only positions: **the players' whole poses are computed in the 30 Hz tick**. At 60 ticks, players' movement and limb animation both step at 30 Hz; the camera and HUD are 60 Hz.
  - World-space bone matrices, 4x4 with the translation in the 4th row, e.g. `0x147EBBF0`. All bones of one player share its position, e.g. (27.37, 1.04, 24.16).
  - 3x4 matrices with the translation in the 4th column (palette layout), 64-byte blocks whose first row is not tick-only, e.g. `0x15160010`.
  - Blocks `0x15100000`–`0x15180000` hold ~30k such floats; others are `0x147E0000`–`0x14810000` and `0x150D0000`. `0x15F30000`–`0x15F60000` holds tiny tick-only values (±0.002, purpose unknown).
- **Consequence:** interpolating player positions alone would make players glide smoothly while their limbs still step at 30 Hz. Smooth motion means extrapolating the pose matrices on the frames without a match tick, then restoring the true values before the next tick so results stay identical.
- **Earlier scans (ps1, ps2, 30 ticks, every tick simulates):** moving-float pairs over 3 and 8 ticks were dominated by these matrices. They were not usable to isolate positions.
- **Pose interpolation attempts (2026-11-08, owner: "자세 보간 진행"; budget 3–3.5 h, stopped at ~2 h 10 min). None works yet; the code stays opt-in (`SAKA_POSE_INTERP=1`, saka_hooks.c) and default behaviour is unchanged.**
  - **1. Matrix extrapolation around the match tick:** capture after `sim_end`, apply at the skip decision. It found almost no pose matrices. The pose is completed after the match tick returns, apparently by SPU jobs it starts.
  - **2. Matrix extrapolation at tick starts:** apply at the start of the frame without a tick, restore at the start of the next. Matrices came from pages written in a match-tick frame and not in the next (frame-delimited write-watch windows; `rsx_ww_harvest_low` / `rsx_ww_written_pages_between`; the SRW lock in `ld_ww_harvest` is needed now that two threads harvest).
    - Only 1–2 matrices a tick moved smoothly. `SAKA_POSE_DEBUG=<tick>` shows why: the pose blocks (`0x147F`, `0x1481`, `0x150x`–`0x1516`) are **rewritten every frame** from match state. Their values only change in match-tick frames.
    - The match tick's own pages are elsewhere: `0x1011`–`0x1018` (some matrices), `0x141C`–`0x1425`, `0x148C`–`0x148D`, `0x1899`–`0x18A0` (16 pages per block; matrices that barely change), `0x16A0`, `0x1928`, `0x1A21`.
  - **3. Smooth-float extrapolation of all of the match tick's own pages:** every float whose last three values moved smoothly, half a step ahead.
    - It moved 70–380 floats a tick; 2.6k–24k a 5 s were overwritten by others in the frame between.
    - At match tick 1200 (pfon) the HUD was gone and the screen faded to black. The same moment with it off (pfoff) is normal play (1st 13:59 → 14:06).
    - It changed the game's flow: not acceptable.
  - **What would work:** extrapolate only the players' and the ball's own motion state (root position and orientation, animation times, blend weights) in the match state. That needs those structures found (RE of the player objects in the pages above), estimated 2–4 h more with an uncertain outcome.
  - **Tools kept:**
    - `SAKA_POS_SCAN` (main.cpp).
    - `SAKA_POSE_DEBUG`. Its histogram code was removed with attempt 3; `SAKA_POSE_SHOT` remains.
    - `LD_FRAME_DUMP_GATE=<file>` (rsx_live_draw.c: dump every presented frame while the file exists; with `LD_FRAME_DUMP=<dir>`, `LD_FRAME_DUMP_EVERY=1`).
    - scratchpad `framesheet.ps1`.

## 60 ticks: player motion at 60 Hz (2026-11-08, owner: "자세 보간 진행", then "역분석은 REA로", "계속")
- **RE tooling:**
  - REA's MCP rejects this PowerPC ELF: the ELF architecture check fails, and raw-binary only offers 6502/x86/ARM. REA's bundled Ghidra 12.1.4 loads it directly, run headless: `re/ghidra`, project `SakaEbootVmx`, language `PowerPC:BE:64:A2ALT-32addr` (AltiVec; the non-ALT variant truncates VMX code).
  - TOC `0x009E1848`, from the entry OPD `0x009D85C0` via REA Rizin.
  - Scripts in `re/ghidra_scripts`: `DecompileFns.java` (`<out> r2 <toc> <addr>...`) and `RefsTo.java`.
  - Runtime tools, all diagnostic:
    - `SAKA_MEM_DUMP=<tick>`: two frames of guest memory.
    - `SAKA_WATCH=<ea,...>` + `SAKA_WATCH_TICK`: hardware write watchpoints in every thread; logs/watch_<i>.txt go to `tools/host_sample_report.py`.
    - `SAKA_WATCH_AUTO=1`.
    - `SAKA_POS_SCAN`.
    - `LD_FRAME_DUMP_GATE`.
- **Findings:**
  - Match engine tick `FUN_00288948`: a 10-slot state machine that calls each state object's vtable+0x10 update. `0x283DF0` etc. are state constructors.
  - **Player model update** `_opd_FUN_0017E1D8` (OPD `0x009C91B0`), every tick:
    - It finds its match player record with `FUN_001F35F0(team, id)` (`team*0xD10 + i*0x130 + 0x20`): `+0x70` speed, `+0x74` animation time (`FUN_001F2D94`), `+0x7C` animation index.
    - The ball is `obj+0x1A30`, position `+0x170`.
    - It advances its skeleton by the time difference since it last looked (`FUN_00601F44`), then evaluates it: `FUN_0060206C` → `FUN_006035A8`, which samples animation layers at their times, slerps and multiplies through a matrix stack into bone world matrices.
    - A hardware watchpoint showed the bones rewritten by `FUN_006035A8` every tick.
  - The displayed root position is the animation's root motion; no match-state field holds it. That is why the earlier value searches found nothing.
  - A model with its own controller (`this+0x94`) uses `this+0x84` instead. Which models these are is unknown; not the goalkeepers in the match (see "Own-time models" below).
  - At 60 ticks the animation time only moves in match ticks, so every other tick the skeleton advanced by 0 and repeated its pose.
- **Fix** (`patches/recompiled-0005-anim60-pose-time.patch`; on at 60 ticks; `SAKA_ANIM60=0` = off):
  - On ticks without the match tick, the getter `func_001F2D94` and the `this+0x84` read in `func_0017E1D8` return the time half a step ahead (c + (c − b)/2).
  - This applies only when the last three values advanced steadily and the value did not change on that tick.
  - The model then advances by half a step each tick, so limbs and root motion are at 60 Hz. Nothing is written to guest memory; the match tick reads true times.
- **Evidence (60 ticks):**
  - **Per-frame scan, ticks 1200–1204:** floats changing only with the match tick fell from 59,804 to 21,043. The pose blocks went from 2,000–4,000 to under 100 each (`0x150D` 3,990→66, `0x147F` 3,618→36).
  - **Same-tick screens** (`SAKA_SIMTICK_SHOT=1300,1500`, off vs on): mean pixel difference 0.0–0.6, against 11–65 between different frames. Same clock, players, ball and HUD, so the match result is unchanged.
  - **Match state at 1200–1204:** 798 of 825 tick-only floats identical. The 27 that differ are a list of rendered positions (`0x100A5890…`, presentation) and one timer (`0x1008C148`).
  - **Default build (an1):** 0 errors; match mean 59.7 fps, 58 of 61 seconds at 58+.
- **Earlier attempts** (pose-matrix extrapolation, `SAKA_POSE_INTERP=1`, opt-in, do not use): the skeleton rewrites the matrices every tick, so what was written was overwritten. Extrapolating all match-state floats (attempt 3) changed the game flow.
- **Open:**
  - The ball is still drawn at 30 Hz: the display reads `ball+0x170` directly.
  - At 30 ticks the hook is inactive.
- **Patches:** `0019-write-watch-sharing-frame-dump-gate-diagnostics.patch` (rsx_live_draw.c write-watch lock and exports, frame dump gate; main.cpp diagnostics) and `recompiled-0005`.
- **Ball and corrections (2026-11-08, owner: "공 움직임 보간"):**
  - **Ball:** the ball model's update (`_opd_FUN_0017CB30`) copies the ball position (`ball+0x170`, ball = match object+0x1A30) through `FUN_001F0CE8`, then rolls the ball by the distance since the last tick.
    - That copy is only one of two branches. With the ball in play (`FUN_001F0CDC(ball) == 1`) the model copies `ball+0x170`. In every other state, which is most of the match, it reads the position of the object `FUN_0020A1EC` returns (`+4..+0xC`). Both meet at `0x0017CD88` with the drawn position in vr29.
    - First hook (lifted `func_001F0CE8`, return address `0x0017CCDC`) saw only the in-play branch: 0 extrapolations per 5 s in the match (ps9def). It is now an empty entry point.
    - **Current hook:** `saka_ball_vec_hook` at `0x0017CD88` in `func_0017CB30` (ppu_recomp_004.cpp). On ticks without the match tick it moves vr29 (guest byte order; key = ball object, gpr31) half a step ahead. This is the model's own vector, not the ball.
    - It applies when the ball moved steadily over the last two match ticks: |d2 − d1| ≤ half the larger step + 5 cm, steps up to 20 units. Stops, kicks, bounces and resets are left as they are.
    - Result (ps11def): called on every tick without the match tick; per 5 s, 31 of 150 ahead, the rest still or not steady. `SAKA_BALL_DEBUG=1` (one line per tick from match tick 1200): dribbles and flights 1.9–6.1 units a tick go ahead, a still ball stays.
    - Match fps 56.6–60 per 5 s window, no faults.
    - `SAKA_BALL60=0` = off.
  - **Not the player models' copy:** their copy (return `0x0017E2F4`) turns players toward the ball. It is left alone, which was the safe choice; this was not separately proven to matter.
  - **History fix (both hooks):** history is now sampled once per match tick (`th_read`/`th_commit`: the last value read during the match-tick frame).
    - Change-based history kept the last movement of a value that had stopped. That would have pushed a stopped ball, or a paused animation, back and forth every other tick.
    - Pose coverage after the fix (ps5on, tick 1200): every pose block at 0 floats changing only with the match tick.
  - **Goalkeepers: opt-in only** (`SAKA_ANIM60_GK=1`). Moving their own time (`this+0x84`) ahead changes the match.
    - Same build, tick 1200: hooks off (ps8off) vs players+ball (ps7nogk): 551 of 558 match-state floats identical; the differing 7 are a timer and rendered-position copies.
    - Hooks off vs players+ball+goalkeepers (ps8all): 88 of 549 identical; formation page `0x14206000` differs.
    - Goalkeepers therefore stay at 30 Hz.
    - **Corrected later the same day — both points above are wrong.** ps8all was a run class (see the identity method), and the `this+0x84` models are not the goalkeepers.
    - **Own-time models (`this+0x84`, `SAKA_ANIM60_GK=1`): opt-in, harmless, no measurable effect.**
      - Match: ps12gk1 (players + ball + own-time) landed in class X at its first run. Against ps11def (default) only the timer `0x1008C148` differs. Against ps8off it differs in the same 6 display copies, the timer and one stack value (`0x0FEFD908`).
      - Effect: the player pose ranges hold 46,205 floats that change only with the match tick with hooks off (ps8off), 480 with the player hook (ps11def), and still 480 with the own-time hook added (ps12gk1).
      - So in this window the player hook alone already moves every moving model at 60 Hz, goalkeepers included. The 480 left over are fewer than one model's block (3,000–4,600).
      - With the switch on, the `[anim60]` "left" count roughly doubles: the hook also sees the unused `+0x84` of normal models. The "ahead" count stays in the default range.
  - **Identity method (corrected):** runs fall into timing classes, whatever the build. The same build with hooks off, run twice (ps10offA, ps10offB), gave only 74 of 422 identical floats.
    - Within a class, hooks on and off are identical apart from display copies:
      - class X: ps8off (off), ps4on, ps7nogk, ps11def (players + new ball hook); ps8off vs ps11def 542/548, the 6 at `0x100A5900–0x100A5938`.
      - class Y: ps10offA (off), ps9def; 457/466.
      - Other classes: {ps5on, ps6anim, ps6fin}, {psoff, pson}. ps10offB and ps8all are alone.
    - Method: compare a new scan with every earlier one. A match of ≥ 98% names its class. No match says nothing.
    - The earlier line "cross-build baselines are invalid" was wrong: ps8off (older build) and ps11def agree.

## Launcher (2026-11-08, owner: "야구츠쿠3같은 런처를 좀 만들어서 게임을 하는것으로")
- `Sakatsuku Launcher.exe` next to `run.bat`, built by `build_launcher.bat` (`tools\launcher`, WPF, the .NET Framework 4 C# compiler that ships with Windows; nothing to install).
- The frame (window, sidebar, theme, dialogs, log page, shortcuts, Steam) is the YKTK3 PC launcher's (`YKTK3_Port\tools\launcher`). The memory card, cheats, PCSX2 video settings and GitHub updater are left out.
- The port has no settings file. The launcher keeps its settings in `launcher.ini` (`[Settings]`, `[Launcher]` = play record and cover) and starts the game the way `run.bat` does, with:
  - always: `PS3_VFS_ROOT`, `PS3_HDD0_ROOT`, `RT_DISPLAY_BY_SIZE=1`, `RSX_LIVE_DRAW=1`;
  - from the settings: `SAKA_LOGIC60=1` (60fps mode, default on), `SAKA_ANIM60=0` / `SAKA_BALL60=0` when switched off, `LD_VTX_CACHE=0` / `LD_VTX_CACHE_MB`, `D3D12_VSYNC=0`, `PS3_MUTE=1`, `PAD_NO_KEYBOARD=1`.
  - A setting at the port's default is not passed at all.
- Pages:
  - home: play, safe run (this run only: 30 fps, no vertex cache), game-file check (`EBOOT.ELF`, disc `PARAM.SFO` `TITLE_ID` BLJM61064, patch `APP_VER` 01.03), disc art (`PIC1.PNG` behind, `ICON0.PNG` as cover), play record;
  - settings;
  - controllers: XInput pads and the port's fixed map, including the keyboard pad of `cellPad.c`;
  - saves: `gamedata\dev_hdd0\home\00000001\savedata\BLJM61064*` with `PARAM.SFO` titles; zip backups in `gamedata\backups`, automatic before each run, restore;
  - logs: `logs\launcher\saka-*.log`, the last 20; a failed run is explained;
  - tools: desktop shortcut (`--play`), Steam, folders.
- `--shot <dir>` renders every page to PNG off screen and exits (layout check without a desktop).
- Checked: compiled; all six pages rendered.
  - `--play` started the game with the environment above; the window title read `FPS: 60.00`, `logic rate: 60 ticks/s`.
  - Closing the game window (WM_CLOSE) went through the port's `[quit]` path with exit 0, and the launcher closed without a dialog.

## Cheats (2026-11-08, owner: PS Vita PCSG00198.suprx + PCSG00198.psv, "네이티브 포팅한것에 맞게 작동하도록")
- **Sources:**
  - `PCSG00198.psv`: 37 VitaCheat codes for the PS Vita version.
  - `PCSG00198.suprx`: a fake-signed SELF, module `vitacheat_PCSG00198`, a VitaCheat stat-editor plugin. unpacked by `tools/unself_vita.py` (zlib segments); Thumb code at 0x8000; decompiled with REA's Ghidra and the `ThumbSeed.java` pre-script.
- **Plugin structure:**
  - Stat table at 0x92B0: 46 × {name, offset 0x0C..0x39, size 1, min 0, max 100}.
  - The selected player ID comes from `*(*(*(0x81BB42D0)+0x1C)+0x80)`. It is searched in the roster: 23 records × 0x218 at Vita 0x81BFEC54, matched on the u32 at +0.
  - A stat at +off is clamped to its limit at +off+0x2F. Its exp (u16 at +0x6A+2·(off−0x0C)) is reset from the game's table `*(*(0x8164875C)+4)[value]`.
- **PS3 game state: the same layout as the Vita one, Vita = PS3 + 0x80D1C300.**
  - Money: a u32 in units of 10,000 yen at **0x00EE2844** (= Vita 0x81BFEB44), found by value (60680 = the office's "6 | 0680"). A second copy sits at 0x00EE2848.
  - The club roster: **0x00EE2954** (= Vita 0x81BFEC54, money + 0x110), 23 × 0x218-byte records, big-endian. IDs are u32 at +0 (0x25F…, 19 players). +4 grade, +5 age, +7 contract, +0xC8 condition (1000 = full).
    - Its slot-order table (23 × u32, −1 = empty) follows directly.
    - Position aptitudes have stat 1 with limit 0, so a "stat ≤ limit" scan misses this roster.
  - **Correction:** the first scan found **0x00F9353C**: another club's roster with the same layout (IDs 0x35C…, the match opponent in that dump). The first converted cheats wrote there. Both were found that same day.
  - Checked in a dump: supporters 2043 and core supporters 100 (the same 100 in the two other copies); the European player list holds player IDs; the young-player records (0x18 bytes) start with their turn count; promotion records hold 0/1 flags and counts.
- **Engine** (`saka_hooks.c`, end; started from `saka_install_syscall_hooks`; patch `0020-cheat-engine.patch`):
  - A host thread reads `cheats/BLJM61064.txt` (VitaCheat syntax, PS3 addresses) and `cheats/enabled.txt` (names) when either changes, checked every second.
  - It writes the enabled codes ten times a second, big-endian, only to committed guest memory.
  - Code types: $0/$1/$2 writes and $4 multi-writes; other types are skipped. `SAKA_CHEATS=0` turns it off; the launcher's safe run sets it.
- **Converted** by `tools/psv_to_ps3.py`: 29 of the 35 Vita cheats, moved by the constant above. Values are kept, except:
  - a 32-bit write of two u16 fields: the halves swap (POLICY CENTER `4FFF7FFF` → `7FFF4FFF`; RAIO SUPPO `00006FFF` → `6FFF0000`; the PS3 holds `00 2B 00 00`);
  - a 16-bit write of the low half of a u32 ID: address +2 (COACH LIST, SCOUT LIST; the PS3 holds `00 00 02 DA`).
  - "[필드 폭 추정]" cheats (STUDY, SP/HIZO TRA, SUPPORTER S, MYUNDAM): the PS3 field was 0 in the dump. They keep the Vita byte width; the Vita author used each field's own width everywhere else.
- **Checked:**
  - Bench `cheat1` (money + age 16, on the wrong roster then): funds 9999 | 9999.
  - Bench `cheat2` (all 29 on, `cheatbench.ps1`: office, △ club, □ player list):
    - The player list showed every player at age 16, contract 3 years and full stat hexagons; funds 9999 | 9999.
    - The game reached the match.
    - The dump had every written value as intended, and the other club's roster untouched.
  - Neither run saved the game.
- **Rewritten for the PS3** (owner, same day: "우호도 … REA로 파악해봐", "트레이드 머스트 … 코드좀 찾아줘", "ARM코드 패치도"; REA's Ghidra on the PS3 EBOOT, with the `RefsInRange.java` script):
  - **UHODO (club affinity):** `0x00F8E032` holds one byte per club, 0x117 clubs.
    - `func_00518f48` bounds the index by 0x116 and treats 0xFF as "always"; `func_00544c40(club, delta)` adds to it with clamping to 0..255; `func_00544cf0` reads its level. All clubs start at 0x40.
    - It is per club, not per player as first thought.
    - The Vita code's 0x218 steps would write 22 bytes past the array. The PS3 cheat writes 0xFF to all 279 bytes: `$4001 00F8E032 FF / $0117 1 0`.
    - Checked (bench `cheat3`): exactly those 279 bytes changed.
  - **Code switches:** `$F0nn` codes set `s_cheat_switch[nn]`; hooks in the recompiled code act only while it is set. Patch `recompiled-0006-code-cheats.patch`, hooks at the end of `saka_hooks.c`.
    - **$F001 WAKATE MUST FIND:** `func_0053557C` sets up a scout's visit to a young player.
      - The 5 scouting slots are 0x18-byte records at `0x00F8EC44`: area, player, …, turns u8 +0x14, active u8 +0x15. `func_00528418` / `func_00527d30` start new searches.
      - The visit's chance (byte `0x00F8EEE3`) comes from the area table and is rolled up to twice against rand(1,100). The hook at 0x0053570C sets it to 100 before the roll, which reads it from memory.
      - A write watch from boot on the turn bytes only caught the save load (memcpy), so the code was found statically.
    - **$F002 TRADE MUST:**
      - `func_0051F148(club, player)` returns refusal reasons: bits 0–3 club/country/affinity; bits 4–10 the player record's flags (+7 &1; +8: 0x800, 0x1000, 0x2000, 0x4000, 0x8000, 0x10000000).
      - `func_0051F2B4` returns the acceptance chance (0 when reasons), rolled as `rand % 100 < chance` in `func_00520338`.
      - Hooks at their epilogues drop bits 4–10 (what the Vita byte write to every player record did) and return chance 100.
      - The player master tables: table 1 at `0x00C7D6CC` (10000 × 0x3C), table 2 at `0x00D9A080` (8082 × 0x3C), via `func_004ca1e4`. They are initialized data placed differently from the Vita, so the Vita byte offsets cannot be reused (the mapped byte is a string pointer on the PS3).
    - **$F003 FOREIGN PLAYER FRAME OVER:** `func_00584388(&free_foreign, &free_special, club, …)` counts the club's foreigners (`func_004960cc == 0`) over the 23 roster slots against its limits (own club: `func_00583eac`; a Japanese club 4 + 1).
      - The hook notes `func_0049578C` (own club) and raises both free counts to at least 23 for the user's club only.
    - Not yet seen running: the bench does not open the scouting, transfer or signing screens. The cheat thread logs `[cheats] code switches used: …` when a hook acts.
- **Left out (2):** EDIT 99 (a Vita heap address; the PS3 edit data is not found) and player exchange max (a Vita `mov.w r0,#0` at 0x813D3DBC whose PS3 counterpart is not found).
  - The Vita EBOOT would let them be matched function by function.
- **Launcher:** "치트" page (`tools/launcher/Cheats.cs`) switches `cheats/enabled.txt`, lists the cheats not yet moved, and shows a home chip with the count of cheats on.
- **Sources found later (owner: "바이두를 이용도 해봐", 5ch thread links):**
  - Baidu Tieba psvita吧 "创造球会金手指" (2018-04-11, tieba.baidu.com/p/5643841608) is the original post of most .psv codes, with Chinese labels. It was reposted in 5ch "PSVita チート解析スレ Part4" >>182 (gameurawaza/1551175120).
    - UHODO there is `$0117 00000001` (279 clubs, step 1), exactly what the PS3 code shows; the .psv's `$0017 00000218` is a later corruption.
    - STUDY ("LIU XUE 留学下月回来") steps **0xC** through 23 records; the .psv's 0x218 would write into other data. **Fixed** in the converter: `$4001 00EE8490 1 / $0017 0000000C 0`.
    - Two cheats the .psv lacks were added: stadium and club-house construction done in a week (`00EE7CF1`, `00EE7CF3`, byte 1; 0 in the dump = no construction).
    - EDIT 99 = "特E球员99（创建时使用）": the special edit player's 99 points, drawn by a slot machine when creating an edit player from the title menu ("エディット"; owner).
  - 5ch Part4 >>85, >>93 (1.03 codes; policy X/Y: X right, Y down, centre 7FFF7FFF); >>203 manager style unlock and max level (0x81CA9318, records 0xFC apart).
  - 5ch Part005 (gameurawaza/1581709488):
    - >>732–735 value lists: grade 0A–0F (0F 神の領域), growth type 00–07 (07 長期安定), traits, play styles, original skills, and the stat layout.
    - >>790/794 "若手必ず発見できる" = NOP of the failure branch of the scout's roll.
    - **>>853 "何度でも選手交代可能 $A200 813D3DBC F04F": "player exchange max" is unlimited substitutions in a match**, and "外国籍選手枠をオーバー回避" is the foreign-slot patch.
    - >>858: the player table has 0x3C records with the nationality at +2.
  - 5ch Part006 >>413: the suprx is "PCSG00198 サカつく ver1.03 自クラブ選手のドリブル精度～5-4-1理解度までの能力変更プラグイン", built from someone's Excel (>>415; "能力値を弄ると連動して経験値も書き換えている", >>441).
- **EDIT 99 search (not finished, 2026-11-08):**
  - Path: title -> down x2 -> EDIT -> PLAYER (選手エディット) -> the "JOIN! NEW PLAYER" basic-info page, which shows "ALL BONUS POINT 50pt".
  - `PS3_OSK_TEXT=TEST` answers the name keyboard. The commentary name has an extra category dialog, and after the name the cursor moves on by itself, so blind button lists drift.
  - The heap block at `0x10088908` (between the face.par and hair.par paths: model parameters) holds the age at `0x10088930` and the name (UTF-16) at `0x100889A2`.
  - No byte 0x32 (50) sits near any copy of the name. The points may be another type, or only made at the slot-machine step.
  - New diagnostic `SAKA_MEM_DUMP_GATE=<file>` (main.cpp): when the file appears, memory goes to logs/memdump_<text>.bin (menus too).
  - The menu bench is `editbench.ps1` (`-Steps "START;D;D;O;..."`, `DUMP:<name>`).

## Cheats v2 (2026-11-08, owner: YKTK3-style cheats, per-player edits and locks, real lists, "추정이나 정확하지 않은것은 REA로")
- **Files** (all in `cheats\`, read again within a second while the game runs):
  - `BLJM61064.txt`: generated by `tools/psv_to_ps3.py`.
    - `#@cat` / `#@note` / `#@value n|label|min|max|kind` / `#@once` lines are launcher metadata; the game skips `#` lines.
    - New kinds `$CT00` / `$DT0x` (one write / multi-write): write only where memory holds a larger value. 1-week/1-turn cheats can therefore stay on: an idle slot (0) is left alone.
  - `values.txt` (`<cheat>\t<n>\t<value>`, `<cheat>\tonce\t1`): value input, and 고정 off (write once when on and a game is loaded).
  - `players.txt` (`<player id>\t<field>\t<value>\t<lock>`):
    - Per-player edits by ID, written into the user's club roster only.
    - Fields: `a<n>` ability n plus its experience from the game's table 0x4D (as `func_005773A8`); `l<n>` limit n; `b`/`h`/`w<hex offset>` a byte/u16/u32 of the record.
  - `lists.txt` (`coach|scout|euro\t<slot>\t<id>\t<lock>`):
    - coach: 0x00EE72A4, 4 x 0xC.
    - scout: 0x00EE72D4, 4 x 0xC.
    - euro: 0x00EE70E8, 20 x 0x14. The player gets the status record (500 x 0xC at 0x00F0C4B8) of the player who was in the slot, as the Vita cheat did.
  - `live/state.bin`: written by the port every 2 s while a game is loaded. Contains the game state 0x00EE2700+0xB900, the status records, club affinity, and the GM block 0x00F8CE28+17x0xFC.
- **Club moves:** `func_005DAE08` rebuilds the user's club at 0x00EE2934 (roster 0x00EE2954, money, supporters, lists, facilities) when the GM moves club or league. Every cheat writes there, and player edits go by player ID, so they follow the club.
- **Launcher:**
  - Cheats page: categories with on-count, search, value boxes (staff/scout name pickers), and 고정.
  - New 선수 page: roster with game names; basic fields and 46 abilities and limits, each with 적용 / 고정 / 해제.
  - New 목록 page: coach/scout/Europe slots with name search.
  - Names come from `txt_static_data.arch`: table 0 players, 9 staff, 10 scouts, 4 clubs. Ability names come from `pledit_ability_name.txb` (patch layer first). Text format: `tools/saka_text.py`.
- **REA results** (decompile of the whole EBOOT in `re/decomp_all.c`, 21 MB):
  - Player record: +4 awakening level u8 0-15, +5 age, +7 contract, +0x0C abilities u8 0-100 (`func_005773A8` clamps), +0x3B limits u8 0-100, +0x6A exp u16, +0xC6 awakening points u16, +0xC8/CA/CC u16 0-1000, +0xE4 policy 2 x u16, +0x108 star power u16 0-0xFFFE (`func_00590DC8`), +0x208 growth type u32 0-7 (`func_004C9880`).
  - Fixed:
    - STAT MAX / limit max wrote 0xCD (205) and also +0x3A / +0x69: now 100 over the 46 bytes.
    - "쇠퇴 없음" wrote byte 7 at +0x208, the top byte of the u32 growth type: an invalid type 0x07xxxxxx. Now u32 7.
    - 면담 무제한 wrote the top byte of the u32 week flags 0x00EECE24; the interview bits 1/0x20/0x40/0x80 are in the last byte. Now 0x00EECE27.
    - Star power max is 0xFFFE.
    - Supporters: count int32 max 200,000 and core int32 max 10,000, merged into one cheat.
  - Removed:
    - CORE SUPPO 2/3: season report snapshots (`func_0050ED40` / `func_00506DB4`).
    - RAIO SUPPO: the "gained this week" display value 0x00EECEF4.
  - Confirmed u8:
    - SP training: 3 x 0xC at 0x00EE83A8; weeks left at +8.
    - 秘蔵っ子 training: 3 x 0x20 at 0x00EE83CC; type +4, program +8, weeks left +0xC.
    - Study: 23 x 0xC at 0x00EE8488; months left at +8.
    - Construction weeks: 0x00EE7CF1 (stadium), 0x00EE7CF3 (clubhouse).
    - Promotions: 81 x 0xC at 0x00EE7F08.
  - Training slots: SP and 秘蔵っ子 have exactly 3 slots each in the save data (loops `< 3`). More would need a bigger save structure, so not done. The 1-turn cheats free the slots every week.
  - Facilities:
    - Clubhouse facilities: 27 x 6 at 0x00EE7C06.
    - `func_0054F92C` returns: 0 built, 1 over capacity (sum of the facilities' size, master 0x2B +0xA, against the clubhouse level's capacity: the "cost"), 2 money, 3 level, 4/5 ok.
    - Clubhouse level u32 0x00EE6E78 (0-2); unlock bits 0x00EE6E80 (7) and stadiums 0x00EE6E7C (bits 1-14, game sets 0x7B7E).
  - GM styles: 0x00F8CE28, current style then 17 x 0xFC; +0xF4 bit 0 = unlocked (`func_00505FE0` sets, `func_00506284` reads). Save copy: styles 1-5 = 1, 6-16 = 0.
- **Code switches added:**
  - 4: 秘蔵っ子 outcome roll (`func_00567A2C` / `func_00567B74`) forced to the first outcome with a chance. SP training has no roll.
  - 5: build check 1 and 3 become 4.
  - 6: new clubhouse facility built in 1 week.
- **Live check:** hidden, muted, save copy.
  - Money (value 12,345,678), supporters 200,000/10,000, player 607 age 27→17 (locked; shown 17 in the game's player list), ability 0 12→77 with exp 1444→38189 from the table, coach slot 0 730→477, scout slot 1 409→153.
  - Clamp kinds left idle slots at 0.
  - Launcher pages rendered with the game names (logs\launcher_shots_v2).
- **Not seen running yet** (needs the screens):
  - Switches 4-6.
  - The GM style screen with all 16.
  - Europe list edit.
  - SUPPORTER S (0x00F99294 is inside match data; kept as [미검증]).
- **EDIT 99** (owner: "계속 진행"): code switch 7.
  - The edit-player menu object (heap, so no fixed address: the Vita cheat wrote a Vita heap byte) keeps the points at +0x8A8.
  - `func_003CB8E8` starts them at 50; `func_003CAD38` adds the slot bonus (+0x8AC) and caps the sum at 99, the game's own maximum.
  - Hook after the 50 is stored (`ppu_recomp_011.cpp`): 99.
  - Seen: the JOIN! NEW PLAYER screen shows ALL BONUS POINT 99pt (logs\e99_shots\09_points.jpg), log "edit points 1".
- **Not done:**
  - Unlimited substitutions: the rule texts are in the tactics menu (`交代枠をオーバー` id 0x27E5 when `func_00633E50` returns 5: lineup changes (`func_00586C48`) > the remaining count it is given; `残り交代枠を超え` 0x27DA in `func_0042B190`).
    - Stopped at its timebox (owner: "계속 진행", about 1.5 h).
    - The menu side: `func_00633E50` is the tactics menu's lineup check (`func_0042973C`); the remaining count reaches the lineup editors through the menu object (+0xA0 → editor +0xD8 / +0x350, `func_00428D30`).
    - Runtime: the bench (`subsbench2.ps1`, 60 ticks for the dump gate, ○ ○ after half-time) dumped one match nine times. The AI substituted 村上 巧 (0x362) / 吉村 圭司 (0x363) early in the second half.
      - No static counter changed then: the count is in the match's heap objects. Two of them changed exactly then: 0x100A2FA4 (1 then 3, with slot numbers 9/6/4/7 at +0xC) and 0x100A43D0, in that run.
    - Next: a write watch (SAKA_WATCH) on those during the second half to name the code, then hook its check; or the owner's button path for an in-match substitution.
- **Patches:** 0019 (main.cpp memdump gate added), 0020 (saka_hooks.c from .pre_cheats) and recompiled-0006 (014/015/016 from .pre_codecheats) regenerated. 0020 checked to rebuild the file.

## In-game cheat menu, game speed, countries (2026-11-08, owner: "야구츠쿠3처럼 인게임에서도 작동해야", "배속", "국가별로도")
- **Menu:** `saka_overlay.cpp`, Dear ImGui 1.92.7 (MIT, `third_party/imgui`, copied from the YKTK3 port's build) with its DX12 backend.
  - Drawn by the live renderer over the back buffer between the copy of the guest frame and the present (`rsx_live_draw_overlay_on/_draw` in `rsx_live_draw.c`); own shader-visible SRV heap; the renderer re-binds its heaps on every draw path.
  - Open and close:
    - F2 or L3+R3 opens and closes it.
    - Esc closes the menu, not the game (`rsx_window_message_hook` in `rsx_null_backend.c`, the live renderer's window).
    - While it is open the game reads a neutral pad (`cellPad_overlay_hook` at the end of `cellPadGetData`, after every input source incl. PAD_FILE).
  - Pad: d-pad, ○ choose, × back.
  - Tabs:
    - 치트: categories, values, 고정.
    - 선수: roster from guest memory; fields with 적용 / 고정 / 해제; boxes start at the current value.
    - 목록: coach, scout and Europe lists with a country filter and search.
  - Same files as the launcher. Fonts: Malgun Gothic plus a Japanese font merged in (glyphs on demand).
  - Switches:
    - `SAKA_OVERLAY=0`: off.
    - `SAKA_OVERLAY_OPEN=1`: open at start.
    - `SAKA_OVERLAY_TAB=n`: first tab.
    - `SAKA_OVERLAY_SHOT=<bmp>`: back buffer with the menu, for tests.
  - Seen: office screen with the 선수 tab (logs\overlay_players.png), 1280x720, 19 px font.
- **Game speed:**
  - Keys: F7 = x2 on/off, F8 = turbo x6 on/off. The menu also offers 보통/x2/x3/x4/x6.
  - `g_saka_user_speed` (main.cpp) divides the vblank beat period, as the load turbo does, and Present stops waiting for the display.
  - A badge shows the speed at the top right.
  - Measured on the office screen (sp1): 60 fps → 105–120 at x2 → 328 at x6 → back.
- **Countries:** the nationality of each player (DB +2), staff member (u32 +4 of the 100-byte records at 0x00BBA60C) and scout (u32 +4, 60 bytes at 0x00E1F6AC). Names come from `txt_static_data.arch` table 2 (134 countries).
  - The launcher's 목록 page reads them from EBOOT.ELF (`GameData`); the in-game menu reads them from memory.
  - Checked: ファンサール / スタン = オランダ.
- **Instant save states (PS2-style F5/F9): not feasible here.** The PPU/SPU code runs as native host threads, and their call stacks and registers cannot be saved and restored. The YKTK3 port can because its runtime steps the one EE itself.
- **Patches:** 0021-in-game-menu-and-game-speed (rsx_live_draw.c, rsx_null_backend.c, cellPad.c, main.cpp, CMakeLists.txt); 0020 regenerated (saka_hooks.c install call).

## Match smoothness, first pass (2026-11-08, owner: "경기 화면에서 버벅거리고 ... 30fps (센터링이나 공이 높이 띄워졌을 때)", "시작"; budget 3 h, used ~2 h 45 min)
- **Frame rate** (fm2, first half, owner's settings: 60 ticks, ANIM60/BALL60, vertex cache, VSync; hidden, 1280x720):
  - Mostly 58–61 fps. 10 of 96 s dip to 49–57 fps, at 3,300–3,800 draws a frame and at scene cuts.
  - The GPU is not the limit (`wait: gpu=0ms`). The render thread is: its idle time in the FIFO drain falls to ~300 ms/s.
  - The vertex cache works in matches (78–95% hits), but its lookups cost 2.5–4.6 ms a frame (per-group GetWriteWatch).
  - Next lever: batch those checks per frame (see phase 5 chunk 2), estimated 2–3 h.
- **Display:** the owner's monitor is 2560x1600 at 180 Hz (AMD Radeon 8060S). The swap chain stays 1280x720 (RT_DISPLAY_BY_SIZE only acts in the old d3d12 backend), so the window size does not add GPU work.
  - With sync interval 1, 60 fps frames show 11/17/22 ms by turns at 180 Hz.
  - New opt-in `LD_SYNC_INTERVAL=auto` (launcher setting "고주사율 모니터 프레임 간격 맞춤", default off) presents at refresh/60 on 120/180/240 Hz: even 16.7 ms when on time, but a late frame stays 33 ms. Match sy2 (auto) had 27 s under 58 fps vs 13 (fm2, interval 1).
  - Default unchanged (interval 1; checked at the title, both 60 fps); `LD_SYNC_INTERVAL=<1-4>` forces a value. The owner should compare live.
- **Ball** (new diagnostic `SAKA_BALL_DUMP_GATE=<file>` [+ `SAKA_BALL_DUMP_Y`, `SAKA_BALL_DUMP_SLOW=n`]: while the drawn ball is above 2.5 a gate file exists for 120 ticks, so `LD_FRAME_DUMP_GATE` dumps those frames, the game runs n times slower if asked, and `[ball-hi]` logs each tick):
  - In flight (crosses, high balls) the drawn ball is already at 60 Hz: on every tick without the match tick it goes half a step ahead.
  - **Fixed:** at a landing that half step drew the ball up to 1.5 below the pitch for a frame (tick 6353: y −1.48). The extrapolation now never goes below `min(y, 0)`. sy2: 702 high-ball ticks, 0 below ground.
  - **Hops, done** (owner: "예, 튀다"; `SAKA_BALL_SMOOTH=0` = off, default on, 30 and 60 ticks): after a landing and in dribbles the drawn position (non-in-play branch: `FUN_0020A1EC` → `[obj+0x3A10+4]` → entity +4..+0xC) stands 2–9 match ticks and then hops 2–8 units.
    - Same positions at 30 ticks (b30, original rate) as at 60: the game's own data, not the 60-tick mode.
    - A hop lands exactly on y 0 (22 of 23 in b30); a kick leaves the ground at its first step (y 0.1–2.8, all 25).
    - `ball_glide` (saka_hooks.c, after `ball_ahead`): after the ball stood ≥ 2 match ticks, a hop of 1–15 units to y 0 is drawn from the last drawn position. The remaining offset shrinks to a tenth over as many calls as the ball stood (2–6 match ticks), and x0.7 a call once the ball moves on. Kicks and restarts (> 15) are unchanged.
    - bs2 (60 ticks): all 23 hops glide, with no drawn step > 15 except restarts. Raw positions follow the glide-off run sy2 through all 6 windows (168 of 177 positions shared).
    - A first version (catch-up within one match tick, kicks included) gave 7–8 unit lurches at kick starts and was replaced.
    - **Not deterministic across runs:** hb1 (glide off) and sy2 (glide off) part at the same kick (window 3, raw 8.58,0,−13.50), and hb1 = bs1 while sy2 = bs2. So comparing runs cannot prove "visual only" by itself. The evidence for it is the same raw path as a glide-off run.
    - 30 ticks (m = 1) uses the same code but was not run with the glide.
    - Launcher (owner: "런처 스위치 먼저"): setting `BallHopSmooth` (default 1), toggle "공 순간이동 부드럽게" in the 60fps card, not dimmed because it also works at 30 fps. Off or a safe run → `SAKA_BALL_SMOOTH=0`; on → not set.
      - Checked: `--shot` settings page, and `Settings.Environment` by reflection on a temp copy (off: 0/0, on: unset/0 for normal/safe).
      - Previous exe: `logs/Sakatsuku Launcher.exe.pre_ballsmooth`.
- **Tools:** `SAKA_BALL_DUMP_*` (saka_hooks.c), `g_saka_diag_slowmo` (main.cpp vblank period), scratchpad `fullmatch.ps1` (first half only: the half-time ○ presses do not always resume).

## Korean patch beside the game files (2026-11-08, owner: "원본은 유지하고 한글패치만 따로 분리해놓고 작동시키는건 불가능한가?", "그렇게 한다면 진행해")
- **The patch** (`Sakatsuku_Korean_Patch_v0.8.exe`, never run):
  - The exe is a stub followed by a zip at 0x174C. Its central-directory offsets are relative to the zip, so the stream must start there; .NET's `ZipArchive` on the whole exe fails.
  - The zip holds `manifest.json` (format `sakatsuku-xor-v2`, `runtime_verified: false`) and one XOR delta per known original of 191 data files: 124 `.txb`, 57 `.arch`, and 10 `.par` including `font.par`. No code.
  - Patched file = (original XOR delta) cut to `target_size`, checked against `target_sha256`.
  - All 191 originals match variant 0, in `game/dev_hdd0/.../USRDIR/data` and identically in `dev_bdvd`.
  - The game reads both copies: `font.par` and `text/txt_static_data.arch` from `/dev_bdvd` at boot, most others from `/dev_hdd0`. An in-place patch would have to cover both.
  - The Korean text is kanji code points that the patched font draws as Hangul (ドリブル精度 → 球軒鷺舛腔亀).
- **Port** (`patches/0022-usrdir-overlay.patch`, ps3recomp): `PS3_USRDIR_OVERLAY=<dir>`.
  - A guest path running through `USRDIR/` is read from `<dir>/<rest>` when that is a regular file. This applies to any device; directories are never redirected.
  - `ps3_vfs_usrdir_overlay` in sys_fs.c is called first in all three translators: ppu_fs.cpp `host_path`, sys_fs.c `sys_fs_translate_path` and cellFs.c `translate_path`.
  - `[fs] overlay '<guest>' -> '<host>'` is logged with PS3_FSLOG. Without the variable nothing changes.
- **Launcher** (`KoreanPatch.cs`, settings card "한글패치"):
  - "설치" reads the exe as data and writes `game\mods\korean\data\<path>` plus `installed.txt`. It builds in a `.new` folder and swaps it in, so nothing changes unless all files are made. Takes 0.5 s.
  - Setting `KoreanPatch` (default 0) → `PS3_USRDIR_OVERLAY=game\mods\korean` when on and installed; never in a safe run.
  - "삭제" removes the folder. The launcher's own name lists and the F2 menu keep reading the originals, so names stay readable Japanese.
  - `LAUNCHER_SHOT_HEIGHT=<px>` makes `--shot` render a taller window, for long pages.
- **Checked:**
  - Install made 191 files, each matching its target hash.
  - ko2 (`-Shots`) showed Korean on the title logo, load menu, help line and load dialog.
  - ko3 ran title → office → locker room → match with 24 overlay opens of 23 files and no open failures.
  - All 382 original copies were unchanged afterwards (SHA-256).
  - Launcher: `--shot` of the card, and `Settings.Environment` by reflection (on: set; off or safe: not set).
- **Not checked:** the patch's own text in later screens (its author marks v0.8 runtime-unverified), and Korean names in the launcher/F2 lists (would need a kanji→Hangul table from `font.par`).
- The bench's load dialog defaults to NO and the `-Shots` timing then cancels it ("no office", ko1 without the overlay as well). Without `-Shots` it reaches the match.

## Save slots in the launcher, locks off by default, in-game parity (2026-11-08)
Owner: "세이브 별로 읽을수 있도록 세이브 파일을 선택 가능한것을 넣어주고, 선수들의 뭐를 하기도 전에 이렇게 고정 해놓으면 안되고 선수 목록도 그렇고. ... 모든 기능이 인게임 들어가서도 가능하도록 해야지."
- **Save format:** `gamedata\dev_hdd0\home\00000001\savedata\BLJM61064-SAVEDATA\MAIN0x.DAT` (slot x + 1, 706,988 bytes) is the game state from RAM `0x00EE27A0` on, byte for byte (RAM `0x00EE27A0..0x00F8F14C`).
  - All four exported ranges matched slot 1, which they were loaded from: the roster at save offset `0x1B4` = RAM `0x00EE2954`.
  - Only the first 0xA0 bytes of the export's first range (`0x00EE2700..0x00EE279F`) are outside the save.
- **Launcher** (`Players.cs` `GameSource`, `Cheats.cs` `LiveState.FromSave` / `LooksLikeSave`):
  - The players and lists pages have a "읽을 데이터" picker: the running game's export, each `MAIN0?.DAT` slot, or "다른 세이브 파일 고르기…" (any MAIN*.DAT, checked for a roster). The choice is kept in launcher state `DataSource`.
  - With no export yet, the newest slot is shown.
  - Edits stay keyed by player ID / list slot and are written in the game once that save is loaded.
- **Locks start off:**
  - The players/lists pages (`edit != null && edit.Lock`) and the F2 menu (`g_fieldLock` default false, `g_listLock` false). A lock appears only when asked for; "능력/한계 전부 100 고정" still lock on purpose.
  - The list-replacement cheats (COACH LIST, SCOUT LIST, EURO PLAYER LIST) are now `#@once` in `tools/psv_to_ps3.py`. They write once when turned on unless 고정 is chosen. `cheats/BLJM61064.txt` was regenerated; the only diff is those three `#@once` lines.
- **F2 menu:**
  - It already had the cheats, players and lists tabs. The players tab now has separate "능력 전부 100 고정" / "한계 전부 100 고정", as the launcher.
  - A "설정" tab with live display options was tried the same day and removed: the owner wants F2 for cheats only ("F2는 치트만 사용하도록 하는게 편할거야"). The display options and the launch-time settings stay in the launcher.
  - Cheats tab: each cheat's 고정 is drawn on its title line. Before, SameLine + Indent placed it at the start of the last row, over the value box or the title (seen in the capture f2c).
- **Checked:**
  - Launcher `--shot` with the running-game source and with slot 1 picked: roster and lists read from the save, all locks off. `launcher.ini` was restored byte for byte after the test.
  - F2 at the office (overlaytest, pad L3+R3): players tab locks unchecked; tabs 치트/선수/목록 only; cheats tab without overlaps (f2d).

## English name search, legend tags (2026-11-08)
Owner: "선수나 감독 스카우트 목록을 검색할때 영어로도 검색이 가능하도록해줘. 그리고 웹검색을 해서 가상선수들중 레전드 선수들은 괄호 해놓고 구별 가능 하도록."
- **No alphabet names in the game:**
  - `txt_static_data.arch` has full names (table 0) and short names (table 1) only.
  - The only Latin-heavy text file is `text3/ng_word.txb`, the NG word list.
- **English search:**
  - Launcher: `tools/launcher/NameSearch.cs`, used by the lists page.
  - F2 menu: `namesearch` in `saka_overlay.cpp`, used by the lists tab. Results there are cached per search text and country.
  - Method:
    - Katakana is romanized (Hepburn; small kana fold into the syllable before; ー and ッ are dropped because doubled letters fold anyway).
    - The romaji and the typed English are folded to one loose spelling: sch/tch/ch/sh/ph/th/ck/dj/dz/tz/ts/nh, c→k or s, qu/gu before e/i, v→b, l→r, j→z, y→i, no h, doubled letters once.
    - A name matches when it contains the query within a few edits; a vowel edit costs half because Japanese adds vowels. The allowance is 0 / 1 / 2 / 3 / 4 for 2–3 / 4 / 5–7 / 8–10 / 11+ letters.
    - Order: a whole word, then a word's start, then inside a word, then across words; then the shorter name.
  - Results show the romaji: "メッシ [meshi] (3710)".
  - Checked on the 18,082 players: messi → メッシ, zidane → ジダン, neymar → ネイマール, ronaldo → ロナルド, pele → ペッレ (all first), ancelotti (staff) → アンクロッティ. F2 Europe list with "messi" (capture f2e).
  - Kanji names (Japanese players, most scouts) have no reading in the game data. They are still found by Japanese text.
- **Legend tags:**
  - `cheats/legends.txt` ("player ID<TAB>label", # comments), read by the launcher (`GameText.Players`) and the F2 menu (`loadTexts`). It is data, so no rebuild is needed.
  - A tagged name reads "名前 (레전드: label)".
  - Latin letters in a name are kept by the romanizer, so the label's English name is searchable too.
- **Legend data** (`cheats/legends.txt`, 166 players; built by `make_legends.py` in the session scratchpad from a research subagent's TSV):
  - 156 come with a source: the fan blog series "サカつく2013日記" (maguron, Ameba, Feb-Mar 2014), whose position rankings from the guidebook data name the real player next to each fictional one.
    - Foreign stand-ins are at IDs 6886-7401 and 7639-7899, grouped by country or club; Japanese ones at 7402-7638.
    - Every ID was checked against table 0.
    - A summarising model extracted the pairs. Only #42, #45 and #52 were read word for word. Five same-name pairs were resolved by their neighbours: インザーラ, コルデバ, アジェラ, カンチェス, パレデス.
  - 10 are inferred from the name, with no source, under "# 이름으로 추정": Gerd Muller, the Laudrups, the de Boers, Butragueno, Dalglish, Effenberg, Fontaine, Socrates.
  - Not tagged: 16 stand-ins for players active in 2013, placed in real squads (Gotze, Lahm, Robben, Casillas, Honda, ...).
  - Searching a legend's real name finds the stand-in first: maradona → ディエゴ・マルドラド, cruyff → グラーフ, zidane → ディザン, kamamoto → 河本 鬼茂. Checked in F2 (capture f2g).
  - Order ties compare names without the tag; the brackets show the Japanese name's romaji only.
- Test aids: `SAKA_OVERLAY_SEARCH=<text>` and `SAKA_OVERLAY_LIST=0-2` (F2 lists tab: the first search text and the list shown first).

## Cheats v3: locks that hold in game (2026-11-09, owner: "돈 마저도 고정이 안돼네", "모든 치트들의 고정이 제대로 되지않으면 진짜 문제야", "구장시설은 아예 제한이 풀리지도 않았어", "고정도 가능하게 해주고 갱신도 가능하게 해서 변경도 가능해야해", "REA로")
- **Money:** the cheat thread kept 0x00EE2844 at 99,999,999, but the HUD (FUN_00084C38) is drawn when the money changes, so after a purchase it showed the lowered amount. The game changes money in three places only (re/decomp_all.c: FUN_0053F3A0 add with the month's ledger 0x00EE2850[cat], FUN_0053F434 weekly instalments, FUN_00544558 event money; each stores the old amount at 0x00EE2848 and clamps at ±99,999,999). `saka_money_hook` after each store puts the locked value back (patches/recompiled-0008-money-facility-hooks.patch; EDIT points: recompiled-0007-edit-points-lock.patch). 
- **Facilities:**
  - Clubhouse: check FUN_0054F92C (0 built, 1 over capacity, 2 money, 3 level, 4/5 can), build FUN_0054220C, used space FUN_005402C4 = the sizes of the 27 built slots (0x00EE7C06, 6 bytes). Master table 0x2B: 41 x 0x14 at 0x00E25B2C (static in the EBOOT; header 0x00E25E60 = count 0x29, base); +0xA size, +9 weeks/2, +8 cost.
  - Stadium: check FUN_0054F130 (same codes; 3 = stadium type via FUN_0053E5B4), build FUN_00542100, used space FUN_00540238 (27 slots at 0x00EE7B64). Master table 0x2A: 50 x 0x10 at 0x00E25E68; +0xA size. Capacity: FUN_004CE65C(stadium level) +0x13 (table 0x43).
  - Master headers: FUN_00574270(id) = pointer array 0x008D4A9C; header +0 count, +4 records (all static data).
  - Switch 5 turned "over capacity" into "can", which let the clubhouse reach SPACE 48/20 and broke its list (owner screenshot). Now: switch 5 only lifts code 3 (clubhouse level / stadium type), at both checks, and the same cheat writes size 0 into both master tables (data codes), so used space stays 0 and capacity is never exceeded. The build still needs a free slot of 27 (the game simply does not build otherwise). Switch 6 (1 week) also hooks the stadium build.
  - Writing the master tables is undone by restarting the game (EBOOT data).
- **Switch values, 고정, 갱신:**
  - A code switch can carry a value (`$F0nn 0 value`, replaced through values.txt like a data code). saka_hooks.c keeps per switch its value, 고정 (not "once") and a generation that goes up when it is turned on, its value or 고정 changes, or 갱신 (values.txt `<name>\trefresh\t<count>`).
  - Once cheats (고정 off) are written again only when they themselves change (turned on, values, 고정, 갱신). Before, any change of the three cheat files wrote every once cheat again.
  - Launcher cheat page and F2 menu: 고정 also for a switch with a value; 갱신 button while 고정 is off.
- **EDIT points (switch 7):** the points (edit menu object, heap, +0x8A8) are spent in func_003CC0D4, the menu's update: an item level (+0x8B0[i], 0-3) costs 10, an ability point 1; going back refunds; cancelling restores +0x90C. 고정: the value is put back after each of those five stores and after the slot bonus (func_003CAD38, also r29 for its display); the update's entry puts it back if anything else changed it and redraws the points tag (func_0064BB08 lookup + func_000879C0, the way func_003CB89C does; skipped if the tag is missing). 고정 off: the value at a new player's start, and once when turned on / changed / 갱신, also in the middle of an edit. Value 0-99 (the game's cap), 99 in the list.
- **Audit and more cheats (same day, REA Ghidra via re/decomp_all.c; three read-only analysis agents):**
  - 면담 무제한: interviews are limited per month (1 + GM skills 0x73/0x74; "今月は面談を行うことはできません"). func_00590B58(mask) tests the month's flags 0x00EECE24 (bits 1/0x20/0x40/0x80, set by func_003F9DF0, cleared monthly by func_0058B004); switch 9 makes it read them as clear (patches/recompiled-0009-interview.patch), on top of the data write.
  - 각성 포인트 "0%": the points are written (65535); the gauge (func_00568C60, thresholds table 0x54) is 0 at level 15, which the 각성 레벨 cheat locks. Notes updated; to get awakening events, use the points cheat without the level cheat.
  - GM styles usable: per style i (1-16) at 0x00F8CE28 + i*0xFC: +0xF4 bit 0 unlocked, +0xF8 u8 level (max 10), +0xFC u32 exp (Lv10 = 22500 for 1-5, 45000 for 6-16; master 0x90). The cheat now writes level 10 and those exps; skills apply when func_005063B0 runs (GM exp gain, style change). Live values matched (style 5 Lv1 exp 1488).
  - 유망주 발견 1턴: a running visit counts down a copy at 0x00F8EEE1 (func_0053557C / func_0051EC6C): now written too, all as $C000.
  - 서포터 평가 S removed: 0x00F99294 is outside the save and no code reads it.
  - Euro list replacement into an empty slot needs a free record of the 500 overseas status records (0x00F0C4B8); the live save had 500 of 500 used, so the engine skipped it. The F2 list tab says so for an empty euro slot.
  - Facility count: 27 slots each for clubhouse and stadium (save structure); the owner's save had all 54 used ("시설 수가 상한에 도달").
  - Training (switches 10-14): course conditions (func_005122DC) and the 能力覚醒 ready bit; slot keep (snapshot before the weekly passes of func_005177A0, the slot put back after func_00515B80 / func_0050CD18 clear it, with the course's full time from the masters); multipliers for normal training (func_00515548), SP weekly and end, 個性強化 exp, 能力覚醒 limit rise, awakening points (func_00568B08). Slots stay 3 (save structure).
  - Negotiation (switch 15): func_0053115C (2 sign, 1 one more, 0 refused) returns 2; the club's fee talks go through it too. Full house (switch 16): func_0053D7A8 takes the game's full-house branch after func_004D1190(0xF). patches/recompiled-0010-training-negotiation-full-house.patch.
  - Switch arrays 16 -> 32; only a switch marked "#@lock" in the list (EDIT 99) shows 고정/갱신.
  - F2 and launcher: players (새로고침 re-reads the game's values and writes once edits again, 전부 고정, 전부 한 번만, 전체 선수에 적용, 전부 해제) and lists (새로고침, 전부 고정, 전부 한 번만, 전부 해제).
- **Per-save settings (owner: "세이브 파일별로 별도로 작동하는 메커니즘을 야구츠쿠3의 치트적용 관련해서 참고해"; YKTK3 keeps cheat_saves/<save>/):**
  - enabled.txt, values.txt, players.txt, lists.txt live in cheats/saves/<id>/ ("MAIN00".. = BLJM61064-SAVEDATA/MAINnn.DAT, "new" for an unsaved new game); the title and its EDIT mode use cheats/ itself; BLJM61064.txt is shared.
  - Which save: ps3recomp's cellSaveData calls g_savedata_file_hook after each file operation (patches/0023-savedata-file-hook.patch). A MAINnn.DAT read since the title + a roster different from the title's = that save; a different roster without a read = a new game (empty settings); a save into another slot copies the settings there. The title is the menu file open (main.cpp, sb2/bg_top_menu.par or sb/top_menu.par).
  - The first save used takes the settings that were in cheats/ (marker cheats/saves/from_cheats.txt). cheats/live/save.txt tells the launcher; the F2 menu shows "이 설정: …" and reloads when the save changes; the launcher pages show the same line.
  - Bench (separate cwd copy): loading MAIN00 logged "settings of: 세이브 1 (MAIN00.DAT)", migrated 39 enabled cheats. Bench path to load: START, W10, D, O (LOAD GAME), O (ロード), O (slot 1), L (YES), O.
  - Not seen yet: a new game, saving into another slot, returning to the title from a game.
- **Youth team (owner: "유스 선수들이 매 턴마다 각성을 하는 수준으로 성장해. psv파일에 봐바"):** the .psv has no youth codes. Its player cheats all write the 23 first-team records. The owner's pasted WAKTE 1TURN is 若手発掘 in a week, already in the list as 유망주 발견 1턴.
  - The youth team is 15 player records of the same 0x218 layout at 0x00EE86E0. Live: five 16-year-olds, awakening level 1-3, abilities 8-18, limits 28-48.
  - REA Ghidra: func_00519BC0 checks youth slots < 0xF, and 15 x 0xA0 of youth data follow at 0x00EEA648; the youth menu copies 0x28C8 bytes from 0x00EE86E0.
  - New "유스" cheats (data, all 15 slots): awakening points 65535, limits 100, ability experience 0xFFFF, abilities 100, growth type (value).
  - The awakening-points multiplier (switch 14) also covers youth records. Not yet seen in game: whether youth awakening follows the points the way the first team's does.
- **Play style and skill editor (owner: "선수들 각종 스타일이나 스킬은 편집 되는거야?"):**
  - The record fields were checked against the live save. Player 617 slot 0 = 05 ドリブラー, skills 0A Lv2, 0B Lv2, 09 Lv1, then 0 / FF.
  - Layout: +0x10A u8 = the style slot in use (0-4). Five slots of 0x30 start at +0x10C: +0 u32 style (data/text2/playstyle_name.txb 1-66), +4 u32 original skill (skill_name.txb 113-181, 0 none), then five 8-byte skills from +8: u32 type (skill_type_name.txb 1-16) and u8 level at +4 (1-7, 0xFF when empty).
  - The skill name id is (type-1)*7 + level.
  - Launcher players page: card 플레이스타일·스킬 (StyleCard). F2 players tab: header 플레이스타일·스킬. Both write all fields of a player at once as players.txt b/w<offset> edits, locked or once; no game-side change.
  - Bench: players.txt "617 w10c 2" and "617 b118 5" in the MAIN00 profile became the live values (state.bin). The F2 shot showed the header, the per-save line, the refresh / apply-to-all buttons, and boxes showing current values.
- **European list into an empty slot (owner: "빈 칸: … 500칸이 다 차서 … 이걸 좀 해결해야하는데"):**
  - The 500 overseas status records (0x00F0C4B8, 12 bytes: +0 id, +4 age, +5 1-4, +8 flags) are a fixed, normally full pool. func_004CB098 only looks a player up (no allocation); func_0052DEFC is the game's own allocator.
  - The engine now follows the game's own allocator: a free record, else the highest record whose player is in neither the European list nor the 0x00EE7340 list (and not wanted by another euro edit). That record is cleared, its id set, +4 = the player's base age (player master +0x10: table 1 for ids < 10000, table 2 above, 0x3C records), and +5 = 1-4.
  - An empty slot's entry gets +4 = 2, +5 = age, as func_0052E190 writes one. A player already listed is not put in twice.
  - Bench: 6886 (no record) went into empty slot 2 and took record 499 (player 178, unlisted).
- **Team list (owner: "팀의 실제 선수 리스트가 꼭 나와야 하고 몇명인지 어떤 포지션들로 몇명있는지도"):**
  - The F2 and launcher lists show all 23 club slots (empty ones as (빈 칸)); the numbers are the game's slots.
  - A header gives the count by group and by position, and each row shows its position. Position = +0x204 u32 (0 GK, 1 LSB, 2 RSB, 3 CB, 4 DM, 5 LSM, 6 RSM, 7 OM, 8 LWG, 9 RWG, 10 FW; the names of pledit_ability_name 19-29). Checked: 佐藤 GK (0), the FWs 10.
  - The youth team's 15 follow with the same summary. Player edits (players.txt) also apply to youth records.
- **Play style names, tiers, families (owner: "번역도 해줘. 스킬도 마찬가지고 … 계열중에서 하이클래스인지도 구분"):**
  - tools/make_playstyles.py writes cheats/playstyles.txt from the text2 names and the style master table 0x27 (67 x 0x3C at 0x00E12B08: +4 family = base style 1-15, +8 tier 1/2/3, +0x10 skill types), with Korean names. 기술 트랩 follows the Korean patch.
  - The editors group styles by family, base first, with ★ / ★★ / ★★★ and "Korean (Japanese)" labels. Skill types show their level-7 name; original skills are in Korean.
- **Refresh at the right (owner: "리스트 리프레시는 오른쪽에 위치해야하고", "팀도 그렇고"):** the F2 main tab bar and the lists sub-tab bar end with a trailing 새로고침 tab button. It acts on the shown tab: cheats re-read, players re-read and write once again, lists write again. The launcher's 목록 새로고침 sits at the right of the team list.
- **Foreign players in the lineup (owner: "외국인 선수 정원초과도 제발 해결해주고"):**
  - The formation screen's 외국인 선수 정원 초과 is game_before_info_text 11002 / menu_prac_information_text 10212 (外国籍選手枠をオーバーしています).
  - func_005864DC counts the foreign players of the starting 11 and the 7 substitutes against the league rules from func_0057F700 (+4 foreign limit, +8 Asian slots, +0xC special slots) and returns 0 when over. Its only caller func_00633E50 then returns 3, which the menus FUN_0038A7E0 / FUN_00432FCC (before a match) and FUN_0042973C (team) show as that message.
  - Switch 3 (FOREIGN PLAYER FRAME OVER) now also makes it return 1 (saka_foreign_lineup_hook, patches/recompiled-0011-foreign-lineup-satisfaction.patch). Its free-slot part (func_00584388) is unchanged.
- **Facility count (owner: "구장과 클럽하우스의 시설설치 상한도 어디까지가 최대인지 체크하고 … 얼마나 설치 했는지를 알아 볼 수 있도록"):**
  - The maximum is 27 per area: the stadium's 27 x 6 slots at 0x00EE7B64 end where the clubhouse's 27 x 6 begin (0x00EE7C06), and the save holds exactly these.
  - The owner's game had 27/27 in both (cheats/live/state.bin), so 施設の数が上限に達しています is that hard limit. Space and level limits are already lifted by switch 5 and the size-0 codes.
  - The F2 header and the launcher's cheats page show 구장 n/27 · 클럽하우스 n/27.
- **Satisfaction S (owner: "경기 후 만족도도 S로 잘 작동하다가 잘 먹히지 않에"):** there was no such cheat; the earlier S grades were the game's own.
  - func_00541640 (from func_00576DA0 after the user's match; result at 0x00F99290) grades the survey 1-6 (E D C B A S): content (func_00541270, r31), players (the best player's rating, r29), facilities (built facilities' satisfaction, r30) and promotion (promotions run, r27).
  - The overall is the average of the four, and the supporters gained use them. The owner's B = S + A + S + E, the E because no satisfaction promotion was run.
  - New switch 17 (SATISFACTION S, $F011): saka_satisfaction_hook after func_0053D558 sets all four to 6, so the overall and the supporter gain are S.
- **Per-player multipliers (owner: "이것도 선수별로 개별로도 가능하게 … 중복이나 이런 문제가 발생할수 있는것에 대한 방지 대책도"):**
  - Stored as players.txt fields xt (normal training), xs (special training / 秘蔵っ子), xa (awakening points), 1-100, set in the F2 players tab and the launcher's players page (card 선수별 배수).
  - saka_hooks.c cheat_mult_for finds the player from the record the hook gets: r3, or r4 = record + ability for the limit rise. Only the club's 23 and the youth team's 15 match.
  - The player's own value replaces the switch's; the two are never multiplied, and his value works with the switch off.
  - One value per player and kind: the editors replace, and the loader keeps the last line. A player who left matches no record, so his line does nothing.
  - SP-end awakening points get the larger of special and awakening, not their product (before: both switches multiplied). Two tables are swapped on reload.
  - The team lists mark players with their own multipliers (배수 연습x5 …).
- **Team skill (試合指示) count and time (owner: "게이지 무한 유지와 시도개수 무한"):** REA rizin byte searches plus the game's text.
  - staff_sp_text 219-223 name the two settings. func_005063B0 / func_00505DBC store them weekly at game state 0x00EE27A0 + 0x4550 / + 0x4554:
    - u32 0x00EE6CF0 = 試合指示 uses per match (3; 4/5/6 with staff abilities 221-223);
    - u32 0x00EE6CF4 = effect-time extension in minutes (0; 5/10 with 219/220).
  - No direct loads of either were found (no lwz with displacement 0x4550/0x4554, nor 0x6CF0 under lis 0xEE; the 0x2028 hits are 0x00F8DFD8). The match reaches them some other way, not traced.
  - Cheat MATCH ORDERS (category 경기) locks them at 9 uses and +90 min (values 1-99 / 0-90), the game's own mechanism.
  - Not seen in a match yet. The bench match (matchbench2.ps1 = matchbench with -Cwd/-Prep) stopped at the locker room: the save's lineup is invalid (控えに出場可能な選手がいます, code 4 of func_00633E50).
  - Found on the way: the +0xD74 "3 - used" counter with skill_info_turn is in a board-style mode (0x0004xxxx-0x0006xxxx, "koma"), not the match.
- **Suspensions (owner: "레드카드를 받거나 옐로우가 누적되어서 출장이 불가능해지지 않도록"):**
  - func_004A206C records each match per competition and player: 5 bytes = games, goals, assists, rating, cards.
    - Tables: league 0x00F0FB78 + c x 0x2EA4; the others from 0x00EE27A0 + 0x5322C. Each record is at +0x22E8 + team x 0x73 + slot x 5.
    - Match entries come from the match data (team + 0x60C + slot x 0x24): +0x19 yellows, +0x1A red, +0x1B played.
  - Cards byte: 0x80 = suspended next match (red, or two yellows in a match); low 7 bits = yellows collected, against the competition's limit (2 or 4, rule +2).
  - func_0049D020 sets who can play (+0xFC of the team's player copies: 2 / 1 / 0) through func_00572524(competition, club, slot).
  - New switch 18 (NO SUSPENSION, $F012): at func_00572524's return, the user's club's cards byte is cleared (club 0x100 or u32 0x00EE28AC, as func_0049578C). The game's empty record 0x00EE1700 is skipped. patches/recompiled-0012-no-suspension.patch.
- **Red cards (owner: "레드카드가 거의 한경기당 2개씩은 나와"):** the in-match card decision is not found yet.
  - Evaluation texts 1200/1210 (FUN_000FA030: status 5 = sent off, +0x19 > 1 = two yellows) only read the result.
  - The 30 Hz match gate gave the same events as the original run (see logic60), so the rate is likely the game's own; not measured.
- **Icon (owner: "실행파일의 아이콘도 좀 만들어주라"):** app.ico, a pitch tile with a football, 16-256 px (BMP entries up to 64, PNG above).
  - Drawn by the scratchpad script make_icon.ps1 (Windows PowerShell 5.1, System.Drawing).
  - app.rc (`1 ICON "app.ico"`) is picked up by CMakeLists.txt's existing app.rc rule. tools/launcher/icon.ico is the same file (old one in logs/launcher_icon.pre_0409.ico).
- **Ball following the players (owner: "공이 선수를 늦게 따라가고 선수들의 움직임 틱에 제대로 붙는 느낌이 아니야"):**
  - The cause was the hop glide (`ball_glide`, SAKA_BALL_SMOOTH, 2026-11-08). After each dribble touch it shrank the distance to the game's ball exponentially over 4-12 calls, so the drawn ball trailed the game's ball and the player.
  - Now a hop is drawn over one match tick: half on the call it happens, the game's position from the next call. At 30 ticks nothing is glided. `SAKA_BALL_SMOOTH=long` = the old glide, `0` = off.
  - The [anim60] 5-second line now reports "ball glide N calls, behind the game's ball mean / max".
  - Bench matches (owner's save, 60 ticks, 60 s each):

    | Run | Glided calls per 5 s | Drawn behind, mean | Max |
    |---|---|---|---|
    | old (bl1) | 47-114 | 1.1-1.8 | 4.9-7.9 |
    | new (bn1) | 3-7 (one per hop) | 2.9-3.7 (half a hop, one frame) | 3.6-4.9 |

  - ball_ahead (steady-motion extrapolation) is unchanged.
- **Paused 2026-11-09 05:3x (owner: "지금은 일시정지하고 나중에 하자"). Next:**
  - **Red cards in the match (owner: about two a match):**
    - The in-match card decision is not found. The match result 0x00F97B24 (teams 0x37C from +0x604; players 0x24 from +0x60C, +0x620 goals, +0x625 yellows, +0x626 red) is not written during the 3D match: `SAKA_CARD_LOG=1` (diagnostic in the cheat thread) logged it only for other clubs' results.
    - The card models s_card_red01a / yellow01a are loaded by FUN_00269BE0 into the match scene (+0x1E0 / +0x1E4, instances +0x1EC / +0x1F0). The trail from their display back to the referee is not followed yet.
    - Ask the owner whose players get the cards.
  - **Team skill unlimited:** the HUD showed 6 with the cheat at 9 (the game's maximum).
    - Planned: two bench matches with the count at 4 and 5. Patch the copied cheat file's `$0200 00EE6CF0` line, dump once about 50 s into the match (SAKA_LOGIC60=1, SAKA_MEM_DUMP_GATE), then diff for 4 vs 5 to find the in-match count.
    - The bench needs NO SUSPENSION on to get into a match: the owner's save has suspended starters.
  - **Bench recipe (scratchpad scripts, may need re-creating):**
    - matchbench2.ps1 = matchbench.ps1 + `-Cwd` (copied cheats, firmware, spu_jobs, shader cache, as editbench2) + `-Prep <script>` (run on the copy before start).
    - prep scripts append cheat names to `cheats\saves\MAIN00\enabled.txt` in the copy.
- **Resumed 2026-11-09 (owner: "멈췄던직전작업 그대로 진행해", "자동으로 팀감지하는 기능은 모든 치트에게서"):**
  - Team skill: bench dumps with the count at 4/5/9 and a write watch found the match's 試合指示 manager.
    - Per team t (0x28 apart): +0x2C914 uses left (func_002361EC at kick-off, from the team's max = u32 0x00EE6CF0 capped at 6 for the user), +0x2C918 + slot x 8 state (4 used, 1 running, 3 over), +0x2C91C its time (func_0023A158 copies the engine's).
    - Uses are taken in func_00239E78 and func_0023A6E8 (state 4).
  - Match team objects: 0x00F8F158 + t x 0x43C4, club ID at +0 (0x100 = the user's), +0x43BC raw count, +0x43C0 extension minutes. Only the user's team gets 0x00EE6CF0/6CF4; the opponent keeps its own (鳥取: 4, 0).
  - New switch 19 MATCH ORDERS INFINITE ($F013): saka_team_skill_hook after both stores gives the use back for the user's team. patches/recompiled-0013-team-skill-unlimited.patch. Not seen in a match: the bench's home side never uses one.
  - FULL HOUSE now only for the user's match: func_0053D7A8's output r16 == 0x00F99254. The simulated other matches (func_00496998) were made full too before. Bench: "full house 1" per match, where it was 4-9.
  - Checked as user-only already:
    - training multipliers: func_00515548 loops over 0x00EE2954 only;
    - NO SUSPENSION, foreign, satisfaction, negotiation, trade, facilities, interviews;
    - the data cheats, which write the user's club area.
  - Red cards: still open. The result 0x00F97B24 is not live; the in-match decision is not found.
- **Red cards, our team (owner: "우리 팀"), 2026-11-09 midday: not found yet.**
  - STAT MAX raises only the 19 skills (no rough-play stat).
  - The match result 0x00F97B24 is not written live.
  - Memory dumps through SAKA_MEM_DUMP_GATE every 15 s disturb the match: a cutscene at ~123 s, then back to the locker room. So the 11→10 drops found in those dumps are not evidence.
  - The in-match swap error 4 (この選手は退場処分のため…) is set by the generic menu state machine FUN_003FFBE8, not traced to the flag.
  - Next: a lighter probe than full dumps (a small range logged by the cheat thread), or the write watch on the team's player copies (+0xFC) during a full match.
- **Cards found statically (2026-11-09 afternoon, owner: "옐로우나 레드카드가 경기중에 나오지 않게"):** REA Ghidra decomp_all.c, from the result writer back.
  - The result writer FUN_0020c038 → FUN_0020bb50 copies:
    - from the match stats table 4 (`FUN_002319d0(core, 4)`): team t player s at data + t*0x668 + 0xC0 + s*0x40. +0x27 = yellows → result +0x19; +0x28 = red → +0x1A;
    - the player copy's +0xFC status → result +4 (5 = sent off).
  - The play-sequence hub FUN_001fae14 (table 0x10) takes the current play record (`FUN_00205448(seq, 0)`) and sends record+0x28 to FUN_002151e4 → stats FUN_002d730c.
  - A foul sub-event at record+0x28 has these fields:
    - [0] = 9/10;
    - [0xC] = side of the fouler, [0xD] = fouler;
    - [8]/[9] = victim side/player;
    - [0xE] = code (record+0x60).
  - Code % 3: 0 = no card, 1 = yellow (a second yellow becomes red: FUN_002d6e34), 2 = red. Code / 3 = the kind of foul: 3-5 / 6-8 / 9-11 also injure the victim.
  - FUN_002dcc48 (from FUN_0020d340 / FUN_0020e03c) counts the card on the fouler: FUN_002d515c = yellow, FUN_002d51c0 = red. When FUN_002d51f4 > 0 it sets status 5 (FUN_002d5204, player copy +0xFC) = sent off.
  - Where the event is made: the play builder func_00205220 (and func_00205568) clears the new record from a template, then runs the steps (func_0028EC08 …).
    - func_0028EC08 calls func_0028CA48(…, record+0x28, previous record, previous record+0x223C, …).
    - That function writes the foul (type 9, or 10 when func_002CE174), the sides and players, and `code = [prev+0x223C + 0xCC]`, the decision carried over from the previous play.
    - Only then does the builder hand the record to func_00204900 (restart), and from there to the consumers above.
  - New switch 20 NO CARDS ($F014): saka_no_cards_hook runs right after the build in both builders. A foul by the user's team (side → team as func_002CDA60; match_team_is_user) gets code - code % 3. patches/recompiled-0014-no-cards.patch.
    - The pending decision upstream only takes effect through this event, so the referee, the scene, the send-off and the records all see no card.
    - Every foul with a card logs `[nocards] foul type … code … side … period … -> team …`, even with the switch off. The first 64 fouls without a card are logged too.
  - Bench nc2 (owner's save copy, NO SUSPENSION + NO CARDS, scratchpad matchbench2.ps1, which presses ○ on menus during the match): 3 fouls. One was ours with a yellow (type 9, code 1, side 1, period 1 → team 0): `card removed`, so the counter is 1. The other two (code 0) had no card.
    - Seen only through the log: the referee's scene and the result screen. The owner's live check is pending.
    - The bench's ○ presses after half-time stopped in a menu (488 draws), so the second half was short.

## Repository and releases without bat files (2026-11-09, owner: "실행파일까지 깃헙에 올려줘", "리포지토리에 있는 bat파일들은 모두 삭제하거나 숨겨줘. 오직 exe들로만")
- The .bat files are no longer in the repository (`*.bat` in .gitignore). They stay on this PC for building: build_release.bat, build_launcher.bat, make_release.bat and run*.bat.
- `tools/make_release.ps1` no longer puts run.bat in the release; the launcher starts the game itself.
- Releases are on GitHub Releases: one zip with `Sakatsuku Launcher.exe` and `build-release\SakatsukuRecomp.exe` + SDL2.dll, and no game data.

## Legend list re-check (2026-11-10, owner: "https://enjoigame.sakura.ne.jp/sakatsuku_2013/index.html 여기서 레전드 선수들 리스트 다시 체크해줘")
- The site's 94 pages name only three such players, checked against txt_static_data.arch table 0:
  - 7473 平塚 浪馬 is modelled on 中田英寿 (sensyu_osusume2.html). It was already tagged Nakata.
  - 7472 河本 鬼茂 is a young-talent pick, a 16-year-old top-class FW. It was already tagged Kamamoto; the site names no model.
  - 7450 中山 雅史 is the pre-order bonus (sensyu_tokuten.html: 16 with the code, 23 in normal play). It was missing and has been added to `cheats/legends.txt` (167 players now).
- The 13 helper players for an original club (sensyu_osusume.html) are fictional, not legends.
- **Second pass** (2026-11-10). Owner: "sheepplus.com/sakatsuku2013, sakatuku7.game-cmr.com/2013/data/model, sakatsuku2013.shiyo.info 여기들을 모두 뒤져봐".
  - Only GAME-CMR has a model list: in-game name -> real player. It has a Japan page, 50 country pages and a few more pairs on its young-talent page. sheepplus and shiyo only have recommendations and young-talent tips.
  - The list has 1446 rows. 599 match game IDs in the legend range 6886-7899.
    - Names were matched exactly.
    - Japanese one-character typos and 3 rows with swapped columns were matched by hand.
    - A short name that fits several IDs was given the ID nearest the other IDs from the same country page.
  - 122 of our earlier tags agree with the site.
  - Kept: 7356 マルゲッティ stays Materazzi.
    - The site says Maldini, and its young-talent page adds "たぶん" (probably).
    - It is a stopper, while 6912 マルティニ (sweeper, captaincy) is Paolo Maldini.
  - Fixed: 7827 ルイス・ペリーラ is Luís Pereira (Brazil, 1970s).
    - The Verdy Pereira is 7642 ペリーラ. It sits in a block of J.League foreign players, 7639-7645, with Hašek, Dutra and Salinas.
  - Added 476 players, labelled "Korean, English".
    - Latin spellings and Japanese readings come from the opening line of each real player's ja.wikipedia article.
    - 5 of the name-inferred entries are now confirmed by the site.
  - Total: 643 players. 638 have a source and 5 are still inferred (Gerd Muller, the Laudrups, the de Boers).
  - Backups: logs/legends.txt.pre_1110 (before 7450) and logs/legends.txt.pre_1110b (before the second pass).
