[CmdletBinding(SupportsShouldProcess)]
param(
    [string]$BuildBin = '',

    [string]$Output = '',

    [switch]$ExcludeControllerPrompts,

    [switch]$Force
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if ([string]::IsNullOrWhiteSpace($BuildBin)) {
    $BuildBin = Join-Path $PSScriptRoot '..\build\meteor\native\Windows\bin'
}
if ([string]::IsNullOrWhiteSpace($Output)) {
    $Output = Join-Path $PSScriptRoot '..\dist\MeteorRecomp-portable'
}

function Resolve-FullPath([string]$Path) {
    return [System.IO.Path]::GetFullPath($Path)
}

function Copy-PortableFile([string]$SourceRoot, [string]$DestinationRoot, [string]$RelativePath) {
    $source = Join-Path $SourceRoot $RelativePath
    $destination = Join-Path $DestinationRoot $RelativePath
    if (-not (Test-Path -LiteralPath $source -PathType Leaf)) {
        throw "Missing required release file: $source"
    }
    $destinationParent = Split-Path -Parent $destination
    New-Item -ItemType Directory -Force -Path $destinationParent | Out-Null
    Copy-Item -LiteralPath $source -Destination $destination -Force
}

function Copy-PortableDirectory([string]$SourceRoot, [string]$DestinationRoot, [string]$RelativePath) {
    $source = Join-Path $SourceRoot $RelativePath
    $destination = Join-Path $DestinationRoot $RelativePath
    if (-not (Test-Path -LiteralPath $source -PathType Container)) {
        throw "Missing required release directory: $source"
    }
    New-Item -ItemType Directory -Force -Path $destination | Out-Null
    Copy-Item -Path (Join-Path $source '*') -Destination $destination -Recurse -Force
}

$sourceRoot = Resolve-FullPath $BuildBin
$destinationRoot = Resolve-FullPath $Output
$includeControllerPrompts = -not $ExcludeControllerPrompts.IsPresent
if (-not (Test-Path -LiteralPath $sourceRoot -PathType Container)) {
    throw "Build output directory was not found: $sourceRoot"
}

$requiredFiles = @(
    'meteor.exe',
    'SDL3.dll',
    'webgpu_dawn.dll',
    'dxcompiler.dll',
    'dxil.dll',
    'libc++.dll',
    'libunwind.dll',
    'libpng16.dll',
    'libz.dll',
    'dsp_coef.bin',
    'font_japanese.bin',
    'font_western.bin',
    'initial_pipeline_cache.db'
)
$requiredDirectories = @('wii_bootstrap')
if ($includeControllerPrompts) {
    $requiredDirectories += 'controller_prompts'
}

foreach ($file in $requiredFiles) {
    if (-not (Test-Path -LiteralPath (Join-Path $sourceRoot $file) -PathType Leaf)) {
        throw "The selected build is incomplete; required file is missing: $file"
    }
}
foreach ($directory in $requiredDirectories) {
    if (-not (Test-Path -LiteralPath (Join-Path $sourceRoot $directory) -PathType Container)) {
        throw "The selected build is incomplete; required directory is missing: $directory"
    }
}

if (Test-Path -LiteralPath $destinationRoot) {
    if (-not $Force) {
        throw "Output already exists: $destinationRoot (use -Force to replace it)"
    }
    if ($PSCmdlet.ShouldProcess($destinationRoot, 'Remove previous portable package')) {
        Remove-Item -LiteralPath $destinationRoot -Recurse -Force
    }
}

if ($PSCmdlet.ShouldProcess($destinationRoot, 'Create portable package')) {
    New-Item -ItemType Directory -Force -Path $destinationRoot | Out-Null

    foreach ($file in $requiredFiles) {
        Copy-PortableFile $sourceRoot $destinationRoot $file
    }
    foreach ($directory in $requiredDirectories) {
        Copy-PortableDirectory $sourceRoot $destinationRoot $directory
    }

    Set-Content -LiteralPath (Join-Path $destinationRoot 'portable.txt') -Value 'portable' -Encoding ascii

    $gameRoot = Join-Path $destinationRoot 'UserData\Projects\meteor\game'
    New-Item -ItemType Directory -Force -Path $gameRoot | Out-Null
    $projectRoot = Split-Path -Parent $gameRoot
    $configPath = Join-Path $projectRoot 'Config.toml'
    if ($includeControllerPrompts) {
        Set-Content -LiteralPath $configPath -Value @'
[controller]
prompt_style = "ps2"
a = "west"
b = "south"
x = "east"
y = "north"
start = "start"
z = "left_shoulder"
l = "back"
r = "right_shoulder"
up = "dpad_up"
down = "dpad_down"
left = "dpad_left"
right = "dpad_right"

[paths]
dvd_root = "game"

[video]
frame_interpolation_fps = 0
skip_unready_pipelines = false
'@ -Encoding utf8
    } else {
        Set-Content -LiteralPath $configPath -Value @'
[controller]
prompt_style = "gamecube"
a = "south"
b = "east"
x = "west"
y = "north"
start = "start"
z = "right_shoulder"
l = "unmapped"
r = "unmapped"
up = "dpad_up"
down = "dpad_down"
left = "dpad_left"
right = "dpad_right"

[paths]
dvd_root = "game"

[video]
frame_interpolation_fps = 0
skip_unready_pipelines = false
'@ -Encoding utf8
    }
    $repositoryRoot = Resolve-FullPath (Join-Path $PSScriptRoot '..')
    $gameDataSource = Join-Path $repositoryRoot 'local-data\meteor\DATA'
    $gameDataBundled = Test-Path -LiteralPath $gameDataSource -PathType Container
    if ($gameDataBundled) {
        Copy-Item -Path (Join-Path $gameDataSource '*') -Destination $gameRoot -Recurse -Force
    } else {
        Set-Content -LiteralPath (Join-Path $gameRoot 'PLACE_DATA_HERE.txt') -Value @(
            'Copy the extracted PAL RDSPAF DATA directory contents here.',
            'The directory must contain files\ and sys\fst.bin.',
            'Do not copy personal saves or Nintendo keys into the portable package.'
        ) -Encoding utf8
    }

    Set-Content -LiteralPath (Join-Path $destinationRoot 'Run-Meteor.cmd') -Value @(
        '@echo off',
        'for /f "tokens=1 delims==" %%V in (''set METEOR_ 2^>nul'') do set "%%V="',
        'for /f "tokens=1 delims==" %%V in (''set AURORA_ 2^>nul'') do set "%%V="',
        'start "MeteorRecomp" "%~dp0meteor.exe"'
    ) -Encoding ascii

    $readme = @'
MeteorRecomp portable build
===========================

1. The package includes the local Dragon Ball Z: Budokai Tenkaichi 3 PAL
   RDSPAF DATA tree when it is available at build time. If it is absent, copy
   your own legally obtained DATA directory into:

       UserData\Projects\meteor\game

   That directory must contain at least `files\` and `sys\fst.bin`.
   Personal saves and Nintendo keys are not included in this package.

2. Start `Run-Meteor.cmd`. The package seeds Config.toml with Meteor's complete
   PlayStation 2 preset: PS2 prompt art plus the matching physical face/shoulder
   layout. F10 -> Controller -> Presets can switch back to GameCube or Classic
   Controller Pro, and individual bindings remain editable. Logs, Cache, NAND,
   memory cards and other writable state stay below this
   package's UserData directory; it does not use AppData/Documents while the
   `portable.txt` marker is present. The folder can be moved or copied to
   another Windows 10/11 x64 computer.

   A packager who explicitly uses `-ExcludeControllerPrompts` gets the GameCube
   preset instead, because PS2 glyphs cannot be selected without that resource
   directory.

3. F10 opens the runtime settings overlay. The portable starts with frame
   interpolation at 0 and required draws waiting for their graphics pipelines.
   The draw-skipping checkbox remains available only as an explicit diagnostic
   option. Resolution can be changed from the overlay; it is not forced by the
   portable launcher.

The package is a release runtime only. Run-Meteor.cmd clears inherited
METEOR_* and AURORA_* developer overrides before launch. To collect a trace or
use a developer-only environment override, launch meteor.exe directly from a
prepared diagnostic terminal instead of using Run-Meteor.cmd.
'@
    Set-Content -LiteralPath (Join-Path $destinationRoot 'PORTABLE_README.txt') -Value $readme -Encoding utf8

    $manifestLines = @(
        '# Relative path<TAB>SHA-256'
    )
    $manifestRoots = @($requiredFiles + $requiredDirectories + @(
        'portable.txt',
        'Run-Meteor.cmd',
        'PORTABLE_README.txt',
        'UserData\Projects\meteor\Config.toml',
        'UserData\Projects\meteor\game'
    ))
    $manifestPaths = @($manifestRoots | ForEach-Object {
        $path = Join-Path $destinationRoot $_
        if (Test-Path -LiteralPath $path -PathType Leaf) {
            $_
        } else {
            Get-ChildItem -LiteralPath $path -Recurse -File | ForEach-Object {
                $_.FullName.Substring($destinationRoot.Length + 1)
            }
        }
    }) | Sort-Object
    foreach ($relativePath in $manifestPaths) {
        $hash = (Get-FileHash -LiteralPath (Join-Path $destinationRoot $relativePath) -Algorithm SHA256).Hash
        $manifestLines += "$relativePath`t$hash"
    }
    Set-Content -LiteralPath (Join-Path $destinationRoot 'portable_manifest.txt') -Value $manifestLines -Encoding ascii

    Write-Output "Portable package ready: $destinationRoot"
    Write-Output "Included controller prompts: $includeControllerPrompts"
    Write-Output "Game data bundled: $gameDataBundled"
}
