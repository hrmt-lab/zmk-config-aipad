param(
    [string]$RepositoryRoot = (Split-Path -Parent $PSScriptRoot)
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$sourcePath = Join-Path $RepositoryRoot 'assets/codex_icon_transparent.png'
$outputPath = Join-Path $RepositoryRoot 'boards/shields/aipad/src/codex_logo.c'

if (-not (Test-Path -LiteralPath $sourcePath -PathType Leaf)) {
    throw "Logo source not found: $sourcePath"
}

$source = [System.Drawing.Bitmap]::new($sourcePath)
$bitmap = [System.Drawing.Bitmap]::new(96, 96, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
$graphics = [System.Drawing.Graphics]::FromImage($bitmap)

try {
    $graphics.Clear([System.Drawing.Color]::Black)
    $graphics.CompositingMode = [System.Drawing.Drawing2D.CompositingMode]::SourceOver
    $graphics.CompositingQuality = [System.Drawing.Drawing2D.CompositingQuality]::HighQuality
    $graphics.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $graphics.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
    $graphics.DrawImage($source, [System.Drawing.Rectangle]::new(0, 0, 96, 96))

    $bytes = [System.Collections.Generic.List[byte]]::new(18432)
    for ($y = 0; $y -lt 96; $y++) {
        for ($x = 0; $x -lt 96; $x++) {
            # The ScreenKey's physical upright orientation is 90 degrees counter-clockwise
            # from the source asset. Map each output pixel back to the scaled source image.
            $pixel = $bitmap.GetPixel(95 - $y, $x)
            $red = [int]$pixel.R
            $green = [int]$pixel.G
            $blue = [int]$pixel.B
            $rgb565 = (($red -shr 3) -shl 11) -bor (($green -shr 2) -shl 5) -bor ($blue -shr 3)
            $bytes.Add([byte]($rgb565 -band 0xff))
            $bytes.Add([byte](($rgb565 -shr 8) -band 0xff))
        }
    }

    $lines = [System.Collections.Generic.List[string]]::new()
    $lines.Add('#include "codex_logo.h"')
    $lines.Add('')
    $lines.Add('static const uint8_t codex_logo_map[] = {')
    for ($offset = 0; $offset -lt $bytes.Count; $offset += 16) {
        $last = [Math]::Min($offset + 15, $bytes.Count - 1)
        $chunk = $bytes[$offset..$last] | ForEach-Object { '0x{0:x2}' -f $_ }
        $lines.Add('    ' + ($chunk -join ', ') + ',')
    }
    $lines.Add('};')
    $lines.Add('')
    $lines.Add('const lv_image_dsc_t screenkey_codex_logo = {')
    $lines.Add('    .header.magic = LV_IMAGE_HEADER_MAGIC,')
    $lines.Add('    .header.cf = LV_COLOR_FORMAT_RGB565,')
    $lines.Add('    .header.flags = 0,')
    $lines.Add('    .header.w = 96,')
    $lines.Add('    .header.h = 96,')
    $lines.Add('    .header.stride = 192,')
    $lines.Add('    .header.reserved_2 = 0,')
    $lines.Add('    .data_size = sizeof(codex_logo_map),')
    $lines.Add('    .data = codex_logo_map,')
    $lines.Add('};')

    $content = ($lines -join "`n") + "`n"
    [System.IO.File]::WriteAllText($outputPath, $content, [System.Text.UTF8Encoding]::new($false))
} finally {
    $graphics.Dispose()
    $bitmap.Dispose()
    $source.Dispose()
}

Write-Output "Generated: $outputPath"
