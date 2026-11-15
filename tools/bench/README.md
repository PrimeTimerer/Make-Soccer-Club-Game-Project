# Bench scripts

Hidden, muted test runs and their analysis helpers, moved here from session scratchpads on 2026-11-09.
- They find the repository root from their own folder.
- Work files go to `logs\bench\` (a copy of `gamedata\`, the pad file, gate files). Logs and captures go to `logs\`.
- The game runs from `build-release\SakatsukuRecomp.exe`. While the owner is playing, use `-Cwd <empty folder>` (matchbench) so the run has its own cheats and shader cache. Test a new build as a `.new.exe` instead of replacing the exe.

| Script | What it does |
| --- | --- |
| `matchbench.ps1` | Title → load slot 1 → office → locker room → match, driven by file-open markers through `PAD_FILE`. `-Tag`, `-Env "A=1;B=2"`, `-MatchSeconds`, `-Shots`, `-Cwd` (separate working folder), `-Prep <script>` (run on that folder first). ○ on menus during the match. |
| `fullmatch.ps1` | The same route with a longer match (first half). |
| `cheatbench.ps1` | Match route plus the office, club and player-list screens, for cheat checks. |
| `menubench.ps1` | Any menu path: `-Steps "START;W10;D;O;DUMP:name"` (button, wait, memory dump by gate); `EV:<n>` / `REF` set and refresh the EDIT points value. `-Exe` (e.g. a `.new.exe`), `-Cwd`. |
| `sndtest.ps1` | Title music for N seconds, writes the mix to `logs\<tag>.f32` (f32le stereo 48 kHz). |
| `framesheet.ps1` | Contact sheet of dumped frames (`ppm2bmp.py` converts them). |
| `cencmp.py` | Compares two `PPU_STEP_CENSUS` logs (30 vs 60 ticks): which per-tick steps still need halving. |
| `simcmp.py` | Compares match frames dumped at the same match ticks (`SAKA_SIMTICK_SHOT`) in two runs: mean pixel difference per pair, and between consecutive frames for scale. |
| `posscan.py` | Reads `logs\posscan.txt` from `SAKA_POS_SCAN`: floats that change only with the match tick, by 64 KB block, with the moving coordinate pairs and their strides. |

Switches and markers are described in `NOTES.md` and `docs\리컴프_지침서.md` (chapter 4).

2026-11-13 continuation checks:

- `test_edit_players.c`: compile with clang-cl and the repository/runtime include paths. Arguments are a **new** disposable output directory and a read-only `PLED.DAT`. Tests three independent masters, the fourth-player limit, library preservation, release/rehire, save-slot isolation and sidecar hash rejection. Refuses an existing output directory.
- `prep_edit3.py <bench_gamedata> [2|3]`: prepares only a save copy resolved below this repository's `logs/bench`. Two players leave room for a native third signing; three exercise save/reload and match use from an existing contracted EDIT player, preserving the roster count. It does not change `PLED.DAT`.
- `test_pending_cheats.c`: isolated guest-memory fixture for attendance/income arithmetic, youth multiplier saturation, training lock after a course clears, hired staff fields, and F6's user-team selection in an away match. It does not launch a game or write a save.
- `test_club_names.cpp`: isolated memory check; run once with `overlay` and once without arguments. Confirms stock full/short names are eligible for translated display, custom names are preserved, and localization never changes the saved name bytes.
- Text and menu corrections are reproducible with `tools/korean_refine.py` and `tools/korean_menu_assets.py` (Pillow for the menu). They start from the first overlay backups in `logs/korean_resume_backup`; the original Japanese assets remain unchanged.

2026-11-14 checks (training events, F6 super play, team color fit):

- `test_nov14_cheats.c`: host fixture on the real EBOOT tables and a save image (both read only): the training report records, the F6 hooks, the team color fit (auto, picked, held slot, restart, off = the save again). Build and run with `build_nov14_test.bat` (here and in `logs\`; it runs in the empty `logs\bench\fixture_cwd`, and the test refuses a folder with real cheats).
- `weekmatchbench.ps1`: the match route with its own cwd (`logs\bench\cwd_1114`, a copy of `cheats\`), a shot at every press between the office and the locker room (the week's events) and right after each forced super play; `-TeamColor` writes the copy's `teamcolor.txt`; use `-Env "SAKA_SUPERPLAY_AUTO=8"` to have F6 press itself.
- `officebench.ps1`: to the office, F2 on the 팀컬러 tab with one overlay shot (F2 is posted to the hidden window), then `-Steps "S;X;R;O"` (T triangle, O circle, X cross, S square, L/R/U/D) with a shot after each. The team color plates are on the 練習 screen (`R;O` from the office).
