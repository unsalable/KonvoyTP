# KonvoyTP uygulama simgesini (app\KonvoyTP.ico) uretir: kehribar renkli yuvarlak kare + beyaz tir.
# Windows PowerShell 5.1 (System.Drawing). Bir kez calistirilir; .ico depoda durur.
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$out = Join-Path $root 'app\KonvoyTP.ico'

function RoundRect([System.Drawing.Drawing2D.GraphicsPath]$p, [float]$x, [float]$y, [float]$w, [float]$h, [float]$r) {
  $d = 2 * $r
  $p.AddArc($x, $y, $d, $d, 180, 90)
  $p.AddArc($x + $w - $d, $y, $d, $d, 270, 90)
  $p.AddArc($x + $w - $d, $y + $h - $d, $d, $d, 0, 90)
  $p.AddArc($x, $y + $h - $d, $d, $d, 90, 90)
  $p.CloseFigure()
}

function Render([int]$s) {
  $bmp = New-Object System.Drawing.Bitmap $s, $s, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
  $g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
  $g.Clear([System.Drawing.Color]::Transparent)
  $k = $s / 256.0
  $g.ScaleTransform($k, $k)

  # background: rounded square with a soft amber gradient
  $bg = New-Object System.Drawing.Drawing2D.GraphicsPath
  RoundRect $bg 8 8 240 240 58
  $grad = New-Object System.Drawing.Drawing2D.LinearGradientBrush ((New-Object System.Drawing.PointF 0, 8), (New-Object System.Drawing.PointF 0, 248), ([System.Drawing.Color]::FromArgb(255, 255, 184, 77)), ([System.Drawing.Color]::FromArgb(255, 255, 140, 0)))
  $g.FillPath($grad, $bg)

  $white = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::White)
  $amber = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 255, 159, 10))

  # trailer
  $tr = New-Object System.Drawing.Drawing2D.GraphicsPath
  RoundRect $tr 36 76 116 80 10
  $g.FillPath($white, $tr)
  # cab
  $cab = New-Object System.Drawing.Drawing2D.GraphicsPath
  $cab.AddPolygon([System.Drawing.PointF[]]@((New-Object System.Drawing.PointF 158, 96), (New-Object System.Drawing.PointF 196, 96), (New-Object System.Drawing.PointF 222, 126), (New-Object System.Drawing.PointF 222, 156), (New-Object System.Drawing.PointF 158, 156)))
  $g.FillPath($white, $cab)
  # cab window
  $win = New-Object System.Drawing.Drawing2D.GraphicsPath
  $win.AddPolygon([System.Drawing.PointF[]]@((New-Object System.Drawing.PointF 168, 106), (New-Object System.Drawing.PointF 191, 106), (New-Object System.Drawing.PointF 209, 127), (New-Object System.Drawing.PointF 168, 127)))
  $g.FillPath($amber, $win)
  # chassis
  $g.FillRectangle($white, 36, 150, 186, 12)
  # wheels
  foreach ($cx in 72, 120, 194) {
    $g.FillEllipse($white, $cx - 19, 156, 38, 38)
    $g.FillEllipse($amber, $cx - 7, 168, 14, 14)
  }
  $g.Dispose()
  $ms = New-Object System.IO.MemoryStream
  $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
  $bmp.Dispose()
  return ,$ms.ToArray()
}

$sizes = 16, 20, 24, 32, 40, 48, 64, 128, 256
$pngs = @()
foreach ($s in $sizes) { $pngs += ,(Render $s) }

$fs = New-Object System.IO.MemoryStream
$bw = New-Object System.IO.BinaryWriter $fs
$bw.Write([UInt16]0); $bw.Write([UInt16]1); $bw.Write([UInt16]$sizes.Count)
$offset = 6 + 16 * $sizes.Count
for ($i = 0; $i -lt $sizes.Count; $i++) {
  $s = $sizes[$i]
  $b = if ($s -ge 256) { 0 } else { $s }
  $bw.Write([byte]$b); $bw.Write([byte]$b); $bw.Write([byte]0); $bw.Write([byte]0)
  $bw.Write([UInt16]1); $bw.Write([UInt16]32)
  $bw.Write([UInt32]$pngs[$i].Length); $bw.Write([UInt32]$offset)
  $offset += $pngs[$i].Length
}
foreach ($p in $pngs) { $bw.Write($p) }
$bw.Flush()
[IO.File]::WriteAllBytes($out, $fs.ToArray())
Write-Host "Simge hazir: $out ($($fs.Length) bayt)"

# preview png for checking
[IO.File]::WriteAllBytes((Join-Path $root 'build\icon_preview.png'), $pngs[-1])
