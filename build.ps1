<#
.SYNOPSIS
    One-command build of the ESP32-S3 firmware (node updater + brain) on Windows.

.DESCRIPTION
    powershell -ExecutionPolicy Bypass -File build.ps1 [project dir [idf.py arguments]]

    With no arguments it builds both boards' firmware, node_updater then
    brain (`idf.py build` each), and prints each binary and its flash offset.
    A failure in either stops the script with an error.

    A first argument naming an ESP-IDF project directory, relative to the
    repo root (`node_updater`, `brain`, `examples/bench_app`, ...), builds
    that project only. Any further arguments are passed to idf.py instead of
    the default build, inside the same repo-local environment (e.g.
    `brain menuconfig`, or `node_updater -p COM5 flash monitor`). idf.py
    arguments without a project first are an error: they could mean either
    board.

    First run: clones ESP-IDF (the exact tag in idf-version.txt) into
    .tools\esp-idf, installs its esp32s3 toolchain, CMake, Ninja and Python
    environment into .tools\espressif (IDF_TOOLS_PATH), then builds.
    Later runs reuse .tools and only rebuild. Nothing is written to the user
    profile: pip and ccache caches also live under .tools. (A checkout path
    with spaces additionally gets a data-less junction to .tools under
    %LOCALAPPDATA%\esp-can-updater-tools, see below.)

    Requires only Git and Python 3.8-3.13 (python.org, the py launcher, or a
    uv-managed interpreter). Everything else is installed by ESP-IDF itself.
#>

# Native tools (git, python) report progress on stderr; under 'Stop' Windows
# PowerShell 5.1 turns that into terminating errors. Every native call's exit
# code is checked explicitly instead, and file cmdlets use -ErrorAction Stop.
$ErrorActionPreference = 'Continue'

$RepoRoot     = $PSScriptRoot
$Target       = 'esp32s3'
$IdfRepoUrl   = 'https://github.com/espressif/esp-idf.git'
$IdfVersion   = (Get-Content -LiteralPath (Join-Path $RepoRoot 'idf-version.txt') -TotalCount 1 -ErrorAction Stop).Trim()
$ToolsDir     = Join-Path $RepoRoot '.tools'
$NodeUpdaterDir  = Join-Path $RepoRoot 'node_updater'
$DefaultProjects = @('node_updater', 'brain')  # built, in order, with no arguments

function Fail([string]$Message) {
    Write-Host "build.ps1: $Message" -ForegroundColor Red
    exit 1
}

function Step([string]$Message) {
    Write-Host "==> $Message" -ForegroundColor Cyan
}

# The full path of an ESP-IDF project directory given relative to the repo
# root, or $null when the argument is not one.
function Resolve-ProjectDir([string]$Relative) {
    try {
        $candidate = [IO.Path]::GetFullPath((Join-Path $RepoRoot $Relative)).TrimEnd('\', '/')
    } catch {
        return $null  # not a usable path (e.g. invalid path characters)
    }
    if (Test-Path -LiteralPath (Join-Path $candidate 'CMakeLists.txt') -PathType Leaf) {
        return $candidate
    }
    return $null
}

# An OTA application uses the node updater's partition table (the one its
# sdkconfig.defaults names) and reaches the node over CAN; any other project
# (the updater itself, the brain, the SLCAN bridge) is a standalone image
# flashed over USB, so it gets a full build and its flash offsets.
function Test-OtaApp([string]$ProjectDir) {
    if ($ProjectDir -eq $NodeUpdaterDir) {
        return $false
    }
    $setting = Select-String -LiteralPath (Join-Path $ProjectDir 'sdkconfig.defaults') `
        -Pattern '^CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="(.*)"' -ErrorAction SilentlyContinue |
        Select-Object -First 1
    if (-not $setting) {
        return $false
    }
    $table = [IO.Path]::GetFullPath([IO.Path]::Combine($ProjectDir, $setting.Matches[0].Groups[1].Value))
    return ($table -eq (Join-Path $NodeUpdaterDir 'partitions.csv'))
}

# Positional, not a -Project parameter: PowerShell would bind idf.py's `-p`
# (port) to it by prefix. idf.py arguments always follow a project, so a
# `-p COM5 flash` can never reach the wrong board.
$IdfCliArgs  = @($args)
$ProjectDirs = @($DefaultProjects | ForEach-Object { Join-Path $RepoRoot $_ })
if ($IdfCliArgs.Count -gt 0) {
    $named = Resolve-ProjectDir ([string]$IdfCliArgs[0])
    if (-not $named) {
        Fail ("'$($IdfCliArgs[0])' is not a project directory. Name the project first, then the idf.py " +
              "arguments, e.g.: build.ps1 node_updater -p COM5 flash  (projects: node_updater, brain, " +
              "examples/bench_app, examples/slcan_bridge)")
    }
    $ProjectDirs = @($named)
    $IdfCliArgs  = @($IdfCliArgs | Select-Object -Skip 1)
}

# First interpreter that runs and is 3.8 <= version < 3.14 (ESP-IDF v5.3's
# supported range); prefers 3.12/3.11, which every pinned package has wheels
# for. The Microsoft Store "python" alias stub fails the probe and is skipped.
function Find-Python {
    $probe = 'import sys; assert (3, 8) <= sys.version_info[:2] < (3, 14); print(sys.executable)'
    $candidates = @(
        @('py', '-3.12'), @('py', '-3.11'), @('py', '-3'),
        @('python'), @('python3'),
        @('uv', 'python', 'find', '3.12'), @('uv', 'python', 'find', '3.11'),
        @('uv', 'python', 'find', '>=3.8,<3.14')
    )
    foreach ($candidate in $candidates) {
        if (-not (Get-Command $candidate[0] -CommandType Application -ErrorAction SilentlyContinue)) {
            continue
        }
        $arguments = @($candidate | Select-Object -Skip 1)
        if ($candidate[0] -ne 'uv') {
            $arguments += @('-c', $probe)
        }
        $found = & $candidate[0] @arguments 2>$null | Select-Object -Last 1
        if ($LASTEXITCODE -ne 0 -or -not $found) {
            continue
        }
        $found = $found.Trim()
        if ($candidate[0] -eq 'uv') {
            & $found -c $probe 2>$null | Out-Null  # uv's pick must pass the same probe
            if ($LASTEXITCODE -ne 0) { continue }
        }
        return $found
    }
    return $null
}

# -----------------------------------------------------------------------------
# 1. Prerequisites - fail fast, naming exactly what is missing
# -----------------------------------------------------------------------------
if (-not (Get-Command git -CommandType Application -ErrorAction SilentlyContinue)) {
    Fail 'Git was not found on PATH. Install Git for Windows (https://git-scm.com/download/win) and re-run.'
}
$Python = Find-Python
if (-not $Python) {
    Fail 'Python 3.8-3.13 was not found (tried py, python, python3, uv). Install Python 3.12 from https://www.python.org/downloads/ (or: uv python install 3.12) and re-run.'
}
Step "Using Python $Python"

# install.ps1/export.ps1 invoke a bare `python`: make it the interpreter above.
$env:PATH = (Split-Path -Parent $Python) + [IO.Path]::PathSeparator + $env:PATH

# The ESP-IDF toolchain cannot run from a path with spaces: CMake hands Ninja
# such a compiler path as its 8.3 short name, and the xtensa-esp32s3-elf-gcc
# wrapper then fails to recognise itself ("XTENSA_GNU_CONFIG ... pointed
# different files"); CMake expands 8.3 aliases back, so those cannot help.
# For such a checkout, reach the same repo-local .tools through a junction at
# a space-free path. The junction holds no data; delete it any time.
New-Item -ItemType Directory -Path $ToolsDir -Force -ErrorAction Stop | Out-Null
$ToolsView = $ToolsDir
if ($RepoRoot.Contains(' ')) {
    $sha = [Security.Cryptography.SHA256]::Create()
    $hash = -join ($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($RepoRoot.ToLowerInvariant()))[0..3] |
                   ForEach-Object { $_.ToString('x2') })
    $ToolsView = Join-Path $env:LOCALAPPDATA "esp-can-updater-tools\$hash"
    if ($ToolsView.Contains(' ')) {
        Fail "The checkout path contains spaces, which the ESP-IDF toolchain does not support, and no space-free junction location exists ($ToolsView). Clone the repo to a path without spaces."
    }
    $view = Get-Item -LiteralPath $ToolsView -Force -ErrorAction SilentlyContinue
    if ($view -and ([string]$view.Target).TrimEnd('\') -ne $ToolsDir) {
        [IO.Directory]::Delete($ToolsView)  # removes the link only, never its target
        $view = $null
    }
    if (-not $view) {
        New-Item -ItemType Directory -Path (Split-Path -Parent $ToolsView) -Force -ErrorAction Stop | Out-Null
        New-Item -ItemType Junction -Path $ToolsView -Target $ToolsDir -ErrorAction Stop | Out-Null
    }
    Step "Checkout path has spaces: using .tools through junction $ToolsView"
}
$IdfPath      = Join-Path $ToolsView 'esp-idf'
$IdfToolsPath = Join-Path $ToolsView 'espressif'
$InstallStamp = Join-Path $IdfToolsPath "installed-$IdfVersion-$Target"

# Keep every ESP-IDF artifact repo-local; ignore any globally exported IDF.
$env:IDF_PATH       = $IdfPath
$env:IDF_TOOLS_PATH = $IdfToolsPath
$env:PIP_CACHE_DIR  = Join-Path $ToolsDir 'pip-cache'
$env:CCACHE_DIR     = Join-Path $ToolsDir 'ccache'
Remove-Item -Path Env:IDF_PYTHON_ENV_PATH -ErrorAction SilentlyContinue
# Launched from Git Bash, MSYSTEM is inherited and idf_tools.py refuses to run
# ("MSys/Mingw is not supported"); this is a native Windows build either way.
Remove-Item -Path Env:MSYSTEM -ErrorAction SilentlyContinue
# ESP-IDF submodule files can sit deeper than Windows' 260-character limit;
# without core.longpaths git fails on them (the clone, and ESP-IDF's own
# version detection during the build). Applies to every git started from
# here; the user's git config is left alone.
$env:GIT_CONFIG_COUNT   = '1'
$env:GIT_CONFIG_KEY_0   = 'core.longpaths'
$env:GIT_CONFIG_VALUE_0 = 'true'

# -----------------------------------------------------------------------------
# 2. ESP-IDF source, pinned (cloned under a temporary name, renamed when
#    complete, so an interrupted clone never looks installed)
# -----------------------------------------------------------------------------
if (Test-Path -LiteralPath $IdfPath) {
    $installed = (& git -C $IdfPath describe --tags --exact-match 2>$null)
    if ($installed -ne $IdfVersion) {
        Step "ESP-IDF '$installed' found but $IdfVersion is pinned: replacing it"
        Remove-Item -LiteralPath $IdfPath -Recurse -Force -ErrorAction Stop
    }
}
if (-not (Test-Path -LiteralPath $IdfPath)) {
    $partial = "$IdfPath.partial"
    if (Test-Path -LiteralPath $partial) {
        Remove-Item -LiteralPath $partial -Recurse -Force -ErrorAction Stop
    }
    Step "Cloning ESP-IDF $IdfVersion into .tools\esp-idf (first run only)"
    & git clone --depth 1 --branch $IdfVersion `
        --recursive --shallow-submodules $IdfRepoUrl $partial
    if ($LASTEXITCODE -ne 0) {
        Fail "git clone of ESP-IDF $IdfVersion failed (network, or tag missing upstream)."
    }
    Rename-Item -LiteralPath $partial -NewName (Split-Path -Leaf $IdfPath) -ErrorAction Stop
}

# -----------------------------------------------------------------------------
# 3. Toolchain + Python environment for esp32s3 (first run only)
# -----------------------------------------------------------------------------
if (-not (Test-Path -LiteralPath $InstallStamp)) {
    Step "Installing ESP-IDF tools for $Target into .tools\espressif (first run only)"
    & (Join-Path $IdfPath 'install.ps1') $Target
    if ($LASTEXITCODE -ne 0) {
        Fail "ESP-IDF install.ps1 failed (exit $LASTEXITCODE); see the output above."
    }
    New-Item -ItemType File -Path $InstallStamp -Force -ErrorAction Stop | Out-Null
}

Step 'Exporting the ESP-IDF environment'
$null = . (Join-Path $IdfPath 'export.ps1')  # dot-sourced: sets PATH/env here
if (-not (Get-Command xtensa-esp32s3-elf-gcc -ErrorAction SilentlyContinue)) {
    Fail 'export.ps1 did not put the esp32s3 toolchain on PATH; delete .tools and re-run.'
}

# -----------------------------------------------------------------------------
# 4. Build each project, then show where its binaries are (offsets straight
#    from the build's flasher args)
# -----------------------------------------------------------------------------
function Build-Project([string]$ProjectDir, [object[]]$CliArgs) {
    $BuildDir = Join-Path $ProjectDir 'build'
    $IsOtaApp = Test-OtaApp $ProjectDir
    Step "Project $($ProjectDir.Substring($RepoRoot.Length).TrimStart('\', '/'))"

    # A build dir configured from another location (e.g. the Docker build at
    # /project) cannot be reused: CMake refuses it. It is only build output.
    $cache = Join-Path $BuildDir 'CMakeCache.txt'
    if (Test-Path -LiteralPath $cache) {
        $home_line = Select-String -LiteralPath $cache -Pattern '^CMAKE_HOME_DIRECTORY:INTERNAL=(.*)$' | Select-Object -First 1
        $configured = if ($home_line) { $home_line.Matches[0].Groups[1].Value } else { '' }
        if ($configured -ne ($ProjectDir -replace '\\', '/')) {
            Step "Discarding build\ configured for '$configured'"
            Remove-Item -LiteralPath $BuildDir -Recurse -Force -ErrorAction Stop
        }
    }

    $IdfPy = Join-Path $IdfPath 'tools\idf.py'
    Push-Location -LiteralPath $ProjectDir
    try {
        if (-not (Test-Path -LiteralPath (Join-Path $ProjectDir 'sdkconfig'))) {
            Step "idf.py set-target $Target"
            & python $IdfPy set-target $Target
            if ($LASTEXITCODE -ne 0) { Fail 'idf.py set-target failed.' }
        }
        # @(...) keeps a one-element list an array: splatting a bare string would
        # pass it to idf.py one character at a time.
        # An application needs only its OTA image: `app` skips its bootloader,
        # which is never flashed (and whose object paths overflow CMake's limit
        # under a long checkout path).
        $IdfArgs = @(if ($CliArgs.Count -gt 0) { $CliArgs }
                     elseif ($IsOtaApp) { 'app' }
                     else { 'build' })
        Step "idf.py $($IdfArgs -join ' ')"
        & python $IdfPy @IdfArgs
        if ($LASTEXITCODE -ne 0) { Fail "idf.py $($IdfArgs -join ' ') failed." }
    }
    finally {
        Pop-Location
    }

    if ($CliArgs.Count -gt 0) {
        return  # custom idf.py command: its own output says what it did
    }
    $flasher = Get-Content -LiteralPath (Join-Path $BuildDir 'flasher_args.json') -Raw | ConvertFrom-Json
    if ($IsOtaApp) {
        # An application reaches the board over CAN. Its flasher args would put it
        # in the first app partition, `factory`, replacing the updater.
        Write-Host ''
        Write-Host "Built with ESP-IDF $IdfVersion for $Target. OTA image to flash over CAN (tools\can-flash):" -ForegroundColor Green
        Write-Host ('  ' + (Join-Path $BuildDir ($flasher.app.file -replace '/', '\')))
        Write-Host 'Do not idf.py flash it onto an updater board. Details: docs\guides\writing-a-node-app.md'
        Write-Host ''
        return
    }
    Write-Host ''
    Write-Host "Built with ESP-IDF $IdfVersion for $Target. Flash offsets and files:" -ForegroundColor Green
    foreach ($entry in ($flasher.flash_files.PSObject.Properties | Sort-Object { [Convert]::ToInt64($_.Name, 16) })) {
        Write-Host ('  {0,-9} {1}' -f $entry.Name, (Join-Path $BuildDir ($entry.Value -replace '/', '\')))
    }
    Write-Host ''
    Write-Host "Flash all of them over USB/UART (from $BuildDir):  python -m esptool --chip esp32s3 write_flash `"@flash_args`""
    Write-Host 'Details: docs\getting-started\first-flash.md'
    Write-Host ''
}

foreach ($projectDir in $ProjectDirs) {
    Build-Project $projectDir $IdfCliArgs
}
