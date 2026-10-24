<#
  build-uwp.ps1: builds castcore (xbox-cast AirPlay receiver core) for UWP and installs the layout of
  docs/contract.md section 2:

      native/out/uwp-<arch>/include/castcore.h
      native/out/uwp-<arch>/lib/castcore.lib + libcrypto.lib + plist-2.0.lib
      native/out/uwp-<arch>/lib/link.txt        (one lib per line: what the app links)
      native/out/uwp-<arch>/bin/                (no runtime DLLs: everything is static)
      native/out/uwp-<arch>/licenses/           (OpenSSL / libplist copyright files)

  Usage:  pwsh native/build-uwp.ps1 -Arch x64 -Config Release

  Self-contained: finds Visual Studio (vswhere), enters its developer shell, uses vcpkg from $env:VCPKG_ROOT or
  $env:VCPKG_INSTALLATION_ROOT (GitHub runners) or clones it into native/out/vcpkg, installs the manifest
  (native/vcpkg.json: openssl, libplist) for the <arch>-uwp-static-md triplet (static libs, dynamic CRT /MD as UWP
  requires), configures CMake with CMAKE_SYSTEM_NAME=WindowsStore, builds, links a test DLL with /APPCONTAINER
  (castcore_linkcheck: proves every symbol resolves with app-partition APIs) and installs.
  Honours $env:VCPKG_DEFAULT_BINARY_CACHE (CI caches it).
#>
param(
    [ValidateSet('x64', 'arm64')] [string] $Arch = 'x64',
    [ValidateSet('Release', 'Debug', 'RelWithDebInfo')] [string] $Config = 'Release'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 3

$native  = $PSScriptRoot
$outRoot = Join-Path $native 'out'
$build   = Join-Path $outRoot "build-uwp-$Arch"
$prefix  = Join-Path $outRoot "uwp-$Arch"
$triplet = "$Arch-uwp-static-md"
New-Item -ItemType Directory -Force $outRoot | Out-Null

function Invoke-Checked([string] $what, [scriptblock] $cmd) {
    Write-Host "==> $what"
    & $cmd
    if ($LASTEXITCODE) { throw "$what failed (exit code $LASTEXITCODE)" }
}

# ------------------------------------------------------------------ Visual Studio developer shell
if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path $vswhere)) { throw "vswhere.exe not found: install Visual Studio with the C++ (and UWP) workloads" }
    $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $vs) { throw "no Visual Studio with C++ tools found" }
    Write-Host "Visual Studio: $vs"
    Import-Module (Join-Path $vs 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll')
    $hostArch = if ([System.Runtime.InteropServices.RuntimeInformation]::OSArchitecture -eq 'Arm64') { 'arm64' } else { 'x64' }
    Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments "-arch=$Arch -host_arch=$hostArch -no_logo" | Out-Null
}
Write-Host "cl.exe: $((Get-Command cl.exe).Source)"

# ------------------------------------------------------------------ vcpkg
$vcpkg = $null
foreach ($cand in @($env:VCPKG_ROOT, $env:VCPKG_INSTALLATION_ROOT, (Join-Path $outRoot 'vcpkg'))) {
    if ($cand -and (Test-Path (Join-Path $cand 'scripts\buildsystems\vcpkg.cmake'))) { $vcpkg = $cand; break }
}
if (-not $vcpkg) {
    $vcpkg = Join-Path $outRoot 'vcpkg'
    Invoke-Checked 'clone vcpkg' { git clone --depth 1 https://github.com/microsoft/vcpkg.git $vcpkg }
}
if (-not (Test-Path (Join-Path $vcpkg 'vcpkg.exe'))) {
    Invoke-Checked 'bootstrap vcpkg' { & (Join-Path $vcpkg 'bootstrap-vcpkg.bat') -disableMetrics }
}
$env:VCPKG_ROOT = $vcpkg
Write-Host "vcpkg: $vcpkg ($triplet)"
if ($env:VCPKG_DEFAULT_BINARY_CACHE) {
    New-Item -ItemType Directory -Force $env:VCPKG_DEFAULT_BINARY_CACHE | Out-Null
    Write-Host "vcpkg binary cache: $env:VCPKG_DEFAULT_BINARY_CACHE"
}

# ------------------------------------------------------------------ configure / build / install
$cmakeArgs = @(
    '-S', $native, '-B', $build,
    "-DCMAKE_TOOLCHAIN_FILE=$vcpkg\scripts\buildsystems\vcpkg.cmake",
    "-DVCPKG_TARGET_TRIPLET=$triplet",
    "-DVCPKG_INSTALLED_DIR=$outRoot\vcpkg_installed",
    '-DCMAKE_SYSTEM_NAME=WindowsStore',
    '-DCMAKE_SYSTEM_VERSION=10.0',
    "-DCMAKE_INSTALL_PREFIX=$prefix",
    '-DCASTCORE_BUILD_HOSTRX=OFF',
    '-DCASTCORE_BUILD_TESTS=OFF'
)
if (Get-Command ninja -ErrorAction SilentlyContinue) {
    $cmakeArgs += @('-G', 'Ninja', "-DCMAKE_BUILD_TYPE=$Config")
} else {
    Write-Host "ninja not found: using the Visual Studio generator"
    $cmakeArgs += @('-A', $(if ($Arch -eq 'x64') { 'x64' } else { 'ARM64' }))
}
Invoke-Checked 'cmake configure (vcpkg installs openssl + libplist on the first run)' { cmake @cmakeArgs }
Invoke-Checked 'cmake build' { cmake --build $build --config $Config --parallel }
if (Test-Path $prefix) { Remove-Item -Recurse -Force $prefix }
Invoke-Checked 'cmake install' { cmake --install $build --config $Config }

# ------------------------------------------------------------------ dependency libs, link.txt, licenses
$depRoot = Join-Path $outRoot "vcpkg_installed\$triplet"
$depLib  = if ($Config -eq 'Debug') { Join-Path $depRoot 'debug\lib' } else { Join-Path $depRoot 'lib' }
$lib = Join-Path $prefix 'lib'
New-Item -ItemType Directory -Force (Join-Path $prefix 'bin'), (Join-Path $prefix 'licenses') | Out-Null

$deps = @('libcrypto.lib', 'plist-2.0.lib')
foreach ($d in $deps) {
    $src = Join-Path $depLib $d
    if (-not (Test-Path $src)) { throw "expected vcpkg library $src not found (contents: $((Get-ChildItem $depLib -Name) -join ', '))" }
    Copy-Item $src $lib
}
foreach ($p in @('openssl', 'libplist')) {
    $c = Join-Path $depRoot "share\$p\copyright"
    if (Test-Path $c) { Copy-Item $c (Join-Path $prefix "licenses\$p.txt") }
}
Copy-Item (Join-Path $native 'third_party\uxplay\LICENSE') (Join-Path $prefix 'licenses\uxplay-GPL-3.0.txt')

# castcore first, then its static dependencies, then the Windows import libraries they need
$linkLibs = @('castcore.lib') + $deps + @('ws2_32.lib', 'iphlpapi.lib', 'bcrypt.lib', 'crypt32.lib')
Set-Content -Path (Join-Path $lib 'link.txt') -Value $linkLibs -Encoding ascii

if (-not (Test-Path (Join-Path $prefix 'include\castcore.h'))) { throw "install did not produce include/castcore.h" }
$check = Get-ChildItem $build -Recurse -Filter 'castcore_linkcheck.dll' | Select-Object -First 1
if (-not $check) { throw "castcore_linkcheck.dll was not built" }
Write-Host "UWP link check DLL: $($check.FullName) ($([math]::Round($check.Length / 1KB)) KB)"
& dumpbin /nologo /dependents $check.FullName | Select-String '\.dll' | ForEach-Object { "  imports $($_.Line.Trim())" }

Write-Host "==> castcore for UWP $Arch ($Config) installed in $prefix"
Get-ChildItem -Recurse $prefix | Where-Object { -not $_.PSIsContainer } |
    ForEach-Object { "  {0,-40} {1,10:N0} bytes" -f $_.FullName.Substring($prefix.Length + 1), $_.Length }
Write-Host "link.txt:"
Get-Content (Join-Path $lib 'link.txt') | ForEach-Object { "  $_" }
