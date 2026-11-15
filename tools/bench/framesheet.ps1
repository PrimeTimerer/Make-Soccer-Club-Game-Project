param([string]$Raw, [string]$Out, [string]$Filter = "frame_*.ppm", [int]$X = 0, [int]$Y = 0, [int]$W = 1280, [int]$H = 720, [int]$TW = 640, [int]$Cols = 2, [int]$Max = 10)
# Contact sheet of consecutive display-surface dumps (f<frame>_..._00F00000_1280x720.ppm),
# each cropped to (X,Y,W,H), scaled to width TW and labelled with its frame number.
Add-Type -AssemblyName System.Drawing
$py = "$(Split-Path -Parent (Split-Path -Parent $PSScriptRoot))\.venv\Scripts\python.exe"
$S = $PSScriptRoot
$files = Get-ChildItem $Raw -Filter $Filter | Sort-Object Name | Select-Object -First $Max
if (-not $files) { "no frames"; exit 1 }
$tmp = Join-Path $env:TEMP ("fs_" + [guid]::NewGuid().ToString("N")); New-Item -ItemType Directory -Force "$tmp\in" | Out-Null
foreach ($f in $files) { Copy-Item $f.FullName "$tmp\in\" }
& $py -I "$PSScriptRoot\ppm2bmp.py" "$tmp\in" "$tmp\bmp" | Out-Null
$bmps = Get-ChildItem "$tmp\bmp" -Filter *.bmp | Sort-Object Name
$TH = [int]($TW * $H / $W)
$rows = [Math]::Ceiling($bmps.Count / $Cols)
$sheet = New-Object System.Drawing.Bitmap ($Cols * $TW), ($rows * $TH)
$g = [System.Drawing.Graphics]::FromImage($sheet)
$g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
$font = New-Object System.Drawing.Font 'Arial', 12, ([System.Drawing.FontStyle]::Bold)
for ($i = 0; $i -lt $bmps.Count; $i++) {
  $img = [System.Drawing.Image]::FromFile($bmps[$i].FullName)
  $dx = ($i % $Cols) * $TW; $dy = [Math]::Floor($i / $Cols) * $TH
  $g.DrawImage($img, (New-Object System.Drawing.Rectangle $dx, $dy, ($TW - 2), ($TH - 2)), (New-Object System.Drawing.Rectangle $X, $Y, $W, $H), [System.Drawing.GraphicsUnit]::Pixel)
  $img.Dispose()
  $label = $bmps[$i].BaseName
  $g.FillRectangle([System.Drawing.Brushes]::Black, $dx, $dy, 80, 20)
  $g.DrawString($label, $font, [System.Drawing.Brushes]::Yellow, $dx + 2, $dy + 1)
}
$g.Dispose(); $sheet.Save($Out, [System.Drawing.Imaging.ImageFormat]::Jpeg); $sheet.Dispose()
Remove-Item -Recurse -Force $tmp
"$($bmps.Count) frames -> $Out"
