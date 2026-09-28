# Downloads War Thunder minimap images + metadata from wt-tools.app's public
# manifest and generates data/maps.cfg entries for wt-rangefinder.
#
# Source: https://wt-tools.app/maps-overview.html (community tool, public
# Google Cloud Storage bucket). Downloads EVERY map+mode variant (Battle,
# Conquest, Domination, ...) since tile_size (and layout) can differ between
# variants of the same map.

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$mapsDir = Join-Path $root "data\maps"
$cfgPath = Join-Path $root "data\maps.cfg"
New-Item -ItemType Directory -Force -Path $mapsDir | Out-Null

Write-Host "Fetching manifest..."
$manifest = Invoke-RestMethod -Uri "https://wt-tools.app/manifest.json"

function ToTitleCase($slug) {
    $words = $slug -split "_"
    $words = $words | ForEach-Object {
        if ($_ -match "^\d") { $_ } else { (Get-Culture).TextInfo.ToTitleCase($_) }
    }
    return ($words -join " ")
}

# "conquest-2" -> mode category "Conquest", variant label "Conquest 2"
function SplitModeKey($modeKey) {
    $parts = $modeKey -split "-"
    $category = (Get-Culture).TextInfo.ToTitleCase($parts[0])
    $variantLabel = ToTitleCase ($modeKey -replace "-", "_")
    return @{ Category = $category; VariantLabel = $variantLabel }
}

$lines = New-Object System.Collections.Generic.List[string]
$lines.Add("")
$lines.Add("# --- Auto-generated from wt-tools.app manifest (scripts/fetch_maps.ps1) ---")

$count = 0
$failed = New-Object System.Collections.Generic.List[string]
$mapKeys = $manifest.PSObject.Properties.Name | Sort-Object

foreach ($mapKey in $mapKeys) {
    $modes = $manifest.$mapKey
    $modeKeys = $modes.PSObject.Properties.Name | Sort-Object
    $mapDisplayName = ToTitleCase $mapKey

    foreach ($modeKey in $modeKeys) {
        $entry = $modes.$modeKey
        $size = [double]$entry.size
        $tile = [double]$entry.tile_size
        $gridCells = [math]::Floor($size / $tile)
        if ($gridCells -lt 1) { $gridCells = 1 }
        if ($gridCells -gt 26) { $gridCells = 26 }

        $split = SplitModeKey $modeKey
        $imageFile = "${mapKey}_${modeKey}.png"
        $destPath = Join-Path $mapsDir $imageFile
        $url = "https://storage.googleapis.com/wt-map-files/maps/$mapKey/$modeKey/$($entry.image)"

        try {
            if (-not (Test-Path $destPath)) {
                Invoke-WebRequest -Uri $url -OutFile $destPath -UseBasicParsing
            }
            $displayName = "$mapDisplayName - $($split.VariantLabel)"
            $lines.Add("$($split.Category) | $displayName | maps/$imageFile | $gridCells | $tile")
            $count++
        } catch {
            $failed.Add("$mapKey/$modeKey : $($_.Exception.Message)")
        }
    }
}

Add-Content -Path $cfgPath -Value ($lines -join "`n")

Write-Host "Done. $count map/mode entries downloaded and added to maps.cfg."
if ($failed.Count -gt 0) {
    Write-Host "Failed ($($failed.Count)):"
    $failed | ForEach-Object { Write-Host "  $_" }
}
