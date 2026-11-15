param([string]$Tag = "b14b", [string]$Env = "", [string]$TeamColor = "on`t1`nmode`t0`ncolors`t0`t0`t0`nlevel`t7", [string]$Steps = "T;X;R;R;O;O;X;X")
# 2026-11-14: office screens with the team color fit on (hidden run on a COPY of the save, own cwd): the F2 팀컬러 tab
# (SAKA_OVERLAY_SHOT) and a shot after each step (T triangle, O circle, X cross, S square, L/R/U/D d-pad).
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
public static class W14b {
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc f, IntPtr l);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint msg, IntPtr w, IntPtr l);
  public static void Key(uint pid, int vk) { var r = new List<IntPtr>(); EnumWindows((h, l) => { uint p; GetWindowThreadProcessId(h, out p); if (p == pid) r.Add(h); return true; }, IntPtr.Zero);
    foreach (var h in r) { PostMessage(h, 0x100, (IntPtr)vk, IntPtr.Zero); PostMessage(h, 0x101, (IntPtr)vk, (IntPtr)unchecked((int)0xC0000001)); } }
}
"@
$p = Start-Process -FilePath "$D\build-release\SakatsukuRecomp.exe" -ArgumentList "`"$D\game\EBOOT.ELF`"" -WorkingDirectory $W -WindowStyle Hidden -PassThru -RedirectStandardOutput $out -RedirectStandardError $err
$t0 = Get-Date
function El { [Math]::Round(((Get-Date) - $t0).TotalSeconds, 1) }
function Say($m) { "{0,6}s  {1}" -f (El), $m }
function Press([string]$m) { Set-Content $pad $m; Start-Sleep -Milliseconds 700 }
function Seen([string]$pat) { (Select-String -Path $err, $out -Pattern $pat -SimpleMatch -Quiet -ErrorAction SilentlyContinue) -eq $true }
function WaitFor([string]$pat, [int]$sec) { $e = (El) + $sec; while ((El) -lt $e -and -not $p.HasExited) { if (Seen $pat) { return $true }; Start-Sleep -Milliseconds 300 }; return $false }
function Shot([string]$name) {
  Set-Content "$C\gate.txt" "1"; Start-Sleep -Milliseconds 450; Remove-Item "$C\gate.txt" -ErrorAction SilentlyContinue; Start-Sleep -Milliseconds 250
  $f = Get-ChildItem "$C\raw" -Filter "*_00F00000_1280x720.ppm" -ErrorAction SilentlyContinue | Sort-Object Name | Select-Object -Last 1
  if (-not $f) { $f = Get-ChildItem "$C\raw" -Filter "*_1280x720.ppm" -ErrorAction SilentlyContinue | Sort-Object Name | Select-Object -Last 1 }
  if ($f) { New-Item -ItemType Directory -Force "$C\one" | Out-Null; Get-ChildItem "$C\one" | Remove-Item -Force
    Copy-Item $f.FullName "$C\one\"; & "$D\.venv\Scripts\python.exe" -I "$D\tools\bench\ppm2bmp.py" "$C\one" "$C\bmp" | Out-Null
    $b = Get-ChildItem "$C\bmp" -Filter *.bmp | Sort-Object LastWriteTime | Select-Object -Last 1
    if ($b) { Add-Type -AssemblyName System.Drawing; $img = [System.Drawing.Image]::FromFile($b.FullName); $img.Save("$C\$name.jpg", [System.Drawing.Imaging.ImageFormat]::Jpeg); $img.Dispose(); Remove-Item $b.FullName } }
  Get-ChildItem "$C\raw" | Remove-Item -Force -ErrorAction SilentlyContinue
}
$bits = @{ T = "0x1000"; O = "0x2000"; X = "0x4000"; S = "0x8000"; L = "0x0080"; R = "0x0020"; U = "0x0010"; D = "0x0040" }
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
  Start-Sleep -Seconds 2
  Press "0x2000"; Start-Sleep -Seconds 2
  Press "0x0080 4"; Start-Sleep -Milliseconds 800; Press "0x2000"; Start-Sleep -Seconds 4
  for ($i = 0; $i -lt 8 -and -not (Seen "menu_level/mainmenu.arch"); $i++) { Press "0x2000"; Start-Sleep -Seconds 2 }
  if (-not (WaitFor "menu_level/mainmenu.arch" 20)) { Say "no office"; throw "stop" }
  Say "office"; Start-Sleep -Seconds 5; Press ""
  [W14b]::Key([uint32]$p.Id, 0x71); Say "F2"; Start-Sleep -Seconds 6
  [W14b]::Key([uint32]$p.Id, 0x71); Say ("F2 off; overlay shot: " + (Test-Path "$C\overlay_teamcolor.bmp")); Start-Sleep -Seconds 1
  $n = 0
  foreach ($s in ($Steps -split ";")) { $n++; Press ($bits[$s] + " 4"); Press ""; Start-Sleep -Seconds 3; Shot ("{0:00}_{1}" -f $n, $s); Say "step $n $s" }
} catch { Say ("stopped early: " + $_) } finally { if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }; Say "stopped" }
Select-String -Path $err -Pattern 'team color' | Select-Object -Last 4 | ForEach-Object { $_.Line.Substring(0, [Math]::Min(300, $_.Line.Length)) }
if (Test-Path "$C\overlay_teamcolor.bmp") { Add-Type -AssemblyName System.Drawing; $img = [System.Drawing.Image]::FromFile("$C\overlay_teamcolor.bmp"); $img.Save("$C\overlay_teamcolor.jpg", [System.Drawing.Imaging.ImageFormat]::Jpeg); $img.Dispose() }
