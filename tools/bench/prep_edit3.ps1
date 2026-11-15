param([string]$WorkingCopy)
# Only the bench's disposable save copy may be changed.
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$copy = [IO.Path]::GetFullPath($env:PS3_GAMEDATA_ROOT)
$bench = [IO.Path]::GetFullPath((Join-Path $repo 'logs\bench')) + '\'
if (-not $copy.StartsWith($bench, [StringComparison]::OrdinalIgnoreCase)) { throw 'EDIT fixture must be inside logs\bench' }
& "$repo\.venv\Scripts\python.exe" -X utf8 "$PSScriptRoot\prep_edit3.py" $copy
if ($LASTEXITCODE) { throw 'EDIT fixture preparation failed' }
