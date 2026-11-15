param([string]$Tag = "fm1", [string]$Env = "", [int]$MatchSeconds = 200, [int]$MaxSeconds = 600, [switch]$Shots)
# Hidden, muted run on a COPY of the owner's save that drives the game into a match
# with closed-loop input (PAD_FILE), reading screen markers from the file-open log,
# then stays in the match for $MatchSeconds. Logs: logs\<Tag>_err/out.txt.
# -Shots: dump the display surface at each step (logs\<Tag>_shots\*.jpg).
foreach ($kv in ($Env -split ";")) { if ($kv -match "^([^=]+)=(.*)$") { [Environment]::SetEnvironmentVariable($Matches[1], $Matches[2], "Process") } }
$D = Split-Path -Parent (Split-Path -Parent $PSScriptRoot); $L = "$D\logs"; $S = "$L\bench"; New-Item -ItemType Directory -Force $S | Out-Null   # work files (save copy, pad file, gates) under logs\, not in the repository
$G = "$S\bench_gamedata"; if (Test-Path $G) { Remove-Item -Recurse -Force $G }; Copy-Item -Recurse "$D\gamedata" $G
$env:PS3_GAMEDATA_ROOT = $G
$env:PS3_VFS_ROOT = "$D\game\dev_bdvd"; $env:PS3_HDD0_ROOT = "$D\game\dev_hdd0"
$env:PS3_MUTE = "1"; $env:RT_DISPLAY_BY_SIZE = "1"; if (-not $env:RSX_LIVE_DRAW) { $env:RSX_LIVE_DRAW = "1" }
$env:PS3_FSLOG = "1"; $env:AUDIO_RATE = "1"; $env:PAD_NO_KEYBOARD = "1"
$pad = "$S\pad.txt"; Set-Content $pad ""; $env:PAD_FILE = $pad
$C = "$L\${Tag}_shots"; if (Test-Path $C) { Remove-Item -Recurse -Force $C }; New-Item -ItemType Directory -Force "$C\raw" | Out-Null
if ($Shots) { $env:LD_SURF_DUMP = "$C\raw"; $env:LD_SURF_DUMP_GATE = "$C\gate.txt" }
$err = "$L\${Tag}_err.txt"; $out = "$L\${Tag}_out.txt"
$p = Start-Process -FilePath "$D\build-release\SakatsukuRecomp.exe" -ArgumentList "`"$D\game\EBOOT.ELF`"" -WorkingDirectory $D -WindowStyle Hidden -PassThru -RedirectStandardOutput $out -RedirectStandardError $err
$t0 = Get-Date
function El { [Math]::Round(((Get-Date) - $t0).TotalSeconds, 1) }
function Say($m) { "{0,6}s  {1}" -f (El), $m }
function Press([string]$m) { Set-Content $pad $m; Start-Sleep -Milliseconds 700 }
function Seen([string]$pat) { (Select-String -Path $err, $out -Pattern $pat -SimpleMatch -Quiet -ErrorAction SilentlyContinue) -eq $true }
function WaitFor([string]$pat, [int]$sec) { $e = (El) + $sec; while ((El) -lt $e -and -not $p.HasExited) { if (Seen $pat) { return $true }; Start-Sleep -Milliseconds 300 }; return $false }
function Shot([string]$name) {
  if (-not $Shots) { Start-Sleep -Milliseconds 1500; return }   # menus need the time a shot takes
  Set-Content "$C\gate.txt" "1"; Start-Sleep -Milliseconds 500; Remove-Item "$C\gate.txt" -ErrorAction SilentlyContinue; Start-Sleep -Milliseconds 300
  $f = Get-ChildItem "$C\raw" -Filter "*_00F00000_1280x720.ppm" -ErrorAction SilentlyContinue | Sort-Object Name | Select-Object -Last 1
  if (-not $f) { $f = Get-ChildItem "$C\raw" -Filter "*_1280x720.ppm" -ErrorAction SilentlyContinue | Sort-Object Name | Select-Object -Last 1 }
  if ($f) { New-Item -ItemType Directory -Force "$C\one" | Out-Null; Get-ChildItem "$C\one" | Remove-Item -Force
    Copy-Item $f.FullName "$C\one\"; & "$D\.venv\Scripts\python.exe" -I "$PSScriptRoot\ppm2bmp.py" "$C\one" "$C\bmp" | Out-Null
    $b = Get-ChildItem "$C\bmp" -Filter *.bmp | Sort-Object LastWriteTime | Select-Object -Last 1
    if ($b) { Add-Type -AssemblyName System.Drawing; $img = [System.Drawing.Image]::FromFile($b.FullName); $sm = New-Object System.Drawing.Bitmap $img, 640, 360
      $sm.Save("$C\$name.jpg", [System.Drawing.Imaging.ImageFormat]::Jpeg); $sm.Dispose(); $img.Dispose(); Remove-Item $b.FullName } }
  Get-ChildItem "$C\raw" | Remove-Item -Force -ErrorAction SilentlyContinue
}
$steps = @()
try {
  if (-not (WaitFor "sb/top_menu.par" 120)) { Say "no title"; throw "stop" }
  Start-Sleep -Seconds 3; Say "title"; Shot "01_title"
  for ($try = 0; $try -lt 3 -and -not (Seen "sb/saveload.par"); $try++) {
    if ($try) { Say "title menu retry $try"; Press "0x4000"; Start-Sleep -Seconds 1; Press "0x4000"; Start-Sleep -Seconds 2 }
    Press "0x0008"; Start-Sleep -Seconds 2; Shot "02_after_start"
    Press "0x0040 4"; Start-Sleep -Milliseconds 800; Shot "03_after_down"; Press "0x2000"; Start-Sleep -Seconds 2; Shot "04_after_circle"
    Press "0x2000"; Start-Sleep -Seconds 2; Shot "04b_load"     # LOAD GAME -> [ロード | ダウンロード] -> ロード
    [void](WaitFor "sb/saveload.par" 6)
  }
  if (-not (WaitFor "sb/saveload.par" 4)) { Say "no load screen"; throw "stop" }
  Say "load screen"; Start-Sleep -Seconds 2; Shot "05_load_screen"
  Press "0x2000"; Start-Sleep -Seconds 2; Shot "06_after_slot"           # slot 01 -> "load? YES/NO" (NO preselected)
  Press "0x0080 4"; Start-Sleep -Milliseconds 800; Press "0x2000"; Start-Sleep -Seconds 4; Shot "06b_after_yes"
  for ($i = 0; $i -lt 8 -and -not (Seen "menu_level/mainmenu.arch"); $i++) { Press "0x2000"; Start-Sleep -Seconds 2 }
  if (-not (WaitFor "menu_level/mainmenu.arch" 20)) { Say "no office"; Shot "07_no_office"; throw "stop" }
  Say "office"; Start-Sleep -Seconds 3; Shot "08_office"
  Press "0x2000"; Start-Sleep -Seconds 3; Shot "09_after_office_circle"
  for ($i = 0; $i -lt 40 -and -not (Seen "menu_level/match_before_all.arch"); $i++) { Press "0x2000"; Start-Sleep -Milliseconds 1200 }
  if (-not (WaitFor "menu_level/match_before_all.arch" 10)) { Say "no locker room"; Shot "10_no_locker"; throw "stop" }
  Say "locker room"; Start-Sleep -Seconds 3; Shot "11_locker"
  Press "0x2000"; Start-Sleep -Seconds 2; Shot "12_after_start_match"   # 試合開始 -> [試合を見る | 結果を見る]
  Press "0x2000"; Start-Sleep -Seconds 2                                  # 試合を見る
  if (-not (WaitFor "3d/audience/audience.par" 120)) { Say "match did not load"; Shot "13_no_match"; throw "stop" }
  Say "match loading done"; $tm = El
  # The whole match: ○ ○ every 20 s resumes after half-time (and skips scenes); stop after the second half ends.
  $end = $tm + $MatchSeconds
  while ((El) -lt $end -and -not $p.HasExited) {
    Start-Sleep -Seconds 20; Press "0x2000"; Start-Sleep -Milliseconds 1500; Press "0x2000"
    if (Seen "sb/game_result_s") { }
  }
  Say "match measured"
} catch { } finally { if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }; Say "stopped" }
