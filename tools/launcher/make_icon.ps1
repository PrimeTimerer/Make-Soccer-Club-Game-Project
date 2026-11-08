# Draws the launcher's icon.ico (a football on a dark pitch-green tile): PNG frames of 256, 64, 48, 32 and 16 px in one
# .ico. Run once; build_launcher.bat uses the file it writes.
Add-Type -AssemblyName System.Drawing
$out = Join-Path $PSScriptRoot "icon.ico"

function Pentagon([double]$cx, [double]$cy, [double]$r, [double]$turn) {
    $points = New-Object 'System.Drawing.PointF[]' 5
    for ($i = 0; $i -lt 5; $i++) {
        $a = ($turn + $i * 72 - 90) * [Math]::PI / 180
        $points[$i] = New-Object System.Drawing.PointF ([float]($cx + $r * [Math]::Cos($a))), ([float]($cy + $r * [Math]::Sin($a)))
    }
    return , $points
}

function Frame([int]$size) {
    $bmp = New-Object System.Drawing.Bitmap $size, $size
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.ScaleTransform($size / 256.0, $size / 256.0)
    # Tile: rounded square, pitch green to night blue.
    $tile = New-Object System.Drawing.Drawing2D.GraphicsPath
    $r = 56
    $tile.AddArc(0, 0, $r, $r, 180, 90); $tile.AddArc(256 - $r, 0, $r, $r, 270, 90)
    $tile.AddArc(256 - $r, 256 - $r, $r, $r, 0, 90); $tile.AddArc(0, 256 - $r, $r, $r, 90, 90); $tile.CloseFigure()
    $bg = New-Object System.Drawing.Drawing2D.LinearGradientBrush (New-Object System.Drawing.Point 0, 0), (New-Object System.Drawing.Point 256, 256), ([System.Drawing.Color]::FromArgb(255, 16, 120, 88)), ([System.Drawing.Color]::FromArgb(255, 14, 22, 54))
    $g.FillPath($bg, $tile)
    # Pitch lines: halfway line and centre circle, faint.
    $line = New-Object System.Drawing.Pen ([System.Drawing.Color]::FromArgb(70, 255, 255, 255)), 6
    $g.DrawLine($line, 128, 8, 128, 248)
    # Ball.
    $ball = New-Object System.Drawing.Drawing2D.GraphicsPath
    $ball.AddEllipse(40, 40, 176, 176)
    $g.FillPath([System.Drawing.Brushes]::White, $ball)
    $g.SetClip($ball)
    $dark = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 20, 24, 36))
    $seam = New-Object System.Drawing.Pen ([System.Drawing.Color]::FromArgb(255, 20, 24, 36)), 7
    $center = Pentagon 128 128 34 0
    $g.FillPolygon($dark, $center)
    for ($i = 0; $i -lt 5; $i++) {
        $a = ($i * 72 - 90) * [Math]::PI / 180
        $x1 = 128 + 34 * [Math]::Cos($a); $y1 = 128 + 34 * [Math]::Sin($a)
        $x2 = 128 + 70 * [Math]::Cos($a); $y2 = 128 + 70 * [Math]::Sin($a)
        $g.DrawLine($seam, [float]$x1, [float]$y1, [float]$x2, [float]$y2)
        $g.FillPolygon($dark, (Pentagon (128 + 98 * [Math]::Cos($a)) (128 + 98 * [Math]::Sin($a)) 30 ($i * 72 + 36)))
    }
    $g.ResetClip()
    $rim = New-Object System.Drawing.Pen ([System.Drawing.Color]::FromArgb(255, 20, 24, 36)), 5
    $g.DrawEllipse($rim, 40, 40, 176, 176)
    $g.Dispose()
    $ms = New-Object System.IO.MemoryStream
    $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    return , $ms.ToArray()
}

$sizes = 256, 64, 48, 32, 16
$frames = @(); foreach ($s in $sizes) { $frames += , (Frame $s) }
$fs = [System.IO.File]::Create($out)
$w = New-Object System.IO.BinaryWriter $fs
$w.Write([uint16]0); $w.Write([uint16]1); $w.Write([uint16]$sizes.Count)
$offset = 6 + 16 * $sizes.Count
for ($i = 0; $i -lt $sizes.Count; $i++) {
    $s = $sizes[$i]; $len = $frames[$i].Length
    $w.Write([byte]($(if ($s -ge 256) { 0 } else { $s }))); $w.Write([byte]($(if ($s -ge 256) { 0 } else { $s })))
    $w.Write([byte]0); $w.Write([byte]0); $w.Write([uint16]1); $w.Write([uint16]32)
    $w.Write([uint32]$len); $w.Write([uint32]$offset); $offset += $len
}
foreach ($f in $frames) { $w.Write($f) }
$w.Dispose()
"wrote $out ($((Get-Item $out).Length) bytes)"
