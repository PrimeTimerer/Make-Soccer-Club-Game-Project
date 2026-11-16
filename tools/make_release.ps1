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

# The newer of the game exe and the launcher: a launcher-only release still gets a newer version (the launcher's
# update check compares these stamps).
$stamp = (@((Get-Item -LiteralPath $exe).LastWriteTime, (Get-Item -LiteralPath $launcher).LastWriteTime) | Measure-Object -Maximum).Maximum.ToString('yyyyMMdd-HHmm')
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

게임 파일은 들어 있지 않습니다. 본인의 것으로 런처의 '게임 파일' 페이지가 자동으로 준비합니다.

필요한 것
  - サカつく 디스크 (BLJM61064): 디스크 폴더(PS3_GAME) 또는 복호화된 ISO
  - 1.03 업데이트 PKG
  - PS3 펌웨어 (PS3UPDAT.PUP, PlayStation 공식 사이트의 PS3 시스템 소프트웨어 업데이트)
  - RPCS3 (rpcs3.net): 암호화된 EBOOT와 펌웨어를 복호화하는 데 씁니다

순서
  1. RPCS3에 펌웨어와 1.03 업데이트를 설치하고, 디스크를 RPCS3 게임 목록에 추가합니다.
     (런처의 '게임 파일' 페이지에도 펌웨어·업데이트 설치 버튼과 디스크 폴더·ISO 고르기가 있습니다.)
  2. "Sakatsuku Launcher.exe"를 열고 '게임 파일' 페이지에서 '자동 준비'를 누릅니다.
     런처가 RPCS3를 찾아 복호화하고, 결과를 확인해 game 폴더에 넣습니다. RPCS3의 파일은 바꾸지 않습니다.
  3. 홈에서 플레이.

세이브는 gamedata\ 에 생깁니다.
새 버전: 런처의 '도구' > '포트 업데이트'에서 받습니다 (game, gamedata, 설정, 내 치트 설정은 그대로).
"@
if ($WithGame) { $guide = "사카츠쿠 PS3 PC 포트 ($name)`r`n`r`n실행: `"Sakatsuku Launcher.exe`"`r`ngame\ 와 gamedata\ (세이브) 포함본입니다. 본인 PC 이전용이며 다른 사람에게 배포하지 마세요." }
[IO.File]::WriteAllText((Join-Path $out '읽어주세요.txt'), $guide, (New-Object Text.UTF8Encoding($true)))
# The launcher's update check compares this with the newest release's zip name (Sakatsuku_PC_<stamp>.zip).
[IO.File]::WriteAllText((Join-Path $out 'version.txt'), $stamp, (New-Object Text.UTF8Encoding($false)))

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
