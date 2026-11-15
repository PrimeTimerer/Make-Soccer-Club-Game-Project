param([int]$Seconds = 45, [string]$Tag = "snd1", [string]$Env = "")
# Hidden, muted run with the audio diagnostics on. Logs go to logs\<Tag>_out/err.txt,
# the final mix (f32le stereo 48 kHz) to the scratchpad.
foreach ($kv in ($Env -split ";")) { if ($kv -match "^([^=]+)=(.*)$") { [Environment]::SetEnvironmentVariable($Matches[1], $Matches[2], "Process") } }
$D = Split-Path -Parent (Split-Path -Parent $PSScriptRoot); $L = "$D\logs"
$env:PS3_VFS_ROOT = "$D\game\dev_bdvd"; $env:PS3_HDD0_ROOT = "$D\game\dev_hdd0"
$env:PS3_MUTE = "1"; $env:RT_DISPLAY_BY_SIZE = "1"; if (-not $env:RSX_LIVE_DRAW) { $env:RSX_LIVE_DRAW = "1" }
$env:AUDIO_PEAK = "1"; $env:AUDIO_RATE = "1"; $env:PS3_FSLOG = "1"
$env:AUDIO_WAV = "$L\${Tag}.f32"
$p = Start-Process -FilePath "$D\build-release\SakatsukuRecomp.exe" -ArgumentList "`"$D\game\EBOOT.ELF`"" -WorkingDirectory $D -WindowStyle Hidden -PassThru -RedirectStandardOutput "$L\${Tag}_out.txt" -RedirectStandardError "$L\${Tag}_err.txt"
$t0 = Get-Date
while (((Get-Date) - $t0).TotalSeconds -lt $Seconds -and -not $p.HasExited) { Start-Sleep -Seconds 1 }
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force; "stopped after $Seconds s" } else { "exited early code=$($p.ExitCode)" }
