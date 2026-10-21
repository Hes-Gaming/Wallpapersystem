# Benchmarks WallpaperSystem.exe: runs it with --bench, samples its GPU engine
# utilization and prints the in-process "bench:" summary as an object.
# -StormRate N moves the mouse cursor N times per second in a small circle at the
# screen center (restored afterwards) to load the pause watcher.
param(
    [string]$Exe = "$PSScriptRoot\WallpaperSystem.exe",
    [int]$Seconds = 20,
    [int]$StormRate = 0,
    [string]$Screenshot = ''
)
$ErrorActionPreference = 'Stop'

Get-Process WallpaperSystem -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 500

$log = Join-Path (Split-Path $Exe) 'wallpaper.log'
$before = if (Test-Path $log) { @(Get-Content $log).Count } else { 0 }

$storm = $null
if ($StormRate -gt 0) {
    $storm = Start-Job -ArgumentList $StormRate, ($Seconds + 8) -ScriptBlock {
        param($rate, $duration)
        Add-Type -AssemblyName System.Windows.Forms
        $home0 = [Windows.Forms.Cursor]::Position
        $c = [Windows.Forms.Screen]::PrimaryScreen.Bounds
        $cx = $c.X + $c.Width / 2; $cy = $c.Y + $c.Height / 2
        $sw = [Diagnostics.Stopwatch]::StartNew(); $sent = 0
        while ($sw.Elapsed.TotalSeconds -lt $duration) {
            $due = [long]($sw.Elapsed.TotalSeconds * $rate)
            while ($sent -lt $due) {
                $a = $sent * 0.05
                [Windows.Forms.Cursor]::Position = [Drawing.Point]::new($cx + 40 * [math]::Cos($a), $cy + 40 * [math]::Sin($a))
                $sent++
            }
            Start-Sleep -Milliseconds 1
        }
        [Windows.Forms.Cursor]::Position = $home0
        $sent / $sw.Elapsed.TotalSeconds
    }
    Start-Sleep -Seconds 2
}

$p = Start-Process -FilePath $Exe -ArgumentList "--bench $Seconds" -WorkingDirectory $PSScriptRoot -PassThru
Start-Sleep -Seconds 3

if ($Screenshot) {
    Add-Type -AssemblyName System.Drawing, System.Windows.Forms
    $b = [Windows.Forms.Screen]::PrimaryScreen.Bounds
    $bmp = New-Object Drawing.Bitmap $b.Width, $b.Height
    [Drawing.Graphics]::FromImage($bmp).CopyFromScreen($b.Location, [Drawing.Point]::Empty, $b.Size)
    $bmp.Save($Screenshot)
    $bmp.Dispose()
}

$gpu = @{}
$luids = @{}
$counter = Get-Counter "\GPU Engine(pid_$($p.Id)_*)\Utilization Percentage" -SampleInterval 1 -MaxSamples ($Seconds - 3) -ErrorAction SilentlyContinue
$sampleCount = @($counter).Count
foreach ($set in $counter) {
    foreach ($s in $set.CounterSamples) {
        $type = ($s.InstanceName -replace '.*engtype_', '') -replace '\s+\d+$', ''
        $gpu[$type] = $gpu[$type] + $s.CookedValue
        $luid = $s.InstanceName -replace '^.*_luid_0x[0-9a-f]+_(0x[0-9a-f]+)_.*$', '$1'
        $luids[$luid] = $luids[$luid] + $s.CookedValue
    }
}

if (-not $p.WaitForExit(($Seconds + 30) * 1000)) { $p.Kill(); throw "bench run did not finish" }

$rate = 0
if ($storm) { $rate = [math]::Round((Receive-Job $storm -Wait -AutoRemoveJob), 0) }

$lines = Get-Content $log | Select-Object -Skip $before
$line = $lines | Where-Object { $_ -match 'bench: ' } | Select-Object -Last 1
if (-not $line) { throw "no bench line in $log" }
$adapter = ''; $deviceLuid = ''
if (($lines | Where-Object { $_ -match 'bench adapter: (.*) luid=(0x[0-9A-F]+)' } | Select-Object -Last 1)) { $adapter = $Matches[1]; $deviceLuid = $Matches[2].ToLower() }

$result = [ordered]@{ exe = Split-Path (Split-Path $Exe) -Leaf; storm_rate = $rate; adapter = $adapter }
foreach ($m in [regex]::Matches($line, '(\w+)=([\d.]+)')) { $result[$m.Groups[1].Value] = [double]$m.Groups[2].Value }
$total = 0
foreach ($k in $gpu.Keys) { $total += $gpu[$k] }
$result['gpu_total_pct'] = if ($sampleCount) { [math]::Round($total / $sampleCount, 2) } else { 0 }
$render = 0
foreach ($k in $luids.Keys) { if ($k -eq $deviceLuid) { $render += $luids[$k] } }
$result['gpu_render_adapter_pct'] = if ($sampleCount) { [math]::Round($render / $sampleCount, 2) } else { 0 }
$result['gpu_other_adapters_pct'] = if ($sampleCount) { [math]::Round(($total - $render) / $sampleCount, 2) } else { 0 }
foreach ($k in ($gpu.Keys | Sort-Object)) {
    if ($sampleCount -and $gpu[$k] -gt 0) { $result["gpu_$($k -replace ' ', '_')_pct"] = [math]::Round($gpu[$k] / $sampleCount, 2) }
}
[pscustomobject]$result
