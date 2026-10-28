param(
    [string]$ExeFsPath = "C:\\zelda_work\\exefs",
    [string]$RomFsPath = "C:\\zelda_work\\romfs",
    [string]$OutZip = "C:\\zelda_work\\botw_core.zip"
)

$ErrorActionPreference = "Stop"
$requiredExeFs = @("main", "main.npdm", "rtld", "sdk", "subsdk0")
foreach ($name in $requiredExeFs) {
    $p = Join-Path $ExeFsPath $name
    if (!(Test-Path -LiteralPath $p -PathType Leaf)) { throw "Missing ExeFS file: $p" }
    if ((Get-Item -LiteralPath $p).Length -le 0) { throw "Empty ExeFS file: $p" }
}
$versionPath = Join-Path $RomFsPath "System\\Version.txt"
$packPath = Join-Path $RomFsPath "Pack\\Bootup_Graphics.pack"
if (!(Test-Path -LiteralPath $versionPath -PathType Leaf)) { throw "Missing $versionPath" }
if ((Get-Content -LiteralPath $versionPath -Raw).Trim() -ne "1.9.0") { throw "RomFS is not BOTW 1.9.0." }
if (!(Test-Path -LiteralPath $packPath -PathType Leaf)) { throw "Missing $packPath" }
if ((Get-Item -LiteralPath $packPath).Length -lt 1MB) { throw "Bootup_Graphics.pack is too small; refusing a placeholder." }

$stage = Join-Path ([IO.Path]::GetTempPath()) ("botw_core_" + [guid]::NewGuid().ToString("N"))
$core = Join-Path $stage "botw_core"
try {
    New-Item -ItemType Directory -Force -Path (Join-Path $core "ExeFS"), (Join-Path $core "RomFS\\System"), (Join-Path $core "RomFS\\Pack") | Out-Null
    foreach ($name in $requiredExeFs) { Copy-Item -LiteralPath (Join-Path $ExeFsPath $name) -Destination (Join-Path $core "ExeFS\\$name") }
    Copy-Item -LiteralPath $versionPath -Destination (Join-Path $core "RomFS\\System\\Version.txt")
    Copy-Item -LiteralPath $packPath -Destination (Join-Path $core "RomFS\\Pack\\Bootup_Graphics.pack")
    $outDir = Split-Path -Parent $OutZip
    if ($outDir) { New-Item -ItemType Directory -Force -Path $outDir | Out-Null }
    if (Test-Path -LiteralPath $OutZip) { Remove-Item -LiteralPath $OutZip -Force }
    Compress-Archive -Path (Join-Path $stage "botw_core") -DestinationPath $OutZip -CompressionLevel Optimal
    $zip = [IO.Compression.ZipFile]::OpenRead((Resolve-Path -LiteralPath $OutZip).Path)
    try {
        if ($zip.Entries.FullName -match '(^|/)prod\\.keys$') { throw "Refusing a core ZIP containing prod.keys." }
        if ($zip.Entries.Count -gt 12) { throw "Core ZIP unexpectedly contains too many entries; do not include the full RomFS." }
    } finally { $zip.Dispose() }
    Write-Host "Created key-free lightweight core ZIP: $OutZip"
    Get-Item -LiteralPath $OutZip | Select-Object FullName, Length
    Write-Host "Contains ExeFS plus only RomFS/System/Version.txt and RomFS/Pack/Bootup_Graphics.pack. It does NOT contain the full RomFS or prod.keys."
} finally {
    if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
}
