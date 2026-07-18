# Build the release firmware and export flashable artifacts (.elf/.bin/.hex)
# for both the bootloader and the demo app into out/.
#
# Usage:  pwsh scripts/build-out.ps1   (or right-click > Run with PowerShell)
# PowerShell 7+ port of build-out.sh for Windows. .hex/.bin are produced with
# llvm-objcopy from the toolchain's llvm-tools component (already required by
# rust-toolchain.toml) — same tool CI uses.
$ErrorActionPreference = 'Stop'

Set-Location (Join-Path $PSScriptRoot '..')

$Target = 'thumbv7em-none-eabihf'
$Out = 'out'
$Host_ = (& rustc -vV | Select-String '^host: ').Line -replace '^host: ', ''
$Sysroot = (& rustc --print sysroot).Trim()
$BinDir = Join-Path $Sysroot "lib\rustlib\$Host_\bin"
$Objcopy = Join-Path $BinDir 'llvm-objcopy.exe'
$Size = Join-Path $BinDir 'llvm-size.exe'

& cargo build --release --target $Target -p bootloader -p demo-app
if ($LASTEXITCODE -ne 0) { throw "cargo build failed ($LASTEXITCODE)" }

New-Item -ItemType Directory -Force -Path $Out | Out-Null
foreach ($crate in 'bootloader', 'demo-app') {
    $elf = "target/$Target/release/$crate"
    Copy-Item $elf "$Out/$crate.elf" -Force
    & $Objcopy -O binary $elf "$Out/$crate.bin"
    & $Objcopy -O ihex   $elf "$Out/$crate.hex"
}

Write-Host ''
Write-Host "Artifacts written to $Out/:"
Get-ChildItem "$Out/bootloader.*", "$Out/demo-app.*" | Select-Object Name, Length | Format-Table | Out-Host
Write-Host ''
Write-Host 'Sizes (bootloader gate = 32768 B):'
& $Size "target/$Target/release/bootloader" "target/$Target/release/demo-app"
