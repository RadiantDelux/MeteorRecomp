param(
    [int]$Threads = 12,
    [switch]$SkipBuild
)

$ErrorActionPreference = 'Stop'

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$projectDir = Join-Path $repoRoot 'projects\meteor'
$manifest = Join-Path $projectDir 'recomp.yml'
$auditManifest = Join-Path $projectDir '.recomp_root_audit.generated.yml'
$translatorProject = Join-Path $repoRoot 'translator\src\Translator.Cli\Translator.Cli.csproj'
$translatorDll = Join-Path $repoRoot 'translator\src\Translator.Cli\bin\Release\net8.0\Translator.Cli.dll'
$productionMetadata = Join-Path $repoRoot 'build\meteor\generated\base_translation_output.json'
$auditRoot = Join-Path $repoRoot 'build\meteor\root-audit'
$auditFunctions = Join-Path $auditRoot 'functions'
$auditMetadata = Join-Path $auditRoot 'base_translation_output.json'
$auditLog = Join-Path $auditRoot 'translate.log'
$deltaReport = Join-Path $auditRoot 'discovery_delta.txt'

New-Item -ItemType Directory -Force -Path $auditRoot | Out-Null

function Invoke-Checked {
    param(
        [Parameter(Mandatory = $true)][string]$FilePath,
        [Parameter(Mandatory = $true)][string[]]$Arguments
    )

    & $FilePath @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "Command failed with exit code ${LASTEXITCODE}: $FilePath $($Arguments -join ' ')"
    }
}

try {
    $manifestText = [System.IO.File]::ReadAllText($manifest)
    if ($manifestText -notmatch 'discover_v_table_functions:\s*false' -and
        $manifestText -notmatch 'discover_v_table_functions:\s*true') {
        throw 'Meteor manifest does not contain discover_v_table_functions.'
    }
    if ($manifestText -notmatch 'discover_static_callbacks:\s*false' -and
        $manifestText -notmatch 'discover_static_callbacks:\s*true') {
        throw 'Meteor manifest does not contain discover_static_callbacks.'
    }

    $manifestText = [regex]::Replace(
        $manifestText,
        'discover_v_table_functions:\s*(?:false|true)',
        'discover_v_table_functions: true')
    $manifestText = [regex]::Replace(
        $manifestText,
        'discover_static_callbacks:\s*(?:false|true)',
        'discover_static_callbacks: true')
    [System.IO.File]::WriteAllText(
        $auditManifest,
        $manifestText,
        (New-Object System.Text.UTF8Encoding($false)))

    Push-Location $repoRoot
    try {
        if (-not $SkipBuild) {
            Invoke-Checked dotnet @('build', $translatorProject, '-c', 'Release')
        }

        $translateArgs = @(
            $translatorDll,
            'translate-recursive',
            '0x8000403C',
            '--project', $auditManifest,
            '--outdir', $auditFunctions,
            '--output-metadata', $auditMetadata,
            '--threads', [string]$Threads,
            '--prune-stale'
        )

        & dotnet @translateArgs 2>&1 | Tee-Object -FilePath $auditLog
        if ($LASTEXITCODE -ne 0) {
            throw "Root-audit translation failed with exit code $LASTEXITCODE."
        }
    }
    finally {
        Pop-Location
    }

    $audit = Get-Content $auditMetadata -Raw | ConvertFrom-Json
    if ($audit.quality.unsupportedInstructionCount -ne 0 -or
        $audit.quality.invalidSsaFunctionCount -ne 0) {
        throw "Audit output is not clean: unsupported=$($audit.quality.unsupportedInstructionCount), invalidSsa=$($audit.quality.invalidSsaFunctionCount)."
    }

    $productionFunctions = @()
    if (Test-Path $productionMetadata) {
        $production = Get-Content $productionMetadata -Raw | ConvertFrom-Json
        $productionFunctions = @($production.functions)
    }

    $productionEntries = @{}
    $productionLocalLabels = @{}
    foreach ($function in $productionFunctions) {
        $productionEntries[[uint32]$function.entryPoint] = $true
        foreach ($label in $function.localLabelAddresses) {
            $productionLocalLabels[[uint32]$label] = $true
        }
    }

    $newFunctions = @(
        $audit.functions |
            Where-Object { -not $productionEntries.ContainsKey([uint32]$_.entryPoint) } |
            Sort-Object entryPoint)
    $newInteriorFunctions = @($newFunctions | Where-Object { $_.interiorToOtherTranslation })
    $newFormerLocalLabels = @(
        $newFunctions |
            Where-Object { $productionLocalLabels.ContainsKey([uint32]$_.entryPoint) })

    $report = New-Object System.Collections.Generic.List[string]
    $report.Add('Meteor root audit')
    $report.Add("production functions: $($productionFunctions.Count)")
    $report.Add("audit functions: $($audit.functions.Count)")
    $report.Add("new functions: $($newFunctions.Count)")
    $report.Add("new entries that were production local labels: $($newFormerLocalLabels.Count)")
    $report.Add("new entries still marked interior after boundary repair: $($newInteriorFunctions.Count)")
    $report.Add("unsupported instructions: $($audit.quality.unsupportedInstructionCount)")
    $report.Add("invalid SSA functions: $($audit.quality.invalidSsaFunctionCount)")
    $report.Add('')
    $report.Add('New entry points:')
    foreach ($function in $newFunctions) {
        $flags = New-Object System.Collections.Generic.List[string]
        if ($productionLocalLabels.ContainsKey([uint32]$function.entryPoint)) {
            $flags.Add('was-local-label')
        }
        if ($function.interiorToOtherTranslation) {
            $flags.Add('interior-after-repair')
        }
        $suffix = if ($flags.Count -eq 0) { '' } else { ' [' + ($flags -join ',') + ']' }
        $report.Add(('0x{0:X8}{1}' -f [uint32]$function.entryPoint, $suffix))
    }

    [System.IO.File]::WriteAllLines($deltaReport, $report, (New-Object System.Text.UTF8Encoding($false)))
    $report | Select-Object -First 12
    Write-Output "Full delta report: $deltaReport"
    Write-Output "Translation log: $auditLog"
}
finally {
    if (Test-Path $auditManifest) {
        Remove-Item -LiteralPath $auditManifest -Force
    }
}
