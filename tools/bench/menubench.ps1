param([string]$Tag = "eb1", [string]$Env = "", [string]$Steps = "START;D;D;O", [int]$MaxSeconds = 240, [switch]$Shots,
      [string]$Exe = "build-release\SakatsukuRecomp.exe", [string]$Cwd = "", [string]$Prep = "")
# -Exe: the exe under the port; -Cwd: run in this folder with a fresh copy of cheats\ (the owner's running game keeps its own).
# Steps also: EV:<n> = write the EDIT points value n into <Cwd>\cheats\values.txt; REF = press 갱신 for it.
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
$W = $D; if ($Cwd) { $W = $Cwd; if (Test-Path "$W\cheats") { Remove-Item -Recurse -Force "$W\cheats" }; New-Item -ItemType Directory -Force $W | Out-Null; Copy-Item -Recurse "$D\cheats" "$W\cheats"; Remove-Item -Recurse -Force "$W\cheats\live" -ErrorAction SilentlyContinue; foreach ($sub in @("game\firmware", "spu_jobs", "shader_cache_d3d12")) { if (Test-Path "$W\$sub") { Remove-Item -Recurse -Force "$W\$sub" }; New-Item -ItemType Directory -Force (Split-Path "$W\$sub") | Out-Null; Copy-Item -Recurse "$D\$sub" "$W\$sub" } }
$edit = "에디트 선수 보너스 포인트 99 [코드 스위치] (EDIT 99)"; $refresh = 0
if ($Prep) { & $Prep $W }
$p = Start-Process -FilePath "$D\$Exe" -ArgumentList "`"$D\game\EBOOT.ELF`"" -WorkingDirectory $W -WindowStyle Hidden -PassThru -RedirectStandardOutput $out -RedirectStandardError $err
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
# Menu explorer: after the title, runs $Steps (";"-separated): START O X T S U D L R = buttons,
# W<n> = wait n s, SHOT:<name>, DUMP:<name> (memory dump through SAKA_MEM_DUMP_GATE). A shot follows every button.
$keys = @{ L3R3 = "0x0006"; START = "0x0008"; O = "0x2000"; X = "0x4000"; T = "0x1000"; S = "0x8000"; U = "0x0010 4"; D = "0x0040 4"; L = "0x0080 4"; R = "0x0020 4"; L1 = "0x0400"; R1 = "0x0800"; L2 = "0x0100"; R2 = "0x0200" }
$gate = "$S\memgate.txt"; Remove-Item $gate -ErrorAction SilentlyContinue
try {
  if (-not (WaitFor "sb/top_menu.par" 120)) { Say "no title"; throw "stop" }
  Start-Sleep -Seconds 3; Say "title"; Shot "00_title"
  $i = 0
  foreach ($st in ($Steps -split ";")) {
    $i++
    if ($p.HasExited) { Say "game exited"; break }
    if ($keys.ContainsKey($st)) { Press $keys[$st]; Start-Sleep -Milliseconds 1500; Shot ("{0:00}_{1}" -f $i, $st) }
    elseif ($st -match "^W(\d+)$") { Start-Sleep -Seconds ([int]$Matches[1]) }
    elseif ($st -match "^EV:(\d+)$") { $v = Get-Content "$W\cheats\values.txt" -Encoding UTF8 -ErrorAction SilentlyContinue | Where-Object { -not $_.StartsWith("$edit`t0`t") }; $v = @($v) + "$edit`t0`t$($Matches[1])"; [IO.File]::WriteAllLines("$W\cheats\values.txt", $v, [Text.UTF8Encoding]::new($false)); Start-Sleep -Seconds 3 }
    elseif ($st -match "^WRITE:([^=]+)=(.*)$") { $wp = Join-Path $W $Matches[1]; New-Item -ItemType Directory -Force (Split-Path $wp) | Out-Null; [IO.File]::WriteAllText($wp, ($Matches[2] -replace "\\t", "`t" -replace "\\n", "`r`n"), [Text.UTF8Encoding]::new($false)); Start-Sleep -Seconds 2 }
    elseif ($st -eq "REF") { $refresh++; $v = Get-Content "$W\cheats\values.txt" -Encoding UTF8 -ErrorAction SilentlyContinue | Where-Object { -not $_.StartsWith("$edit`trefresh`t") }; $v = @($v) + "$edit`trefresh`t$refresh"; [IO.File]::WriteAllLines("$W\cheats\values.txt", $v, [Text.UTF8Encoding]::new($false)); Start-Sleep -Seconds 3 }
    elseif ($st -match "^SHOT:(.+)$") { Shot ("{0:00}_{1}" -f $i, $Matches[1]) }
    elseif ($st -match "^DUMP:(.+)$") { Set-Content $gate $Matches[1]; for ($k = 0; $k -lt 40 -and (Test-Path $gate); $k++) { Start-Sleep -Milliseconds 500 }; Start-Sleep -Seconds 4; Say "dump $($Matches[1])" }
    Say "step $i $st"
  }
} catch { } finally { if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }; Say "stopped" }
