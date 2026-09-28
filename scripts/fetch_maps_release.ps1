# Downloads War Thunder minimap images for a compiled wt-rangefinder
# release. Unlike scripts/fetch_maps.ps1 (used from a source checkout to
# also regenerate data/maps.cfg), this only fetches the images - a release
# zip already ships a maps.cfg with the right metadata, it just doesn't
# bundle the images themselves (see the README for why).
#
# Run this from the folder where wt-rangefinder.exe lives.

$ErrorActionPreference = "Stop"
$root = $PSScriptRoot
$mapsDir = Join-Path $root "maps"
New-Item -ItemType Directory -Force -Path $mapsDir | Out-Null

Write-Host "Fetching manifest..."
$manifest = Invoke-RestMethod -Uri "https://wt-tools.app/manifest.json"

$count = 0
$skipped = 0
$failed = New-Object System.Collections.Generic.List[string]
$mapKeys = $manifest.PSObject.Properties.Name | Sort-Object

foreach ($mapKey in $mapKeys) {
    $modes = $manifest.$mapKey
    $modeKeys = $modes.PSObject.Properties.Name | Sort-Object

    foreach ($modeKey in $modeKeys) {
        $entry = $modes.$modeKey
        $imageFile = "${mapKey}_${modeKey}.png"
        $destPath = Join-Path $mapsDir $imageFile
        if (Test-Path $destPath) {
            $skipped++
            continue
        }
        $url = "https://storage.googleapis.com/wt-map-files/maps/$mapKey/$modeKey/$($entry.image)"
        try {
            Invoke-WebRequest -Uri $url -OutFile $destPath -UseBasicParsing
            $count++
        } catch {
            $failed.Add("$mapKey/$modeKey : $($_.Exception.Message)")
        }
    }
}

Write-Host "Done. Downloaded $count map/mode images ($skipped already present)."
if ($failed.Count -gt 0) {
    Write-Host "Failed ($($failed.Count)):"
    $failed | ForEach-Object { Write-Host "  $_" }
}
