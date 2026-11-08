# Quiet test run (hidden window, muted) of the Sakatsuku 1.03 recomp, then stop the exe.
# usage: powershell -File tools\quiet_run.ps1 -Tag run2 -Seconds 60 [-FrameEvery 60]
#        -Cap "200:8:400"  -> D3D12 backbuffer BMPs (RSX_CAP start:count:stride) in logs\cap_<Tag>\frame_NN
#        -Build build-release  -> run the Release exe;  -Threads -> per-thread CPU % over the last 5 s
param([string]$Tag = "run", [int]$Seconds = 60, [int]$FrameEvery = 60, [string]$Cap = "",
      [string]$Build = "build", [switch]$Threads)
$D = Split-Path -Parent $PSScriptRoot
$L = "$D\logs"; $F = "$L\frames_$Tag"
New-Item -ItemType Directory -Force $F | Out-Null
$env:PS3_VFS_ROOT = "$D\game\dev_bdvd"; $env:PS3_HDD0_ROOT = "$D\game\dev_hdd0"
$env:PS3_MUTE = "1"; $env:PS3_FSLOG = "1"
# Sakatsuku renders its frame into a 1280x720 surface at 0x00F00000, not into the
# registered display buffers (0x0 / 0x3C0000); treat display-sized surfaces as the backbuffer.
$env:RT_DISPLAY_BY_SIZE = "1"
$env:LD_FRAME_DUMP = $F; $env:LD_FRAME_DUMP_EVERY = "$FrameEvery"
if ($Cap) { $env:RSX_CAP = $Cap; $env:RSX_CAP_DIR = "$L\cap_$Tag" } else { Remove-Item Env:RSX_CAP -ErrorAction SilentlyContinue }
$p = Start-Process -FilePath "$D\$Build\SakatsukuRecomp.exe" -ArgumentList "`"$D\game\EBOOT.ELF`"" `
    -WorkingDirectory $D -WindowStyle Hidden -PassThru `
    -RedirectStandardOutput "$L\${Tag}_out.txt" -RedirectStandardError "$L\${Tag}_err.txt"
Add-Type @"
using System; using System.Runtime.InteropServices;
public class QRWin { public delegate bool P(IntPtr h, IntPtr l);
 [DllImport("user32.dll")] static extern bool EnumWindows(P f, IntPtr l);
 [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr h, out uint p);
 [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr h);
 public static int Visible(uint pid) { int n = 0; EnumWindows((h, l) => { uint q; GetWindowThreadProcessId(h, out q); if (q == pid && IsWindowVisible(h)) n++; return true; }, IntPtr.Zero); return n; } }
"@
$vis = 0; $t0 = Get-Date
while (((Get-Date) - $t0).TotalSeconds -lt $Seconds -and -not $p.HasExited) {
    Start-Sleep -Seconds 2; $vis = [Math]::Max($vis, [QRWin]::Visible([uint32]$p.Id))
}
$threadReport = @()
if ($Threads -and -not $p.HasExited) {
    $snap = { $h = @{}; (Get-Process -Id $p.Id).Threads | ForEach-Object { $h[$_.Id] = $_.TotalProcessorTime.TotalMilliseconds }; $h }
    $a = & $snap; Start-Sleep -Seconds 5; $b = & $snap
    $threadReport = $b.Keys | ForEach-Object { [pscustomobject]@{ Tid = $_; Pct = [math]::Round((($b[$_] - $a[$_]) / 5000) * 100, 1) } } |
        Where-Object Pct -gt 1 | Sort-Object Pct -Descending | Select-Object -First 12
}
$exited = $p.HasExited; $code = if ($exited) { $p.ExitCode } else { "running" }
if (-not $exited) { Stop-Process -Id $p.Id -Force; Start-Sleep -Seconds 1 }
$out = Get-Content "$L\${Tag}_out.txt" -ErrorAction SilentlyContinue
$err = Get-Content "$L\${Tag}_err.txt" -ErrorAction SilentlyContinue
"[$Tag] ran $([int]((Get-Date)-$t0).TotalSeconds)s, exited by itself: $exited (code $code), visible windows: $vis, stdout $($out.Count) / stderr $($err.Count) lines, frames $((Get-ChildItem $F -File).Count)"
if ($threadReport) {
    # Map host thread ids to guest thread names from the runtime's "[THREAD n] ... hosttid=" and sys_ppu_thread_create lines.
    $names = @{}; $all = @($out) + @($err)
    $all | Select-String 'sys_ppu_thread_create tid=(\d+) name="([^"]*)"' | ForEach-Object { $names["g" + $_.Matches[0].Groups[1].Value] = $_.Matches[0].Groups[2].Value }
    $all | Select-String '\[THREAD (\d+)\] host thread started.*hosttid=(\d+)' | ForEach-Object { $names["h" + $_.Matches[0].Groups[2].Value] = $names["g" + $_.Matches[0].Groups[1].Value] }
    "per-thread CPU (% of one core, last 5 s):"
    $threadReport | ForEach-Object { "  tid {0,6}  {1,6}%  {2}" -f $_.Tid, $_.Pct, $names["h$($_.Tid)"] }
}
if ($Cap -and (Test-Path "$L\cap_$Tag")) {
    $caps = Get-ChildItem "$L\cap_$Tag" -Directory | Sort-Object Name | ForEach-Object {
        [pscustomobject]@{ Frame = [int]((Get-Content "$($_.FullName)\manifest.txt" -TotalCount 1) -replace '^frame_count=(\d+).*', '$1'); Time = $_.CreationTime } }
    for ($i = 1; $i -lt $caps.Count; $i++) {
        $dt = ($caps[$i].Time - $caps[$i-1].Time).TotalSeconds
        if ($dt -gt 0) { "  frames {0}->{1}: {2:N1} fps" -f $caps[$i-1].Frame, $caps[$i].Frame, (($caps[$i].Frame - $caps[$i-1].Frame) / $dt) }
    }
}
