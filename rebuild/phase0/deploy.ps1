# Copies the built sb2modern DLLs into the game folder.
# dgVoodoo's wrapper DLLs are moved aside (not deleted) the first time; restore them from
# _dgvoodoo_backup\ to go back.
param([string]$GameDir = (Resolve-Path "$PSScriptRoot\..\..").Path,
      [string]$Config = "Release")

$build = Join-Path $PSScriptRoot "build\$Config"
$backup = Join-Path $GameDir "_dgvoodoo_backup"

foreach ($f in 'D3D8.dll', 'D3D9.dll', 'D3DImm.dll', 'DDraw.dll', 'dgVoodoo.conf') {
    $src = Join-Path $GameDir $f
    if ((Test-Path $src) -and ((Get-Item $src).VersionInfo.ProductName -eq 'dgVoodoo' -or $f -eq 'dgVoodoo.conf')) {
        New-Item -ItemType Directory -Force $backup | Out-Null
        Move-Item -Force $src (Join-Path $backup $f)
        Write-Host "moved $f -> _dgvoodoo_backup\"
    }
}

foreach ($f in 'ddraw.dll', 'dsound.dll') {
    Copy-Item -Force (Join-Path $build $f) (Join-Path $GameDir $f)
    Write-Host "installed $f"
}
