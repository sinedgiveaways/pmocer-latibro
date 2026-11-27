[CmdletBinding()]
param(
    [Parameter(Mandatory = $true, Position = 0)]
    [string] $LogPath,

    [switch] $AllThreads
)

# Read-only Windows PowerShell analyzer for Orbitals Together orbitals.log.
# Usage: .\Analyze-Timeout.ps1 .\logs\orbitals.log [-AllThreads]
$ErrorActionPreference = 'Stop'

try {
    $resolvedPath = (Resolve-Path -LiteralPath $LogPath).Path
    $lines = [System.IO.File]::ReadAllLines($resolvedPath)
} catch {
    Write-Error "Cannot read log '$LogPath': $($_.Exception.Message)"
    exit 2
}

# Keep the latest full fatal block. If the file contains only a filtered excerpt,
# analyze all available lines instead.
$fatalStart = -1
for ($i = 0; $i -lt $lines.Length; $i++) {
    if ($lines[$i] -match '^\s*FATAL: svc #0x26 Break at\b') { $fatalStart = $i }
}
$analysisLines = $lines
if ($fatalStart -ge 0) {
    $fatalEnd = $lines.Length
    for ($i = $fatalStart + 1; $i -lt $lines.Length; $i++) {
        if ($lines[$i] -match 'FATAL: svc #0x26 Break is not implemented') {
            $fatalEnd = $i + 1
            break
        }
    }
    $analysisLines = $lines[$fatalStart..($fatalEnd - 1)]
}

Write-Output 'Orbitals Together timeout analysis (Windows PowerShell; read-only)'
Write-Output "Log: $resolvedPath"

$fatal = @($analysisLines | Where-Object { $_ -match '^\s*FATAL: svc #0x26 Break at\b' } | Select-Object -Last 1)
if ($fatal.Count -gt 0) {
    Write-Output "`nFatal event:"
    Write-Output ('  ' + $fatal[0].Trim())
} elseif ($analysisLines -match 'GameThread timed out waiting') {
    Write-Output "`nTimeout text found in available lines; no complete svc Break block found."
} else {
    Write-Output "`nNo svc #0x26 Break signature found in the selected log region."
}

$timeoutLine = @($analysisLines | Where-Object { $_ -match 'GameThread timed out waiting for %s after' } | Select-Object -Last 1)
$targetLine = @($analysisLines | Where-Object { $_ -match 'sp\+0x[0-9a-fA-F]+ "RenderThread"' } | Select-Object -Last 1)
if ($timeoutLine.Count -gt 0) {
    Write-Output "`nTimeout evidence: GameThread's abort stack contains the Unreal timeout format string."
    if ($targetLine.Count -gt 0) { Write-Output '  The captured wait target string is RenderThread.' }
    Write-Output '  Note: the format string does not include the substituted timeout duration.'
}

$backendLines = @($lines | Where-Object { $_ -match '\[host\].*vk: using gpu' } | Select-Object -Last 2)
if ($backendLines.Count -gt 0) {
    Write-Output "`nVulkan backend evidence (from the log):"
    foreach ($line in $backendLines) { Write-Output ('  ' + $line.Trim()) }
} elseif ($lines -match 'ORBITALS_GPU\s*=\s*null') {
    Write-Output "`nFound a log line mentioning ORBITALS_GPU=null; confirm this is the current run's log."
} else {
    Write-Output "`nNo 'vk: using gpu' line found in the log. This alone does not prove which backend was selected."
}

$threads = New-Object System.Collections.Generic.List[object]
$current = $null
$inDump = $false
foreach ($line in $analysisLines) {
    if ($line -match 'every guest thread') { $inDump = $true; continue }
    if (-not $inDump) { continue }

    if ($line -match '^\s*thread\s+(0x[0-9a-fA-F]+)\s+"([^"]*)"\s+prio\s+(-?\d+)\s+core\s+(-?\d+)([^:]*):\s*(.*)$') {
        $current = [pscustomobject]@{
            Handle = $Matches[1]
            Name = $Matches[2]
            Priority = [int]$Matches[3]
            Core = [int]$Matches[4]
            Flags = $Matches[5].Trim()
            State = $Matches[6].Trim()
            Frames = New-Object System.Collections.Generic.List[string]
        }
        $threads.Add($current)
        continue
    }
    if ($null -ne $current -and $line -match '^\s*(?:guest )?frame\s+\d+:\s*(.*)$') {
        $current.Frames.Add($Matches[1].Trim())
        continue
    }
    if ($line -match '^\s*run time at the abort:') { break }
}

if ($threads.Count -gt 0) {
    $stateCounts = @{}
    foreach ($thread in $threads) {
        $state = if ($thread.State -match 'waiting on a condition variable') { 'condition-variable wait' }
                 elseif ($thread.State -match 'waiting on a mutex') { 'mutex wait' }
                 elseif ($thread.State -match 'waiting on an address') { 'address wait' }
                 elseif ($thread.State -match 'waiting on handles') { 'handle wait' }
                 elseif ($thread.State -match 'running or ready') { 'running or ready (snapshot label)' }
                 else { 'other' }
        if (-not $stateCounts.ContainsKey($state)) { $stateCounts[$state] = 0 }
        $stateCounts[$state]++
    }

    Write-Output "`nThread snapshot: $($threads.Count) guest threads"
    $countText = @($stateCounts.GetEnumerator() | Sort-Object Name | ForEach-Object { "$($_.Name): $($_.Value)" }) -join '; '
    Write-Output "  States: $countText"

    $selected = if ($AllThreads) { @($threads) } else {
        @($threads | Where-Object { $_.Name -match 'GameThread|RenderThread|RHIThread|RHIInterruptThread|RTHeartBeat|FHeartBeat|HttpManagerThread' })
    }
    if ($selected.Count -eq 0) {
        Write-Output '  No recognized game/render thread names found. Use -AllThreads or check that the full fatal dump is present.'
    } else {
        Write-Output "`nRelevant thread records (stacks are from the fatal snapshot):"
        foreach ($thread in $selected) {
            $suffix = if ($thread.Flags) { " $($thread.Flags)" } else { '' }
            Write-Output "  $($thread.Handle) `"$($thread.Name)`" prio $($thread.Priority) core $($thread.Core)$suffix`: $($thread.State)"
            $thread.Frames | Select-Object -First 8 | ForEach-Object { Write-Output "    $_" }
        }
    }

    $mutexWaiters = @($threads | Where-Object { $_.State -match 'waiting on a mutex' })
    if ($mutexWaiters.Count -gt 0) {
        Write-Output "`nMutex waiters:"
        foreach ($thread in $mutexWaiters) { Write-Output "  $($thread.Handle) `"$($thread.Name)`": $($thread.State)" }
    }
} else {
    Write-Output "`nNo complete thread dump parsed. Include the section beginning 'every guest thread'."
}

$runtime = @($analysisLines | Where-Object { $_ -match 'run time at the abort:' } | Select-Object -Last 1)
if ($runtime.Count -gt 0) { Write-Output "`n$($runtime[0].Trim())" }

Write-Output "`nInterpretation limits:"
Write-Output '  The fatal snapshot is taken after the GameThread enters the abort handler; it is not its pre-timeout wait stack.'
Write-Output '  "Running or ready" is a snapshot label, not proof the thread was making progress.'
Write-Output '  Idle worker waits are common. This report summarizes evidence; it does not determine root cause.'
