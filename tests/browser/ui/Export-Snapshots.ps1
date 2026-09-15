$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$directory = Join-Path $PSScriptRoot 'snapshots'
$results = foreach ($name in 'all-files', 'launchers', 'last-page-255') {
    $source = Join-Path $directory ($name + '.bmp')
    $target = Join-Path $directory ($name + '.png')
    $image = New-Object System.Drawing.Bitmap $source
    try {
        if ($image.Width -ne 320 -or $image.Height -ne 240) {
            throw "Unexpected dimensions: $source"
        }
        $image.Save($target, [System.Drawing.Imaging.ImageFormat]::Png)
        [pscustomobject]@{
            Image = $target
            Dimensions = '320x240'
            Bytes = (Get-Item -LiteralPath $target).Length
            SHA256 = (Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash.ToLowerInvariant()
        }
    }
    finally {
        $image.Dispose()
    }
}
$results | Format-List