#!/usr/bin/env bash
# Downloads the CC0 Poly Haven ground textures the terrain shader expects (1K JPG).
set -euo pipefail
cd "$(dirname "$0")/../assets/textures"
for n in aerial_rocks_02 aerial_rocks_04 rocky_terrain_02 forrest_ground_01 snow_02 aerial_grass_rock; do
  for k in diff nor_gl; do
    f="${n}_${k}.jpg"
    [ -f "$f" ] || [ -f "${n}_${k}.png" ] || curl -sSL -o "$f" "https://dl.polyhaven.org/file/ph-assets/Textures/jpg/1k/${n}/${n}_${k}_1k.jpg"
  done
done
ls -la
