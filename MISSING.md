# Missing Systems For A Real Alien Weather World

Goal: turn MountainSim from a convincing realtime weather sketch into a small physical
world in a box. The target is not a NOAA-scale forecast model. The target is an alien
mountain terrarium where terrain, air, water, snow, heat, cloud, rain, and time all have
state, memory, and measurable consequences.

## Current Baseline

MountainSim already has:

- Procedural terrain with erosion traces.
- A 3D CPU atmosphere with wind, temperature, vapor, cloud water, rain, buoyancy, pressure
  projection, slope winds, and terrain-driven lift.
- Day/night sun controls.
- Ray-marched volumetric clouds.
- Weather slices for visual inspection.
- Rain-weighted terrain erosion.

The missing work is making every visible thing correspond to physical state instead of a
shader shortcut or tuned preset.

## 1. Terrain Surface Physics

Current gap: terrain is mostly a boundary and a color source. Snow is altitude/slope color,
water is erosion display, and ground has no thermal memory.

Needed:

- Per-cell terrain material: rock, soil, grass/vegetation, sediment, standing water, snow,
  ice.
- Per-cell surface temperature.
- Subsurface temperature layers so terrain stores heat during the day and leaks it back at
  night.
- Surface wetness and soil water content.
- Snow mass, ice mass, and liquid water mass.
- Material albedo, emissivity, heat capacity, conductivity, roughness, and moisture
  availability.
- Snow/ice as physical overlays, not paint.
- Terrain shadow memory: shaded slopes stay cold because they stored less energy.
- Wetness memory after rain.

First implementation path:

- Replace fake snow color with `snowMass`.
- Add material/wetness/ice/surface-temperature fields.
- Render from surface state.
- Later feed surface state from weather and energy balance.

## 2. Surface Energy Balance

Current gap: simple surface heating/cooling acts directly on the air. The ground itself does
not conserve or store energy.

Needed:

- Incoming direct solar radiation by sun angle.
- Terrain self-shadowing.
- Diffuse sky radiation.
- Reflected sunlight from material albedo.
- Longwave emission from surface.
- Downwelling longwave from clouds/sky.
- Heat conduction into/out of subsurface.
- Sensible heat exchange with lowest air cell.
- Latent heat from evaporation/sublimation.
- Snow melt/freeze latent heat.
- Wind-dependent exchange strength.

Observable results:

- Warm rocks after sunset.
- Cold shaded basins.
- Persistent snow on cold faces.
- Stronger fog after wet days.
- Slope breezes from real heating differences.

## 3. Real Near-Surface Boundary Layer

Current gap: slope winds and drag exist, but the air-ground interface is still a simplified
forcing.

Needed:

- Surface roughness by material.
- Wind shear near the ground.
- Stability-dependent turbulent exchange.
- Calm-night decoupling near the surface.
- Daytime mixed-layer growth.
- Nocturnal stable layer.
- Anabatic upslope winds from warm slopes.
- Katabatic drainage from cooled slopes.
- Valley cold pools.
- Fog trapped below inversions.

Validation targets:

- Heated slope makes upslope flow by day.
- Cooled slope makes downslope flow by night.
- Valley basin collects cold air overnight.

## 4. Atmosphere State And Equations

Current gap: Boussinesq incompressible flow is good for a realtime toy, but the world lacks
full pressure-density-temperature consistency.

Needed:

- Physical pressure field, not only projection pressure.
- Density from pressure, temperature, and moisture.
- Altitude-consistent pressure/temperature/humidity profiles.
- Wind profiles that vary with height.
- Temperature inversions and layered stability.
- Anelastic or compressible solver path for higher fidelity.
- Terrain-following vertical coordinate instead of stair-step terrain cells.
- Better open boundaries for inflow/outflow.
- Less diffusive advection for waves and sharp features.
- Better pressure solve for high resolution.

Observable results:

- More believable lee waves and rotors.
- Better trapped cold pools.
- More realistic cloud base and layer depth.

## 5. Turbulence

Current gap: turbulence is noise forcing plus diffusion. Real mountain weather is dominated
by turbulent exchange and eddies.

Needed:

- Sub-grid turbulence model.
- Stability-dependent eddy viscosity/diffusivity.
- More turbulence over rough surfaces.
- Less turbulence in stable nighttime valleys.
- Convective thermals from heated slopes.
- Wake turbulence behind ridges.
- Cloud-edge entrainment.
- Turbulent fog erosion after sunrise.

Validation targets:

- Dry convective boundary layer over heated ground.
- Ridge wake and eddies behind a sharp crest.
- Fog layer mixing out after surface heating starts.

## 6. Moisture, Clouds, And Microphysics

Current gap: warm-rain vapor/cloud/rain is a useful start, but real cloud life needs more
species and phase changes.

Needed:

- Cloud ice.
- Snow.
- Graupel/hail if storms matter.
- Mixed-phase clouds.
- Fog droplets as near-surface cloud with different behavior.
- Droplet/aerosol number proxy.
- Condensation nuclei.
- Deposition/sublimation.
- Cloud-top radiative cooling.
- Cloud-edge evaporation and entrainment.
- Virga.
- Rain evaporation and downdraft feedback.
- Snow/rain transition from vertical temperature profile.

Observable results:

- Fog behaves differently from elevated cumulus.
- Snow appears from cold precipitation, not height.
- Rain shadows deplete moisture across ridges.
- Downdrafts and gust fronts under heavy precip.

## 7. Precipitation And Hydrology

Current gap: rain exists in the atmosphere and can seed erosion, but terrain water is not a
continuing part of the world.

Needed:

- Rain/snow reaching the surface updates liquid water, snow, or ice.
- Runoff across terrain.
- Infiltration into soil.
- Evaporation from wet ground and water bodies.
- Snow accumulation.
- Snowmelt runoff.
- Refreezing into ice crust.
- Puddles/lakes as thermal and humidity reservoirs.
- Precipitation type by vertical temperature profile.
- Long-term erosion from actual water routes.

Observable results:

- Wet valleys produce morning fog.
- Snowmelt feeds streams.
- Rain carves where storms actually happened.
- Water bodies smooth local temperature swings.

## 8. Radiation And Cloud Feedback

Current gap: sun and IR are simple controls; clouds do not strongly feed back into surface
energy.

Needed:

- Cloud optical-depth shadowing on terrain.
- Cloud longwave blanket at night.
- Sky-view factor in valleys.
- Direct vs diffuse sunlight.
- Slope/aspect solar exposure.
- Seasonal energy cycle.
- Radiative cooling of fog/cloud tops.

Observable results:

- Cloudy nights stay warmer.
- Clear basins cool fast.
- Cloud shadows suppress thermals.
- North-facing slopes keep snow longer.

## 9. Snow, Ice, And Frozen Ground

Current gap: snow is visual. Ice/frozen ground do not exist.

Needed:

- Snow water equivalent.
- Snow depth.
- Snow age and albedo aging.
- Snow density/compaction.
- Melt, refreeze, sublimation.
- Ice crust from refrozen melt/rain.
- Frozen soil layer.
- Insulation of ground by snow.
- Wind redistribution of snow.

Observable results:

- Fresh snow brightens and cools terrain.
- Old snow darkens and melts faster.
- Snow persists in shaded/cold zones.
- Meltwater appears below snowfields.

## 10. Alien World Parameters

Current gap: the world assumes Earth-like constants in places, but an alien terrain box
should expose its physical constants.

Needed:

- Gravity.
- Atmospheric composition / gas constant.
- Surface pressure.
- Stellar intensity and color.
- Day length.
- Axial tilt / season.
- Planetary humidity availability.
- Optional non-Earth freezing/boiling points for exotic fluids.
- Optional different condensable species later.

Keep the first implementation Earth-water based. Add alien constants as a clean parameter
layer, not by hardcoding weirdness everywhere.

## 11. Probes And Observability

Current state: slices already inspect fields spatially. A probe-world needs deeper time and
point interrogation.

Needed:

- Point probe at air cell or terrain cell.
- Probe readout: material, surface temp, snow, ice, wetness, albedo, air temp, pressure,
  RH, wind, cloud, rain.
- Time series for selected probe.
- Vertical sounding above a terrain point.
- Parcel tracers that follow the flow.
- Streamlines.
- Integrated maps: solar energy, precip, evaporation, min/max temp, fog depth, snow depth.
- Saveable snapshots and long timeline scrubber.

## 12. Scale, Units, And Consistency

Current gap: the visual domain is small and uses scale boosts like `vScale` to make weather
visible.

Needed:

- Decide the physical size of the box.
- Make terrain height, air column depth, wind, heat, pressure, and timestep consistent.
- Reduce or isolate scale fudge factors.
- Separate visual exaggeration from physics units.
- Define whether the sim is meter-scale LES, valley-scale, or stylized alien-scale.

## 13. Validation Worlds

Current gap: behavior is judged mostly by appearance.

Needed test cases:

- Dry flow over a bell hill.
- Heated slope daytime upslope wind.
- Cooled slope nighttime drainage wind.
- Closed basin cold pool and fog.
- Moist ridge cloud and rain shadow.
- Rising warm bubble forming cloud.
- Rain shaft causing downdraft.
- Snow accumulation/melt/refreeze over a day.
- Cloud shadow suppressing thermals.

## Suggested Build Order

1. Physical terrain surface state.
2. Snow/water/ice state replacing fake snow color.
3. Surface temperature and material thermal properties.
4. Surface-air heat and moisture exchange.
5. Snow/rain phase coupling from weather to terrain.
6. Valley cold pools and stable boundary-layer behavior.
7. Cloud/fog distinction and better cloud-edge mixing.
8. Radiation feedback from clouds and sky view.
9. Probe time series and vertical soundings.
10. Higher-fidelity solver path only after the world-state plumbing is real.

