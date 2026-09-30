<p align="center">
  <img src="assets/logo-256.png" alt="wt-rangefinder logo" width="160">
</p>

# wt-rangefinder

A small standalone Windows tool for estimating distances on War Thunder's
minimap by hand: drop a pin for yourself and one for a target, and it reads
off the distance using each map's real grid-square size. No game
integration, no memory reading, no automation - it just measures pixels on
an image and does the arithmetic.

Not affiliated with or endorsed by Gaijin Entertainment. "War Thunder" is a
trademark of Gaijin Entertainment; the minimap images are their game assets
and are **not** included in this repository (see [Map assets](#map-assets)
below).

## Features

- Searchable, mode-filtered map picker (62 maps, every game-mode variant).
- Click/drag to place your position and the enemy's; distance (rounded to
  the nearest 5 m), grid sector and grid squares update live.
- Range rings snapped to each map's real grid spacing (not an arbitrary
  scale).
- Keyboard pin nudging: `hjkl` for half-a-grid-square jumps, arrow keys for
  2px precision, `Shift` to target the enemy pin instead of yourself.
- **In-game mode** (`F1`): hides everything but the map, turns on
  always-on-top, and sizes/positions the window to match War Thunder's own
  minimap so it drops in already aligned after alt-tabbing back. The window
  fades to low opacity when unfocused so the real game shows through, and
  snaps back when you click it. Leaving it restores your previous window
  size and position.
- Window always keeps the map's true aspect ratio while resizing - no
  letterboxing.
- Window size and UI theme persist between runs.
- 15 color themes, grouped in the picker: Dark and Light; single-color
  palettes (Amber, Blue, Green, Pink, Purple, Red, Teal); and well-known
  palettes (Gruvbox, Nord, and the four Catppuccin flavors).
- `Ctrl+Q` to quit.

## Download

Grab the latest `wt-rangefinder-*-win64.zip` from the
[Releases](https://github.com/Fedebarriosd/wt-rangefinder/releases) page,
extract it anywhere, and run `fetch_maps.ps1` once (right click ->
"Run with PowerShell") to download the minimap images next to the exe - see
[Map assets](#map-assets) for why they aren't bundled. Then run
`wt-rangefinder.exe`.

The exe is unsigned, so Windows SmartScreen / Smart App Control may warn
about it. You can audit the source here and build it yourself instead.

## Building

Requires CMake and MSVC (Visual Studio 2022 Build Tools or full VS). SDL2,
Dear ImGui and stb_image are fetched automatically by CMake.

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

The executable ends up at `build/Release/wt-rangefinder.exe`, with the
`data/` folder (minus the map images themselves, see below) copied next to
it automatically.

## Map assets

The minimap images aren't tracked in this repo - they're Gaijin's game
assets, scraped from the community tool [wt-tools.app](https://wt-tools.app),
and redistributing them here would step on their copyright. `data/maps.cfg`
has the grid metadata for every map already; you just need the images.

From a source checkout:

```powershell
./scripts/fetch_maps.ps1
```

This downloads all ~300 map/mode image variants (~500MB) into `data/maps/`
and regenerates the map entries in `data/maps.cfg`. Run it once after
cloning, and again when a new map ships in-game (as long as wt-tools.app's
manifest has been updated for it).

Release zips include a lighter `fetch_maps.ps1` (source:
`scripts/fetch_maps_release.ps1`) that only downloads the images into
`maps/` next to the exe, since the zip's `maps.cfg` is already complete.

## Usage

- **Left click / drag**: place your position.
- **Right click / drag**: place the enemy's position.
- **hjkl / arrow keys**: nudge the active pin (hold `Shift` to move the
  enemy pin instead).
- **C**: clear both pins.
- **F1**: toggle in-game mode.
- **Ctrl+Q**: quit.
- "Calibrate grid" (in the panel) if a map's grid doesn't line up with the
  image borders - click the two opposite corners of the grid. "Reset to
  default" undoes it.

## License

[GNU AGPL v3](LICENSE). If you run a modified version of this as a network
service, the AGPL requires you to make your modified source available to
users of that service - see the license text for the exact terms.
