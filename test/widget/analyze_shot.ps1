# analyze_shot.ps1 — 白色/蓝色/黑色像素行扫描 + 两帧对比
param(
    [string]$path = 'C:\Users\7052\Desktop\other\JZFramework\build\test\widget\Debug\ui_shot_001.png',
    [int]$col = 60
)
Add-Type -AssemblyName System.Drawing
$bmp = New-Object System.Drawing.Bitmap($path)
Write-Host "size: $($bmp.Width)x$($bmp.Height)  col=$col"

$start = -1; $kind = ''
for ($y = 0; $y -lt $bmp.Height; $y++) {
    $c = $bmp.GetPixel($col, $y)
    $k = ''
    if ($c.R -gt 230 -and $c.G -gt 230 -and $c.B -gt 230) { $k = 'WHITE' }
    elseif ([Math]::Abs($c.R - 51) -lt 35 -and [Math]::Abs($c.G - 102) -lt 35 -and [Math]::Abs($c.B - 204) -lt 45) { $k = 'BLUE' }
    elseif ($c.R -lt 70 -and $c.G -lt 70 -and $c.B -lt 80) { $k = 'DARK' }
    if ($k -ne $kind) {
        if ($kind -ne '' -and $start -ge 0 -and ($y - $start) -gt 1) {
            Write-Host "  $kind y=$start..$($y-1) (client logical $([int](($start-62)/2))..$([int]((($y-1)-62)/2)))"
        }
        $start = $y; $kind = $k
    }
}
$bmp.Dispose()
