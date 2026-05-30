# MountainSim

A real-time **3D microclimate weather simulation** running live inside a Vulkan terrain
renderer. Procedurally generated mountains are fed to a coupled atmospheric solver — wind,
temperature, and moisture — so that clouds and rain *emerge from the physics* of air being
forced up over the terrain, rendered with ray-marched volumetric clouds. Built in C++20 on
Vulkan via MoltenVK on macOS (Apple Silicon).

> This is the **spiritual predecessor** of
> [**Mountain-weather**](https://github.com/vidvudsc/Mountain-weather) — the later C / raylib
> reimplementation of the same idea. MountainSim is the original C++/Vulkan exploration that
> the concept grew out of.

---

## What it does

- **Procedural terrain** — layered Perlin noise with octaves/lacunarity/persistence controls,
  peak sharpening, and a shaded mesh (grass, alpine rock, cliff, snow, water, sediment).
- **Hydraulic erosion** — droplet-based erosion you can run live and watch carve the terrain;
  the weather's rainfall can also drive erosion where it actually rains.
- **A genuine 3D atmosphere** — not a particle effect. A collocated-grid, incompressible
  Boussinesq solver runs on a background thread over the terrain:
  - semi-Lagrangian advection of velocity and scalars,
  - a red–black Gauss–Seidel **SOR pressure projection** (divergence-free flow that bends
    around the mountains),
  - **buoyancy** from potential-temperature *and* moisture perturbations,
  - **warm-rain (Kessler) microphysics** — condensation with latent heating, cloud→rain
    autoconversion, rain evaporation and sedimentation,
  - adiabatic cooling on lift (Exner function) — the trigger for **orographic clouds**,
  - shadowed solar heating / IR cooling driving slope and valley winds,
  - open lateral inflow/outflow boundaries so the airmass flows straight through.
- **Volumetric clouds** — a fullscreen **GPU ray-march** through the live cloud-water field
  (3D density sampled per pixel), with a secondary march toward the sun for self-shadowing and
  analytic terrain occlusion (clouds behind ridges are hidden). A legacy billboard mode is
  kept behind a toggle.
- **Inspection tools** — continuous gouraud-shaded horizontal + vertical field slices
  (temperature, vertical wind, humidity, cloud, rain, vorticity, …) and wind streaks.

It is a physically *motivated, qualitative* atmosphere — a believable sped-up microclimate,
not a calibrated forecast model. See the inline notes in [`src/weather.h`](src/weather.h) for
where it is faithful and where it approximates.

## Stack

- **C++20**
- **Vulkan / MoltenVK** renderer (single render pass: sky → terrain → volumetric clouds → UI)
- **GLFW** window + input
- **GLM** math
- **Dear ImGui** control panels (fetched automatically by CMake)
- **CMake** build

## Build & run

Requires the Vulkan SDK (MoltenVK) plus `glfw3` and `glm` (e.g. via Homebrew):

```bash
brew install glfw glm molten-vk vulkan-headers vulkan-loader
```

Then:

```bash
./script/build_and_run.sh        # configure, build, and run
```

Or manually:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build --target Mountains --parallel
./build/Mountains
```

## Controls

- **Left / right mouse drag** — orbit the camera
- **Shift + right drag**, or **middle drag** — pan
- **Scroll / two-finger swipe / pinch** — zoom (also in the Camera window's *Orbit distance*)
- **WASD** — pan the look target; **Space / Ctrl** — raise / lower it; **Shift** — move faster
- The ImGui windows expose everything else: regenerate terrain, run erosion, move the sun, and
  drive the **Weather** panel (run/pause/reset, wind, humidity, stability, grid resolution) and
  the **Slices & view** panel (clouds incl. volumetric density/steps, rain, wind, slice planes).

## How it's put together

| File | Role |
|------|------|
| [`src/weather.h`](src/weather.h) | The 3D atmosphere: solver, microphysics, background worker thread, immutable snapshots, and a persistent thread pool for `parallelFor`. |
| [`src/terrain.h`](src/terrain.h) | Procedural heightmap generation + hydraulic erosion + mesh. |
| [`src/vulkan_app.h`](src/vulkan_app.h) | Vulkan setup, render passes/pipelines, ImGui UI, field-slice overlay, and the volumetric cloud pass. |
| [`shaders/cloud.frag`](shaders/cloud.frag) | The volumetric cloud ray-marcher. |
| [`src/common.h`](src/common.h), [`src/camera.h`](src/camera.h), [`src/perlin.h`](src/perlin.h) | Shared types, orbit camera, noise. |

The solver runs on its own thread and publishes immutable snapshots, so the render/camera
framerate stays smooth and independent of grid resolution. The pressure Poisson stencil is
precomputed per terrain change and swept with a barrier-synchronized thread pool, which lets
the CPU solver handle well over a million cells in real time.

## Assets

- **Skybox:** *Miramar* by Jockum Skoglund (hipshot), via OpenGameArt under CC BY 3.0.
- **Terrain surface:** procedural color blending (grass, alpine rock, cliff, snow, water,
  sediment); texture sampling is intentionally disabled pending art direction.
