param(
    [string]$LogsRoot = "build\meteor\native\Windows\bin\UserData\Projects\meteor\Logs",
    [string]$Output = "build\meteor\missing-target-report.txt"
)

$ErrorActionPreference = 'Stop'

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$logsPath = if ([IO.Path]::IsPathRooted($LogsRoot)) { $LogsRoot } else { Join-Path $repoRoot $LogsRoot }
$outputPath = if ([IO.Path]::IsPathRooted($Output)) { $Output } else { Join-Path $repoRoot $Output }

if (-not (Test-Path -LiteralPath $logsPath)) {
    throw "Meteor log root does not exist: $logsPath"
}

$translated = @{}
$metadataPath = Join-Path $repoRoot 'build\meteor\generated\base_translation_output.json'
if (Test-Path -LiteralPath $metadataPath) {
    $metadata = Get-Content -LiteralPath $metadataPath -Raw | ConvertFrom-Json
    foreach ($function in $metadata.functions) {
        $translated[[uint32]$function.entryPoint] = $true
    }
}

$rows = New-Object System.Collections.Generic.List[object]
$files = Get-ChildItem -LiteralPath $logsPath -Recurse -File -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -match '^crash_missing_(?:jump_)?target\.txt$' }

foreach ($file in $files) {
    $text = Get-Content -LiteralPath $file.FullName -Raw -ErrorAction SilentlyContinue
    if ([string]::IsNullOrWhiteSpace($text)) { continue }

    $targetMatch = [regex]::Match($text, '(?im)(?:missing (?:jump )?target|target)\s*(?:=|:)?\s*0x([0-9a-f]{8})')
    if (-not $targetMatch.Success) {
        $targetMatch = [regex]::Match($text, '(?im)InvokeIndirect(?:Cpu|Jump):\s*target\s+0x([0-9a-f]{8})')
    }
    if (-not $targetMatch.Success) { continue }

    $targetHex = $targetMatch.Groups[1].Value.ToUpperInvariant()
    $target = [Convert]::ToUInt32($targetHex, 16)

    $lrMatch = [regex]::Match($text, '(?im)\bLR\s*(?:=|:)\s*0x([0-9a-f]{8})')
    $callerMatch = [regex]::Match($text, '(?im)\b(?:caller|function)\s*(?:=|:)?\s*(func_[0-9a-f]{8})')
    $pidMatch = [regex]::Match($file.Directory.Name, 'pid(\d+)', 'IgnoreCase')

    $rows.Add([pscustomobject]@{
        Target = ('0x' + $targetHex)
        TranslatedNow = $translated.ContainsKey($target)
        LR = if ($lrMatch.Success) { '0x' + $lrMatch.Groups[1].Value.ToUpperInvariant() } else { '' }
        Caller = if ($callerMatch.Success) { $callerMatch.Groups[1].Value } else { '' }
        PID = if ($pidMatch.Success) { [int]$pidMatch.Groups[1].Value } else { 0 }
        Log = $file.Directory.Name
        File = $file.FullName
        LastWriteTime = $file.LastWriteTime
    })
}

$grouped = $rows |
    Group-Object Target |
    ForEach-Object {
        $latest = $_.Group | Sort-Object LastWriteTime -Descending | Select-Object -First 1
        [pscustomobject]@{
            Target = $_.Name
            Hits = $_.Count
            TranslatedNow = [bool]$latest.TranslatedNow
            LatestLR = $latest.LR
            LatestCaller = $latest.Caller
            LatestPID = $latest.PID
            LatestLog = $latest.Log
            LatestTime = $latest.LastWriteTime
        }
    } |
    Sort-Object @{Expression='TranslatedNow';Descending=$false}, @{Expression='Hits';Descending=$true}, Target

$unresolved = @($grouped | Where-Object { -not $_.TranslatedNow })
$resolved = @($grouped | Where-Object { $_.TranslatedNow })

$out = New-Object System.Collections.Generic.List[string]
$out.Add('Meteor missing-target report')
$out.Add(('Generated: ' + (Get-Date).ToString('yyyy-MM-dd HH:mm:ss')))
$out.Add(('Crash files scanned: ' + $files.Count))
$out.Add(('Unique targets: ' + @($grouped).Count))
$out.Add(('Already translated now: ' + $resolved.Count))
$out.Add(('Still unresolved: ' + $unresolved.Count))
$out.Add('')
$out.Add('UNRESOLVED')
foreach ($row in $unresolved) {
    $out.Add(('{0} hits={1} lr={2} caller={3} pid={4} log={5}' -f
        $row.Target, $row.Hits, $row.LatestLR, $row.LatestCaller, $row.LatestPID, $row.LatestLog))
}
$out.Add('')
$out.Add('RESOLVED BY CURRENT GRAPH')
foreach ($row in $resolved) {
    $out.Add(('{0} hits={1} latest={2}' -f $row.Target, $row.Hits, $row.LatestLog))
}

$parent = Split-Path -Parent $outputPath
if ($parent -and -not (Test-Path -LiteralPath $parent)) {
    New-Item -ItemType Directory -Force -Path $parent | Out-Null
}
[IO.File]::WriteAllLines($outputPath, $out, (New-Object Text.UTF8Encoding($false)))

$out | Select-Object -First ([Math]::Min(30, $out.Count))
Write-Output "Full report: $outputPath"
