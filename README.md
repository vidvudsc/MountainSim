# Mountains

Native C++ terrain lab inspired by the referenced terrain-generation video, rendered with Vulkan through MoltenVK on macOS.

## Stack

- C++20
- Vulkan / MoltenVK renderer
- GLFW for the window and input
- GLM for vector and matrix math
- Dear ImGui for the tweakable control panel
- CMake for builds

## Run

```bash
./script/build_and_run.sh
```

## Controls

- One-finger click-drag / left mouse drag: orbit around the terrain
- Right mouse drag: orbit around the terrain
- Shift + right mouse drag or middle mouse drag: alternate pan controls
- Two-finger vertical swipe: zoom in and out
- Two-finger horizontal swipe: pan across the terrain
- Trackpad pinch: zoom in and out
- Zoom is also available in the Camera window's Orbit distance slider
- WASD: pan the target
- Space / Control: move the target up and down
- Use the separate ImGui windows to regenerate terrain, run erosion, move the sun, adjust snow, water, sediment, fog, and camera view.

## Assets

- Skybox: Miramar by Jockum Skoglund (hipshot), distributed via OpenGameArt under CC BY 3.0.
- Terrain surface: procedural color blending for grass, alpine rock, cliff rock, snow, water, and sediment. Texture sampling is intentionally disabled until a better art direction is chosen.
