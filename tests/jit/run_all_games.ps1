param(
    [ValidateRange(1, 100000)]
    [int]$Frames = 120,
    [ValidateSet('on', 'off')]
    [string]$Jit = 'on',
    [ValidateRange(0, 254)]
    [int]$FileIndex = 0,
    [string]$Output = 'build/all-games'
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$jitDirectory = Join-Path $root 'tests\jit'
$make = 'C:\Users\alvaj\CEdev\bin\make.exe'
$gccDirectory = 'C:\msys64\ucrt64\bin'
$env:PATH = "$gccDirectory;$env:PATH"
$env:Path = "$gccDirectory;$env:Path"

New-Item -ItemType Directory -Force -Path (Join-Path $jitDirectory $Output) | Out-Null
& $make -C $root -B
if ($LASTEXITCODE) { throw "release build failed with exit code $LASTEXITCODE" }
& $make -C $jitDirectory -f game.mk -B all EXEEXT=.all-games.exe "CC=$gccDirectory\gcc.exe"
if ($LASTEXITCODE) { throw "native game harness build failed with exit code $LASTEXITCODE" }

$media = Get-ChildItem -File (Join-Path $root 'tests') |
    Where-Object { $_.Extension -match '^\.(d64|t64)$' } |
    Sort-Object Name
if (!$media) { throw 'no D64 or T64 media found under tests' }

$results = foreach ($item in $media) {
    $stem = [IO.Path]::GetFileNameWithoutExtension($item.Name) -replace '[^A-Za-z0-9]+', '-'
    $prefix = Join-Path $jitDirectory (Join-Path $Output $stem)
    $log = "$prefix.log"
    $arguments = @((Join-Path $root 'bin\EMUDORE.obj'), $Frames, $Jit, $FileIndex, $prefix, 'none', 'none', $item.FullName)
    $ErrorActionPreference = 'Continue'
    Push-Location $jitDirectory
    try {
        $lines = & '.\game_test.all-games.exe' @arguments 2>&1
        $exitCode = $LASTEXITCODE
    }
    finally {
        Pop-Location
    }
    $ErrorActionPreference = 'Stop'
    $lines | Set-Content -Encoding ascii $log
    $result = $lines | Where-Object { $_ -match '^RESULT ' } | Select-Object -Last 1
    $throttle = $lines | Where-Object { $_ -match '^THROTTLE .*simulated_seconds=' } | Select-Object -Last 1
    $framesCompleted = 0
    $simulatedSeconds = 0.0
    $wallSeconds = 0.0
    $nativeHits = 0
    $loads = 0
    if ($result -match 'frames=(\d+).*native=(\d+).*loads=(\d+)') {
        $framesCompleted = [int]$Matches[1]
        $nativeHits = [int64]$Matches[2]
        $loads = [int]$Matches[3]
    }
    if ($throttle -match 'simulated_seconds=([0-9.]+).*wall_seconds=([0-9.]+)') {
        $simulatedSeconds = [double]$Matches[1]
        $wallSeconds = [double]$Matches[2]
    }
    [PSCustomObject]@{
        Game = $item.Name
        Status = if ($exitCode -eq 0 -and $framesCompleted -eq $Frames) { 'PASS' } else { 'FAIL' }
        Frames = $framesCompleted
        GuestFPS = if ($simulatedSeconds) { [math]::Round($framesCompleted / $simulatedSeconds, 2) } else { 0 }
        WallFPS = if ($wallSeconds) { [math]::Round($framesCompleted / $wallSeconds, 2) } else { 0 }
        NativeHits = $nativeHits
        Loads = $loads
        Log = $log
    }
}

$csv = Join-Path $jitDirectory (Join-Path $Output 'summary.csv')
$results | Export-Csv -NoTypeInformation -Encoding ascii $csv
Write-Output ''
Write-Output "ALL-GAMES PERFORMANCE SUMMARY (JIT=$Jit, frames=$Frames, throttle=135%)"
$results | Format-Table Game, Status, Frames, GuestFPS, WallFPS, NativeHits, Loads -AutoSize | Out-String | Write-Output
Write-Output "Summary CSV: $csv"
if ($results.Status -contains 'FAIL') { exit 1 }