param([string]$Tag = "b14", [string]$Env = "", [int]$MatchSeconds = 120, [string]$TeamColor = "on`t1`nmode`t0`ncolors`t0`t0`t0`nlevel`t7")
# 2026-11-14 check run (from tools\bench\matchbench.ps1): a hidden, muted run on a COPY of the save with its own cwd.
#  - at the office: F2 on the 팀컬러 tab, one overlay shot (SAKA_OVERLAY_SHOT), F2 again
#  - office -> locker room: a shot at every press (the week's events: training results)
#  - match: a shot right after each forced super play
foreach ($kv in ($Env -split ";")) { if ($kv -match "^([^=]+)=(.*)$") { [Environment]::SetEnvironmentVariable($Matches[1], $Matches[2], "Process") } }
$D = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path; $L = "$D\logs"; $S = "$L\bench"; New-Item -ItemType Directory -Force $S | Out-Null
$G = "$S\bench_gamedata"; if (Test-Path $G) { Remove-Item -Recurse -Force $G }; Copy-Item -Recurse "$D\gamedata" $G
$env:PS3_GAMEDATA_ROOT = $G
$env:PS3_VFS_ROOT = "$D\game\dev_bdvd"; $env:PS3_HDD0_ROOT = "$D\game\dev_hdd0"
$env:PS3_MUTE = "1"; $env:RT_DISPLAY_BY_SIZE = "1"; $env:RSX_LIVE_DRAW = "1"
$env:PS3_FSLOG = "1"; $env:AUDIO_RATE = "1"; $env:PAD_NO_KEYBOARD = "1"
$pad = "$S\pad.txt"; Set-Content $pad ""; $env:PAD_FILE = $pad
$C = "$L\${Tag}_shots"; if (Test-Path $C) { Remove-Item -Recurse -Force $C }; New-Item -ItemType Directory -Force "$C\raw" | Out-Null
$env:LD_SURF_DUMP = "$C\raw"; $env:LD_SURF_DUMP_GATE = "$C\gate.txt"
$env:SAKA_OVERLAY_SHOT = "$C\overlay_teamcolor.bmp"; $env:SAKA_OVERLAY_TAB = "5"
$err = "$L\${Tag}_err.txt"; $out = "$L\${Tag}_out.txt"
$W = "$S\cwd_1114"
if (Test-Path "$W\cheats") { Remove-Item -Recurse -Force "$W\cheats" }; New-Item -ItemType Directory -Force $W | Out-Null; Copy-Item -Recurse "$D\cheats" "$W\cheats"; Remove-Item -Recurse -Force "$W\cheats\live" -ErrorAction SilentlyContinue
foreach ($sub in @("game\firmware", "spu_jobs", "shader_cache_d3d12")) { if (-not (Test-Path "$W\$sub")) { New-Item -ItemType Directory -Force (Split-Path "$W\$sub") | Out-Null; Copy-Item -Recurse "$D\$sub" "$W\$sub" } }
if ($TeamColor) { New-Item -ItemType Directory -Force "$W\cheats\saves\MAIN00" | Out-Null; [IO.File]::WriteAllText("$W\cheats\saves\MAIN00\teamcolor.txt", $TeamColor + "`n") }
Add-Type @"
using System; using System.Runtime.InteropServices; using System.Collections.Generic;
public static class W14 {
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc f, IntPtr l);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint msg, IntPtr w, IntPtr l);
  [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassName(IntPtr h, System.Text.StringBuilder s, int n);
  public static List<IntPtr> Of(uint pid) { var r = new List<IntPtr>(); EnumWindows((h, l) => { uint p; GetWindowThreadProcessId(h, out p); if (p == pid) r.Add(h); return true; }, IntPtr.Zero); return r; }
  public static void Key(uint pid, int vk) { foreach (var h in Of(pid)) { PostMessage(h, 0x100, (IntPtr)vk, IntPtr.Zero); PostMessage(h, 0x101, (IntPtr)vk, (IntPtr)unchecked((int)0xC0000001)); } }
}
"@
$p = Start-Process -FilePath "$D\build-release\SakatsukuRecomp.exe" -ArgumentList "`"$D\game\EBOOT.ELF`"" -WorkingDirectory $W -WindowStyle Hidden -PassThru -RedirectStandardOutput $out -RedirectStandardError $err
$t0 = Get-Date
function El { [Math]::Round(((Get-Date) - $t0).TotalSeconds, 1) }
function Say($m) { "{0,6}s  {1}" -f (El), $m }
function Press([string]$m) { Set-Content $pad $m; Start-Sleep -Milliseconds 700 }
function Seen([string]$pat) { (Select-String -Path $err, $out -Pattern $pat -SimpleMatch -Quiet -ErrorAction SilentlyContinue) -eq $true }
function Count([string]$pat) { @(Select-String -Path $err -Pattern $pat -SimpleMatch -ErrorAction SilentlyContinue).Count }
function WaitFor([string]$pat, [int]$sec) { $e = (El) + $sec; while ((El) -lt $e -and -not $p.HasExited) { if (Seen $pat) { return $true }; Start-Sleep -Milliseconds 300 }; return $false }
function Shot([string]$name) {
  Set-Content "$C\gate.txt" "1"; Start-Sleep -Milliseconds 450; Remove-Item "$C\gate.txt" -ErrorAction SilentlyContinue; Start-Sleep -Milliseconds 250
  $f = Get-ChildItem "$C\raw" -Filter "*_00F00000_1280x720.ppm" -ErrorAction SilentlyContinue | Sort-Object Name | Select-Object -Last 1
  if (-not $f) { $f = Get-ChildItem "$C\raw" -Filter "*_1280x720.ppm" -ErrorAction SilentlyContinue | Sort-Object Name | Select-Object -Last 1 }
  if ($f) { New-Item -ItemType Directory -Force "$C\one" | Out-Null; Get-ChildItem "$C\one" | Remove-Item -Force
    Copy-Item $f.FullName "$C\one\"; & "$D\.venv\Scripts\python.exe" -I "$D\tools\bench\ppm2bmp.py" "$C\one" "$C\bmp" | Out-Null
    $b = Get-ChildItem "$C\bmp" -Filter *.bmp | Sort-Object LastWriteTime | Select-Object -Last 1
    if ($b) { Add-Type -AssemblyName System.Drawing; $img = [System.Drawing.Image]::FromFile($b.FullName); $sm = New-Object System.Drawing.Bitmap $img, 960, 540
      $sm.Save("$C\$name.jpg", [System.Drawing.Imaging.ImageFormat]::Jpeg); $sm.Dispose(); $img.Dispose(); Remove-Item $b.FullName } }
  Get-ChildItem "$C\raw" | Remove-Item -Force -ErrorAction SilentlyContinue
}
try {
  if (-not (WaitFor "sb/top_menu.par" 120)) { Say "no title"; throw "stop" }
  Start-Sleep -Seconds 3; Say "title"
  for ($try = 0; $try -lt 3 -and -not (Seen "sb/saveload.par"); $try++) {
    if ($try) { Press "0x4000"; Start-Sleep -Seconds 1; Press "0x4000"; Start-Sleep -Seconds 2 }
    Press "0x0008"; Start-Sleep -Seconds 2
    Press "0x0040 4"; Start-Sleep -Milliseconds 800; Press "0x2000"; Start-Sleep -Seconds 2
    Press "0x2000"; Start-Sleep -Seconds 2
    [void](WaitFor "sb/saveload.par" 6)
  }
  if (-not (WaitFor "sb/saveload.par" 4)) { Say "no load screen"; throw "stop" }
  Say "load screen"; Start-Sleep -Seconds 2
  Press "0x2000"; Start-Sleep -Seconds 2
  Press "0x0080 4"; Start-Sleep -Milliseconds 800; Press "0x2000"; Start-Sleep -Seconds 4
  for ($i = 0; $i -lt 8 -and -not (Seen "menu_level/mainmenu.arch"); $i++) { Press "0x2000"; Start-Sleep -Seconds 2 }
  if (-not (WaitFor "menu_level/mainmenu.arch" 20)) { Say "no office"; throw "stop" }
  Say "office"; Start-Sleep -Seconds 5; Press ""; Shot "01_office"
  [W14]::Key([uint32]$p.Id, 0x71); Say "F2 (overlay on the team color tab)"; Start-Sleep -Seconds 6
  [W14]::Key([uint32]$p.Id, 0x71); Say ("F2 off; overlay shot: " + (Test-Path "$C\overlay_teamcolor.bmp")); Start-Sleep -Seconds 1
  Shot "02_office_after"
  Press "0x2000"; Start-Sleep -Seconds 3; Shot "03_week_000"
  for ($i = 1; $i -lt 60 -and -not (Seen "menu_level/match_before_all.arch"); $i++) { Press "0x2000"; Start-Sleep -Milliseconds 900; Shot ("03_week_{0:000}" -f $i) }
  if (-not (WaitFor "menu_level/match_before_all.arch" 10)) { Say "no locker room"; throw "stop" }
  Say "locker room"; Start-Sleep -Seconds 3; Shot "04_locker"
  Press "0x2000"; Start-Sleep -Seconds 2
  Press "0x2000"; Start-Sleep -Seconds 2
  if (-not (WaitFor "3d/audience/audience.par" 120)) { Say "match did not load"; throw "stop" }
  Say "match loading done"; $tm = El; $seen = Count "super play: the user"
  while ((El) -lt $tm + $MatchSeconds -and -not $p.HasExited) {
    Start-Sleep -Milliseconds 250
    $n = Count "super play: the user"
    if ($n -gt $seen) { $seen = $n; Start-Sleep -Milliseconds 500; Shot ("05_super_{0:00}a" -f $n); Start-Sleep -Milliseconds 600; Shot ("05_super_{0:00}b" -f $n); Say "forced super play $n" }
    $pl = Select-String -Path $err -Pattern "^\[perf\] .*draws=(\d+)" -ErrorAction SilentlyContinue | Select-Object -Last 1
    if ($pl -and [int]$pl.Matches[0].Groups[1].Value -lt 600 -and (El) -gt $tm + 20) { Press "0x2000"; Press "" }
  }
  Say "match measured"
} catch { Say ("stopped early: " + $_) } finally { if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }; Say "stopped" }
"--- cheat lines"
Select-String -Path $err -Pattern 'team color|training reports|\[cheats\] super play: the user' | Select-Object -Last 12 | ForEach-Object { $_.Line.Substring(0, [Math]::Min(460, $_.Line.Length)) }
"armed: " + (Count "super play armed") + "  forced: " + (Count "super play: the user") + "  errors: " + (Count "DEVICE_HUNG") + "/" + (Count "Access violation")
"backup file: " + (Test-Path "$W\cheats\saves\MAIN00\teamcolor_backup.txt")
Get-ChildItem $C -Filter *.jpg | Measure-Object | ForEach-Object { "shots: " + $_.Count }
