# MountainSim

3D weather over procedural mountains, in a Vulkan terrain renderer (C++20 / MoltenVK, macOS).

A small atmospheric solver runs live over generated terrain — wind, temperature and moisture —
so clouds and rain form from air being lifted over the peaks, drawn with ray-marched
volumetric clouds. It's a qualitative, sped-up microclimate, not a forecast model.

## Features

- Procedural terrain with layered noise and hydraulic erosion.
- A 3D Boussinesq atmosphere on a background thread: semi-Lagrangian advection, pressure
  projection, buoyancy from temperature + moisture, and warm-rain microphysics.
- Orographic clouds and rain that emerge from air rising over the terrain.
- Ray-marched volumetric clouds with terrain occlusion (billboard mode behind a toggle).
- Field slices (temperature, wind, humidity, cloud, rain, …) and ImGui control panels.

## Build & run

```bash
brew install glfw glm molten-vk vulkan-headers vulkan-loader
./script/build_and_run.sh
```

Or manually:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build --target Mountains --parallel
./build/Mountains
```

## Controls

- Left / right mouse drag — orbit; Shift + right drag or middle drag — pan; scroll — zoom.
- WASD move the target, Space / Ctrl raise / lower, Shift to move faster.
- ImGui panels handle terrain, erosion, the sun, and the weather + slice settings.
