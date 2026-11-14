# Builds a ready-to-play folder (and a zip of it) from this checkout, in dist\Sakatsuku_PC_<exe time>\:
#   Sakatsuku Launcher.exe, build-release\SakatsukuRecomp.exe + its DLLs, the shared cheat lists, README, LICENSE.
# The player's own files are left out: game\ (disc, update, firmware), gamedata\ (saves), launcher.ini and the personal
# cheat files (enabled/values/players/lists, live\, saves\).
#   -WithGame  also copies game\ (about 6.6 GB) and gamedata\, to move everything to another PC of your own. No zip then.
#   -NoZip     folder only.
# Running it again after a new build makes a new folder named after the new exe's time; older ones stay until removed.
param([switch]$WithGame, [switch]$NoZip)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSCommandPath)
$exe = Join-Path $root 'build-release\SakatsukuRecomp.exe'
$launcher = Join-Path $root 'Sakatsuku Launcher.exe'
foreach ($need in $exe, $launcher) {
    if (-not (Test-Path -LiteralPath $need)) { throw "$need 이(가) 없습니다. build_release.bat / build_launcher.bat로 먼저 빌드하세요." }
}

$stamp = (Get-Item -LiteralPath $exe).LastWriteTime.ToString('yyyyMMdd-HHmm')
$name = "Sakatsuku_PC_$stamp"
$dist = Join-Path $root 'dist'
$out = Join-Path $dist $name
if (Test-Path -LiteralPath $out) { Remove-Item -LiteralPath $out -Recurse -Force }   # our own output of the same exe
New-Item -ItemType Directory -Force (Join-Path $out 'build-release'), (Join-Path $out 'cheats') | Out-Null

Copy-Item -LiteralPath $launcher -Destination $out
Copy-Item -LiteralPath $exe -Destination (Join-Path $out 'build-release')
Get-ChildItem -LiteralPath (Join-Path $root 'build-release') -Filter *.dll -File | Copy-Item -Destination (Join-Path $out 'build-release')
foreach ($f in 'README.md', 'LICENSE') {   # no bat files: the launcher starts the game (owner, 2026-11-09)
    $p = Join-Path $root $f
    if (Test-Path -LiteralPath $p) { Copy-Item -LiteralPath $p -Destination $out }
}
# Shared cheat lists only; the personal ones are made by the launcher and the game.
$personal = 'enabled.txt', 'values.txt', 'players.txt', 'lists.txt', 'training.txt', 'staff.txt', 'teamcolor.txt', 'teamcolor_backup.txt'
Get-ChildItem -LiteralPath (Join-Path $root 'cheats') -Filter *.txt -File | Where-Object { $personal -notcontains $_.Name } |
    Copy-Item -Destination (Join-Path $out 'cheats')

if ($WithGame) {
    foreach ($d in 'game', 'gamedata') {
        $src = Join-Path $root $d
        if (Test-Path -LiteralPath $src) {
            & robocopy $src (Join-Path $out $d) /E /MT:8 /NFL /NDL /NJH /NJS /NP | Out-Null
            if ($LASTEXITCODE -ge 8) { throw "robocopy $d 실패 (코드 $LASTEXITCODE)" }
        }
    }
}

$guide = @"
사카츠쿠 PS3 PC 포트 ($name)

실행: "Sakatsuku Launcher.exe"

게임 파일은 들어 있지 않습니다. 이 폴더에 game 폴더를 만들고 본인 디스크의 파일을 넣으세요.
  game\EBOOT.ELF       1.03 업데이트의 복호화된 EBOOT
  game\dev_bdvd\       디스크 (PS3_GAME 등)
  game\dev_hdd0\       1.03 업데이트 (game\BLJM61064\...)
  game\firmware\       libsre_image.bin, libspurs_jq_image.bin (본인 펌웨어의 libsre/libspurs_jq를 복호화·재배치한 이미지, NOTES.md "Run 2–4")
세이브는 gamedata\ 에 생깁니다. 만드는 법은 README.md 와 NOTES.md (저장소)에 있습니다.
"@
if ($WithGame) { $guide = "사카츠쿠 PS3 PC 포트 ($name)`r`n`r`n실행: `"Sakatsuku Launcher.exe`"`r`ngame\ 와 gamedata\ (세이브) 포함본입니다. 본인 PC 이전용이며 다른 사람에게 배포하지 마세요." }
[IO.File]::WriteAllText((Join-Path $out '읽어주세요.txt'), $guide, (New-Object Text.UTF8Encoding($true)))

$zip = $null
if (-not $WithGame -and -not $NoZip) {
    $zip = "$out.zip"
    if (Test-Path -LiteralPath $zip) { Remove-Item -LiteralPath $zip -Force }
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    [IO.Compression.ZipFile]::CreateFromDirectory($out, $zip, [IO.Compression.CompressionLevel]::Optimal, $true)
}

$size = (Get-ChildItem -LiteralPath $out -Recurse -File | Measure-Object Length -Sum).Sum
"폴더: $out ({0:N0} MB)" -f ($size / 1MB)
if ($zip) { "zip : $zip ({0:N0} MB)" -f ((Get-Item -LiteralPath $zip).Length / 1MB) }
