Add-Type -AssemblyName System.Drawing

$darkBg = [System.Drawing.Color]::FromArgb(15, 17, 23)
$cardBg = [System.Drawing.Color]::FromArgb(26, 31, 44)
$cyan = [System.Drawing.Color]::FromArgb(14, 165, 233)
$white = [System.Drawing.Color]::FromArgb(248, 250, 252)
$gray = [System.Drawing.Color]::FromArgb(148, 163, 184)

# 1. Wizard Large Left Banner (164 x 314)
$banner = New-Object System.Drawing.Bitmap 164, 314
$g = [System.Drawing.Graphics]::FromImage($banner)
$g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
$g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
$g.Clear($darkBg)

# Background subtle card highlight
$rect = New-Object System.Drawing.Rectangle 8, 8, 148, 298
$brush = New-Object System.Drawing.SolidBrush $cardBg
$g.FillRectangle($brush, $rect)
$brush.Dispose()

# Draw border
$pen = New-Object System.Drawing.Pen $cyan, 1.5
$g.DrawRectangle($pen, $rect)
$pen.Dispose()

# Draw Cat Camera Icon
if (Test-Path "resources\icon.png") {
    $iconImg = [System.Drawing.Image]::FromFile("resources\icon.png")
    $g.DrawImage($iconImg, 32, 28, 100, 100)
    $iconImg.Dispose()
}

# Typography using [System.Drawing.Font]::new
$titleFont = [System.Drawing.Font]::new("Segoe UI", [single]13.0, [System.Drawing.FontStyle]::Bold)
$subFont = [System.Drawing.Font]::new("Segoe UI", [single]8.5, [System.Drawing.FontStyle]::Regular)
$authorFont = [System.Drawing.Font]::new("Segoe UI", [single]7.5, [System.Drawing.FontStyle]::Italic)

$textBrush = New-Object System.Drawing.SolidBrush $white
$cyanBrush = New-Object System.Drawing.SolidBrush $cyan
$grayBrush = New-Object System.Drawing.SolidBrush $gray

$format = New-Object System.Drawing.StringFormat
$format.Alignment = [System.Drawing.StringAlignment]::Center

$g.DrawString("LifeScreen", $titleFont, $cyanBrush, [single]82.0, [single]142.0, $format)
$g.DrawString("Screen Recorder", $subFont, $textBrush, [single]82.0, [single]168.0, $format)
$g.DrawString("Zero-Copy GPU Engine", $authorFont, $grayBrush, [single]82.0, [single]188.0, $format)

$g.DrawString("Developed by:", $authorFont, $grayBrush, [single]82.0, [single]235.0, $format)
$g.DrawString("Baneronetwo", $subFont, $textBrush, [single]82.0, [single]252.0, $format)
$g.DrawString("(BANSAFAn)", $authorFont, $cyanBrush, [single]82.0, [single]270.0, $format)

$banner.Save("resources\installer_banner.bmp", [System.Drawing.Imaging.ImageFormat]::Bmp)

$titleFont.Dispose()
$subFont.Dispose()
$authorFont.Dispose()
$textBrush.Dispose()
$cyanBrush.Dispose()
$grayBrush.Dispose()
$g.Dispose()
$banner.Dispose()

# 2. Wizard Small Top-Right Icon (55 x 55)
$small = New-Object System.Drawing.Bitmap 55, 55
$g2 = [System.Drawing.Graphics]::FromImage($small)
$g2.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
$g2.Clear($darkBg)

if (Test-Path "resources\icon.png") {
    $iconImg2 = [System.Drawing.Image]::FromFile("resources\icon.png")
    $g2.DrawImage($iconImg2, 3, 3, 49, 49)
    $iconImg2.Dispose()
}

$small.Save("resources\installer_small.bmp", [System.Drawing.Imaging.ImageFormat]::Bmp)
$g2.Dispose()
$small.Dispose()

Write-Host "Installer assets generated successfully."
