# bake.md — Making the microclimate *really real* (offline bake path)

This is the high-fidelity counterpart to the realtime solver. Realtime trades physics
for frame budget; the bake spends hours to get a result that is *physically faithful* to
**this** terrain — so when you scrub through it you are looking at weather that genuinely
belongs to the mountain you generated, not a generic plume demo.

The realtime mode is the *driver's seat*. The bake is the *wind tunnel*.

---

## 0. What "really real" means here

A bake is real when every visible phenomenon is a **consequence**, never a texture:

- Clouds form because moist air was *lifted* over the windward face until it hit its
  lifting condensation level — not because a noise field said "cloud here."
- The north face holds snow because it received less integrated solar energy over the
  diurnal cycle — computed from actual ray-cast shadows against the heightmap.
- Valley fog at dawn exists because radiative cooling drained cold air downslope into a
  basin where the air saturated — katabatic flow + IR cooling + saturation, all coupled.
- Rain falls harder on the windward slope and the lee is dry because the moisture budget
  was actually depleted crossing the ridge.

Nothing is painted. You set the **initial state and the inflow**, press bake, and the
microclimate is whatever the equations say it must be. That is the whole point.

---

## 1. Domain & grid

Inherit the existing world so the bake lands exactly on your terrain.

- Horizontal extent: **165 m × 165 m** (`kTerrainWorldSize`), matching the heightmap.
- Heightmap: **193 × 193** (`kTerrainSize`), ~0.86 m post spacing. The atmosphere grid
  does **not** have to match the terrain grid — decouple them.
- **Atmosphere grid (production):** `256 × 256 × 192`, **terrain-following (Gal–Chen)**
  vertical coordinate. ~12.6M cells.
  - Horizontal: ~0.65 m.
  - Vertical: stretched — **~0.4 m at the surface** (resolve the surface layer) growing to
    **~3 m** at a **~300 m** lid. The lid sits well above the highest peak so lee waves and
    cap clouds have room.
- **Scratch grid (physics dev):** `128 × 128 × 96` (~1.6M cells). 8× cheaper. Use this for
  every iteration; only run the production grid for "real" bakes.

**Gal–Chen, not sigma-pressure.** Over 300 m of vertical, pressure is nearly constant;
sigma-pressure buys nothing and complicates the metric terms. Height-based terrain-following
gives clean coordinate surfaces that hug the terrain near the ground and flatten toward the
lid. Store the Jacobian/metric terms (∂z/∂ζ, slope-induced cross terms) per column once at
bake start — they never change.

**Why a separate, finer atmosphere grid is worth it.** The realtime path can share the
terrain grid; the bake should not. Sharp orographic lift, lee waves, and the surface layer
all live at sub-meter scales that a 193 grid smears.

---

## 2. Governing equations — fully compressible, 3D

Solve the **fully compressible Navier–Stokes + thermodynamics** in 3D (the realtime path
uses Boussinesq; the bake does not approximate density away). Prognostic variables, per cell:

| Symbol        | Quantity                                  |
|---------------|-------------------------------------------|
| ρ             | dry air density                           |
| ρu, ρv, ρw    | momentum (3 components)                   |
| ρθ            | density × potential temperature           |
| ρq_v          | water vapor mixing ratio                  |
| ρq_c          | cloud water                               |
| ρq_r          | rain                                      |
| ρq_i          | cloud ice                                 |
| ρq_s          | snow                                      |
| (ρq_g)        | graupel (optional 6th species)            |

That's **~11 prognostic floats/cell**. Pressure is diagnostic via the equation of state
(ideal gas with moisture loading), not prognostic.

- **No Coriolis.** Rossby number at 165 m is ~hundreds — Coriolis is a rounding error.
- **No hydrostatic assumption.** Non-hydrostatic is mandatory: orographic lift at meter
  scale is the entire show.
- **Buoyancy** from the full density anomaly including moisture loading and latent heat,
  so condensation actually drives updrafts.
- Equation of state carries **virtual potential temperature** so humid air is correctly
  lighter than dry air.

---

## 3. Turbulence — true LES

This is what separates the bake from the realtime mode (which only models the *mean* flow).

- **Dynamic Smagorinsky–Lilly** (Germano procedure) for the sub-grid stress, or a TKE-1.5
  closure. Dynamic is preferred: it computes the Smagorinsky coefficient locally from the
  resolved field instead of hand-tuning it, so it behaves correctly near walls and in
  stable nocturnal layers (where a constant coefficient over-mixes and kills your cold
  pools).
- At ~0.65 m horizontal you **resolve** the energy-containing eddies. The payoff you cannot
  get in realtime: gust fronts, rotors downwind of peaks, vortex shedding off sharp ridges,
  turbulent entrainment at cloud edges.

---

## 4. Microphysics — Thompson (or Morrison) 5–6 species

Single-moment Kessler (realtime) only knows vapor/cloud/rain. The bake runs a real scheme.

- **Thompson aerosol-aware** is the recommended starting point: well documented, reference
  implementations exist, double-moment for rain & ice.
- Species: vapor, cloud water, cloud ice, rain, snow (+ optional graupel).
- Gives you, *for free* and *physically*:
  - Ice-phase clouds that look different from liquid (glaciated anvils, virga).
  - Riming, snow aggregation, melting layer.
  - Real precipitation onset and a moisture budget that depletes crossing the ridge →
    genuine rain shadow.
- Saturation adjustment with latent heat release fed back into θ → into buoyancy → into the
  dynamics. The loop is closed.

**Sedimentation** (falling rain/snow/graupel) uses fall-speed–weighted flux. Consider
seeding **Lagrangian precip particles** at the surface for the *visual* layer (Niels747's
trick) while the grid carries the *budget* — particles are prettier and give honest
precipitation-driven downdrafts, the grid conserves mass.

---

## 5. Radiation

A full two-stream RTM (RRTMG) is a ~3k-line subproject and over-engineered for 300 m of
clear-ish air with no meaningfully varying trace gases. The bake uses the **honest cheap**
version that still captures everything you'll *see*:

- **Direct shortwave** with **topographic shadowing**: ray-cast the sun direction against
  the heightmap per surface cell. This is the single most important "mountain" term — it is
  what makes east faces warm at sunrise and valleys stay shadowed. Recompute only when the
  sun moves (cache between sun steps).
- **Slope–aspect cosine** weighting of incident flux (a steep south face intercepts far
  more energy than flat ground).
- **Cloud shadowing**: integrate cloud optical depth along the solar column so a cloud
  actually shades the ground beneath it (drives convective gust fronts, suppresses local
  heating). This *is* worth doing in the bake even though realtime skips it.
- **Longwave**: column IR cooling + surface emission (Stefan–Boltzmann) with a
  **sky-view-factor** correction in steep valleys (a narrow valley floor sees little cold
  sky, so it cools less — except clear flat basins cool hard → frost). View factors are
  pure geometry; bake them once from the heightmap.

The diurnal cycle is driven by an actual solar ephemeris (date + latitude → sun path), so
the asymmetry between morning and afternoon heating is real.

---

## 6. Surface energy balance + Monin–Obukhov

The ground is not a boundary condition — it is a **coupled energy budget** per surface cell:

```
net radiation = sensible + latent + ground heat + (snow melt/freeze)
```

- Surface types from the existing material masks: **snow / rock / vegetation / standing
  water** (lakes carved by erosion are real water surfaces with their own albedo & high heat
  capacity).
- Per-type **albedo, roughness length, emissivity, heat capacity, moisture availability.**
- **Monin–Obukhov similarity** (iterative, not bulk) for momentum/heat/moisture fluxes —
  this is the realtime path's bulk formula upgraded to the proper stability-dependent solve.
  It is what makes **anabatic** (morning upslope) and **katabatic** (nocturnal drainage)
  winds emerge correctly instead of being faked.
- A **multi-layer snowpack** (not just a mask): accumulation, densification, albedo aging,
  melt. Snow albedo feedback closes onto the radiation budget — fresh snow reflects, raises
  local albedo, stays cold, persists. Old snow darkens and melts.

This block is where the *microclimate* lives. Get it right and the slope winds, fog, frost,
and snow asymmetry all fall out without being asked for.

---

## 7. Time integration — IMEX + multigrid

Compressible flow has fast acoustic modes that would force a punishing explicit timestep.

- **IMEX (implicit-explicit) split**: treat acoustic + fast gravity-wave terms **implicitly**
  (a pressure/Helmholtz solve each step), everything else (advection, microphysics, SGS,
  radiation source terms) **explicitly**. This decouples dt from the speed of sound.
- **Pressure solve: geometric multigrid** (V-cycle) on the terrain-following grid. Jacobi
  (realtime) is too slow to converge at 12M cells; multigrid is mandatory here.
- **Advection: high-order, dispersion-respecting** — 5th-order WENO or a flux-limited scheme.
  This is *required* for lee waves and lenticular clouds; semi-Lagrangian (realtime) is too
  diffusive and smears them out. Scalars (moisture, θ) can use a positivity-preserving WENO
  so mixing ratios never go negative.
- **No sub-grid convection parameterization** (no Kain–Fritsch). Convection is *resolved* at
  meter scale — parameterizing it would be wrong.
- Sub-step microphysics and acoustic terms on their own faster clocks where needed.

---

## 8. GPU implementation on M1 Pro / MoltenVK / Metal

You're on Apple Silicon (M1 Pro, 14–16-core GPU, ~5 TFLOPS fp32, unified memory) talking to
Metal through MoltenVK. Constraints and how the bake respects them:

- **fp32 only.** Metal has no fp64. Compressible solvers can be fp32-sensitive (pressure
  perturbations are small relative to the base state). **Mitigation:** solve for *deviations*
  from a hydrostatic reference state (subtract the mean profile so the solver only sees the
  perturbation — keeps the bits where they matter). This is standard practice and essential
  on fp32.
- **Unified memory is an advantage for baking:** snapshots can be written to the same memory
  the renderer reads, no PCIe copy. A 12.6M-cell field at 11 fp32 = ~555 MB lives comfortably
  in M1 Pro's shared RAM.
- **Everything is a compute shader.** Today the project has **zero compute infrastructure** —
  the Vulkan work so far is graphics only (sky/terrain/particle draw in
  `src/vulkan_app.h`). The bake (and realtime) both need a compute pipeline scaffold built
  first: storage buffers for the SoA fields, descriptor sets, dispatch + barrier plumbing.
- **Struct-of-arrays layout.** One buffer per field (ρ, ρu, …), not an array of structs.
  Memory bandwidth, not flops, is the bottleneck on this GPU — coalesced reads matter more
  than anything.
- **Multigrid in compute** is the hard part: each V-cycle level is its own dispatch, with
  restriction/prolongation kernels between them. Budget real time for this; it is the single
  most complex kernel in the system.
- The bake does **not** need to fit a frame budget, so you can run many solver substeps
  between snapshots and let a step take 50–200 ms.

---

## 9. The bake pipeline (record → store → scrub)

This is what turns a solver into a thing you *explore*.

**Snapshot.** Every *N* sim-seconds, write a frame: all prognostic fields + key diagnostics
(see §10). Raw frame ≈ 555 MB; never store raw.

**Compression.**
- Store **deltas** against the previous frame (the atmosphere changes slowly between close
  snapshots) → typically 5–15 MB/frame after zstd.
- Quantize for storage: θ and winds to fp16, mixing ratios to a log-quantized 8–12 bit
  (mixing ratios span orders of magnitude). The *solver* stays fp32; only the *archive* is
  quantized.
- A **keyframe every K frames** (full fp16 dump) so the scrubber can seek without replaying
  from t=0.

**Storage budget.** 1 sim-hour at 1 snapshot / sim-minute = 60 frames ≈ **0.4–1 GB**. A
day-cycle bake (24 sim-hours, snapshot/2-min) ≈ 700 frames ≈ **4–10 GB**. Fine on disk.

**Manifest.** A small JSON/header describing grid, metric terms, surface masks, sun ephemeris,
inflow profile, and the frame index → byte-offset table. Self-describing so a bake is a
single portable artifact.

---

## 10. "Probes on a world — see everything"

The bake's job is to let you interrogate the volume. Two layers:

**A. 2D-slice scrubber (the primary viewer).** Fully 3D simulation, but you inspect it by
slicing:
- **Three orthogonal slice planes** (XY/XZ/YZ) with sliders to drag each plane through the
  volume — including a terrain-following horizontal slice ("N meters above ground") which is
  far more useful in mountains than a flat-z slice.
- **Field selector** per slice: θ, vertical velocity w (updraft/downdraft map — this is the
  money view), wind speed, vapor, cloud water, cloud ice, rain, RH, vorticity magnitude,
  TKE, divergence.
- **Time slider** scrubbing the baked frames — forward, backward, scrub, loop. This is why
  you baked: instant random-access time travel through the weather.
- Volumetric cloud render (ray-march q_c + q_i for the pretty view) layered over the terrain
  you already render, with the slice planes composited in.

**B. Point/line probes (the "everything about it" view).**
- Click a world point → a **time-series panel** of every variable at that cell over the whole
  bake (a virtual weather station / sounding).
- **Vertical sounding** at a clicked column: skew-T-style θ/T/dewpoint/wind-with-height — read
  off stability, the LCL, inversions, the cold-pool depth directly.
- **Streamlines / tracer release**: drop a tracer at a point and watch the baked flow carry it
  (visualizes channeling through passes, drainage into valleys).
- **Surface dashboards**: maps of accumulated precip, integrated solar energy per cell,
  snow-water-equivalent, min/max temperature — the *climatology* of the bake, which is exactly
  "the microclimate of this terrain."

Diagnostics (vorticity, divergence, TKE, RH, LCL, etc.) are computed at **bake time** and
stored, so the scrubber stays instant — no recompute on seek.

---

## 11. Coupling back to terrain & erosion

The bake closes the loop with the geology you already simulate:

- **Precipitation field** from microphysics replaces the uniform-random droplet spawn in
  `Terrain` — drops are seeded weighted by where rain/snow actually fell. The existing
  `water_`/`sediment_` grids and the `hydro` vertex channel become the *deposition target* of
  real weather.
- **Snowpack** from §6 drives the rendered snow line and snow albedo — north-face/lee-drift
  asymmetry shows up on the actual mesh, not a flat `snowLevel` threshold.
- **Freeze–thaw** weathering where the surface temperature crosses 0 °C repeatedly (frost
  shattering on exposed rock) feeds the erosion rate.
- **Wind shear** at the surface drives aeolian transport on exposed ridges.

You can run a long bake and then "apply" its integrated precipitation/weathering to advance
the terrain a geological step — weather literally sculpts the mountain.

---

## 12. Validation (don't skip this)

The bake is worthless if it's wrong-but-pretty. Validate each layer against a known answer
before trusting it on your terrain:

1. **Flow over a bell-shaped hill** (Agnesi witch) → compare lee-wave wavelength/amplitude to
   the published linear-theory solution. Validates dynamics + advection + terrain coordinate.
2. **Dry convective boundary layer** (uniform surface heating, no terrain) → check the
   boundary-layer growth rate and convective cell spacing against LES literature. Validates
   buoyancy + SGS + surface flux.
3. **Saturated rising thermal** (warm moist bubble) → cloud forms at the right height, latent
   heat accelerates the updraft. Validates microphysics coupling.
4. **Idealized slope, clear sky, diurnal sun** → katabatic flow at night, anabatic by morning,
   of the right depth and timing. Validates radiation + surface + the whole point.

Only after these pass do you bake the real heightmap.

---

## 13. Phases (bake path)

These assume the realtime solver may or may not exist yet; the bake shares the compute
scaffold and viewer with it.

0. **Compute scaffold** — SoA storage buffers, descriptor sets, dispatch/barrier plumbing,
   a 3D field allocator. (Shared with realtime. Nothing exists today; this is foundational.)
1. **Compressible NS + multigrid pressure + Gal–Chen grid.** Validate on the bell hill (§12.1).
2. **Dynamic Smagorinsky SGS + inflow boundary conditions** (prescribed, optionally
   time-varying wind/θ/humidity profile to drive a front through). Validate on the dry CBL.
3. **Moisture + Thompson microphysics + saturation adjustment + sedimentation.** Validate on
   the rising thermal.
4. **Radiation (solar shadowing + cloud shadow + IR view factors) + surface energy balance +
   Monin–Obukhov + snowpack.** Validate on the diurnal slope. *Microclimate appears here.*
5. **Bake recorder** — snapshot/delta/keyframe writer, manifest, compression.
6. **Scrubber + probes** — slice planes, time slider, point/column probes, soundings, surface
   dashboards (§10).
7. **Erosion coupling** — precip-weighted droplets, snowpack→render, freeze-thaw, aeolian (§11).

---

## 14. Wall-time expectations (M1 Pro)

Rough, single-GPU, fp32:

| Grid              | Cells  | ~Step time | 1 sim-hour bake | Use                     |
|-------------------|--------|------------|-----------------|-------------------------|
| 128×128×96        | 1.6M   | ~30–80 ms  | ~20–60 min      | physics dev / scratch   |
| 256×256×192       | 12.6M  | ~0.2–0.5 s | **~2–6 hours**  | production bake         |

- Iterate on the scratch grid; bake production overnight.
- Step time is dominated by the **multigrid pressure solve** and **memory bandwidth**, not
  microphysics. Optimize those two first if it's slow.
- The M1 Pro is not a 3090 — a production bake is an overnight job, not a coffee break. That's
  the deal you accept for "really real."

---

## 15. What the bake is NOT

- **Not synoptic.** A 165 m box can't grow its own fronts or cyclones — large-scale weather
  enters through the inflow boundary (§13 phase 2). You *prescribe* the airmass; the mountain
  decides what happens to it.
- **Not validated against real Alps observations.** It will be physically self-consistent and
  look right; matching a specific real day is a separate research effort.
- **Not realtime.** That's the other solver. The bake and realtime modes share the grid,
  surface coupling, erosion plumbing, and viewer — they differ only in the solver kernels
  (compressible+WENO+multigrid+Thompson here vs Boussinesq+semi-Lagrangian+Jacobi+Kessler
  there). Build them behind one `Weather` interface; do not try to make one solver do both.

---

### TL;DR

Fully compressible 3D LES on a `256²×192` terrain-following grid, dynamic Smagorinsky,
Thompson microphysics, shadowed solar + IR radiation, a coupled surface-energy/snowpack model,
IMEX time stepping with a multigrid pressure solve — all in Metal compute via MoltenVK, in
fp32-deviation form. Record compressed snapshots, then scrub the volume with orthogonal slice
planes, a time slider, and clickable point/column probes. Overnight bake on the M1 Pro;
the microclimate of *your* mountain, emergent and interrogable.
