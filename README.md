# ClickGotchi

A pet companion for the iPod Video 5.5G, running as a RockBox plugin. A port of CYDgotchi / LoRagotchi.

**Status: v0.1, preliminary.** Built for IPOD_VIDEO against RockBox v4.0-final (plugin API 273). Stage 1 (the pet) and Stage 2 (options, create a pet, reset) ran on the device. The home-screen controls, music-reactive meters and the track progress bar are built but not yet fully tested on the device.

## Install
1. Download `clickgotchi.rock` from the latest [release](../../releases).
2. Copy it to `.rockbox/rocks/` on the iPod (or `.rockbox/rocks/games/`).
3. Open Plugins > Games > ClickGotchi.

## Controls
- **Home screen (music keys):**
  - Play/Pause toggles playback.
  - Left / Right skip to the previous or next track.
  - Scroll wheel changes volume.
  - SELECT held cleans the environment.
  - MENU opens the Care menu. MENU held quits.
- **Options and dialogs:** the standard RockBox keys. CANCEL goes back.

## What the pet does
- Full-size CYDgotchi sprites (scale 3, 96x72), the CYD scenery (Water / Air / Land), egg, hatch and memorial art. 76 species, including the tropical fish as the default for a new pet.
- Care rules from LoRagotchi: meal once an hour, waste, clean, happiness decay (30 minutes awake), life stages, 7-day life.
- Reacts to the iPod: asleep 22:00-07:00 by the RTC (option), rests while charging (option), tired at 20% battery or less, USB quits as before.
- **Fertilise** is a Care menu option. It is never automatic and not tied to charging or resting. It lays a solo egg.
- Ages only while the plugin is open (CYD rule).
- Music reactive (option, off by default): a playing track keeps the pet awake, the home screen shows a left and right level meter at the screen edges, "Artist - Title" above the footer, and a progress bar on the sand.
- Options: show seconds, backlight on USB, sleep hours, rest while charging, rendering speed (Smooth / Simulation / Efficient), music reactive, change pet name, reset pet.

## Not in this version
- No breeding with visitors, and no transfer over BLE, WiFi or LoRa.
- Hard-coded English text. RockBox normally uses `LANG_` IDs, so these need moving before any upstream submission.

## Layout
```
ClickGotchi/
  README.md
  LICENSE, NOTICE
  apps/plugins/clickgotchi.c       the plugin
  apps/plugins/clickgotchi_art.h   generated sprite masks (from LoRagotchi-DX)
  apps/plugins/SOURCES.add         one line for RockBox apps/plugins/SOURCES
  apps/plugins/CATEGORIES.add      one line for RockBox apps/plugins/CATEGORIES
  tools/build-rock.sh              builds clickgotchi.rock (clones RockBox v4.0-final)
  tools/fish/                      generator for the tropical fish mask
  .github/workflows/release.yml    builds the .rock and publishes it on a v* tag
```

## Building
```
tools/build-rock.sh [output-dir]
```
This clones RockBox at `v4.0-final`, adds the plugin, configures `ipodvideo` and writes `clickgotchi.rock` (needs `git`, `make`, `gcc`, `arm-none-eabi-gcc`).

## Releases
Pushing a tag that starts with `v` (for example `v0.1`) runs `.github/workflows/release.yml`. It builds the `.rock` and publishes it as a GitHub release with a SHA-256 checksum. Tags `v0.x` are marked as pre-releases.

## Credits and licence
- Art, scenery, sprites and care rules are taken from [LoRagotchi-DX](https://github.com/DrToxic/LoRagotchi-DX) (MIT). See `NOTICE`.
- The plugin is GPL-2.0-or-later, like RockBox. See `LICENSE`.
