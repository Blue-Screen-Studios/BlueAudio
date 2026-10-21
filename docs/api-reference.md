# API reference

Everything is declared in `include/blueaudio/blueaudio.h`.

## Context

| Function | Description |
|---|---|
| `bau_context* bau_create(const bau_config*)` | A context. `NULL`, or any field 0, takes the defaults. |
| `void bau_destroy(bau_context*)` | Frees the context and every grid and source in it. |
| `void bau_get_config(const bau_context*, bau_config* out)` | The config, with defaults filled in. |
| `int bau_channels(const bau_context*)` | Channels per frame that `bau_render` writes. |
| `void bau_set_layout(bau_context*, bau_layout)` | Changes the output layout. Don't let it race the audio callback's `bau_channels` read; for example, hold your audio stream's lock. |
| `void bau_set_rays(bau_context*, int rays, int bounces)` | Changes the ray budget. A value of 0 keeps the current setting. |

### `bau_config`

| Field | Default | Meaning |
|---|---|---|
| `sample_rate` | 48000 | Rate of the sources' callbacks and the output. |
| `layout` | `BAU_LAYOUT_STEREO` | Output layout. |
| `rays` | 1024 | Rays fired from the listener each simulate. |
| `bounces` | 4 | Bounces per ray, at most 6. |
| `max_distance` | 90 | Metres. Sources further away are silent, and no ray segment goes further. |
| `listener_radius` | 0.25 | Metres. Solids this close to the listener are ignored. |
| `seed` | 1 | Seed for the rays. The same seed gives the same results. |

### `bau_layout`

| Value | Channels |
|---|---|
| `BAU_LAYOUT_MONO` | C |
| `BAU_LAYOUT_STEREO` | L R (speakers) |
| `BAU_LAYOUT_HEADPHONES` | L R (interaural delay, head shadow) |
| `BAU_LAYOUT_QUAD` | FL FR BL BR |
| `BAU_LAYOUT_SURROUND_51` | FL FR C LFE BL BR |
| `BAU_LAYOUT_SURROUND_71` | FL FR C LFE BL BR SL SR |

## World

| Function | Description |
|---|---|
| `bau_material bau_material_preset(bau_material_kind)` | A preset: `GENERIC`, `CONCRETE`, `BRICK`, `METAL`, `GLASS`, `WOOD`, `GROUND`, `FOLIAGE`, `CARPET`, `MESH`, `WATER`. |
| `void bau_set_material(bau_context*, uint8_t index, bau_material)` | Material for voxels holding `index` (1..255). |
| `bau_grid* bau_grid_create(bau_context*, int nx, int ny, int nz, float cell, const uint8_t* cells)` | A voxel grid with `cell` m sides, indexed `cells[x + nx * (y + ny * z)]`. The cells are copied; `NULL` gives all air. |
| `void bau_grid_update(bau_grid*, int x0, int y0, int z0, int sx, int sy, int sz, const uint8_t* cells)` | Replaces a box of cells. |
| `void bau_grid_set_pose(bau_grid*, const float origin[3], float yaw)` | Places cell 0's low corner at `origin`, turned `yaw` radians about y. Cheap enough for every frame. |
| `void bau_grid_destroy(bau_grid*)` | Removes the grid. |
| `int bau_trace_ray(bau_context*, const float origin[3], const float dir[3], float max_distance, bau_hit* hit)` | One ray through the world: the nearest solid it hits. |

### `bau_material`

| Field | Meaning |
|---|---|
| `absorption[3]` | Low, mid and high: the share of what hits it that is soaked up (0..1). |
| `scattering` | 0 reflects like a mirror; 1 scatters every way. |
| `transmission[3]` | Low, mid and high: the share let through one cell of it. |

### `bau_hit`

`distance` (m), `point[3]`, `normal[3]` (facing back along the ray), `material`.

## Listener

| Function | Description |
|---|---|
| `void bau_set_listener(bau_context*, const float pos[3], const float ahead[3], const float up[3])` | Position and facing. `up` is squared to `ahead`. |

## Sources

| Function | Description |
|---|---|
| `bau_source* bau_source_create(bau_context*, bau_fill_fn, void* user)` | A source. Its callback is called from `bau_render`. |
| `void bau_source_set_position(bau_source*, const float pos[3])` | Where it is. |
| `void bau_source_set_gain(bau_source*, float)` | Its loudness up close. Default 1. |
| `void bau_source_set_rolloff(bau_source*, float ref, float max)` | Full gain within `ref` m, inverse square past it, faded to nothing at `max`. Defaults are 7 and `max_distance`. |
| `void bau_source_set_radius(bau_source*, float)` | Solids within this many metres of the source are ignored. Default 0.5. |
| `void* bau_source_user(const bau_source*)` | The `user` pointer it was created with. |
| `int bau_source_finished(const bau_source*)` | Its callback has ended and its delayed tail has played out. |
| `void bau_source_destroy(bau_source*)` | Removes it. Its callback is never called again after this returns. |

`typedef int (*bau_fill_fn)(void* user, float* mono, int frames)` fills `frames` mono samples and returns how many it filled. Fewer than `frames` means the source is finished.

## Simulate and render

| Function | Description |
|---|---|
| `void bau_simulate(bau_context*)` | Traces the rays and publishes, blended with what was published before. |
| `void bau_render(bau_context*, float* out, int frames)` | The final mix: `frames × bau_channels` floats, interleaved, overwriting `out`. |

## Read back

| Function | Description |
|---|---|
| `void bau_source_heard(const bau_source*, bau_heard*)` | How a source is heard. |
| `void bau_listener_echo(const bau_context*, bau_echo*)` | The listener's echo. |
| `int bau_debug_rays(const bau_context*, bau_ray* out, int max)` | Up to `max` of the last simulate's paths that found a source. |
| `int bau_debug_paths(bau_context*, const bau_path_config*, bau_path* out, int max)` | Paths to draw: a fan of rays fired from the listener now, with branches to the sources and back to the listener. See below. |
| `void bau_path_sample(const bau_path*, float distance, float pos[3], float energy[3])` | The position and energy a given distance along a path, interpolated between its points. |
| `void bau_path_color(bau_path_kind, const float energy[3], float rgba[4])` | A colour for what a path carries. Brightness is the energy left; cool white means every band and orange means the highs are gone. FOUND paths are tinted gold and ECHO paths violet. |
| `void bau_get_stats(const bau_context*, bau_stats*)` | Counts and timings. |

### `bau_heard`

| Field | Meaning |
|---|---|
| `simulated` | Traced at least once. |
| `distance` | Straight distance in metres. |
| `occluded` | Something solid is on the straight line. |
| `muffle` | 0 is clear; 1 is all but gone. Drives the low-pass. |
| `reach` | Fraction of rays that found it. |
| `direct`, `reflected`, `gain` | Gains: straight through, off the walls, and both together with rolloff. |
| `direction[3]` | World direction from the listener that it is heard from. |
| `spread` | 0 is all from that direction; 1 is all round. |
| `delay` | Seconds. |

### `bau_echo`

| Field | Meaning |
|---|---|
| `level` | 0..1, how much comes back. Sets the reverb's level. |
| `decay` | RT60 in seconds. |
| `enclosure` | 0 is open sky; 1 is shut in. |
| `free_path` | Mean metres between bounces. |
| `direction[3]`, `focus` | Where most of it comes back from, and how much from there. |
| `taps`, `tap_delay[4]`, `tap_gain[4]`, `tap_direction[4][3]` | Distinct echoes. |

### `bau_ray`

`source` (identity only), `points`, `at[BAU_RAY_POINTS][3]` (the listener, each bounce, then the source), `gain`.

### Debug paths

`bau_debug_paths` is for drawing how sound moves through the world, slowed down so it can be followed by eye. It uses the same world, materials and energy sums as the simulation, but its rays bounce like a mirror, with no random scattering, so the picture is steady from one call to the next.

`bau_path_config`:

| Field | Default | Meaning |
|---|---|---|
| `rays` | 40 | Rays in the fan. |
| `bounces` | 5 | Bounces per ray, at most `BAU_PATH_POINTS − 1`. |
| `reach` | 40 | Metres a ray travels between bounces, at most. |
| `turn` | 0 | Radians the fan is turned about up. Change it each call for a different fan. |
| `flat` | 0 | 1: the fan lies flat at the listener's height, for a view from above. Bounces stay level, and sources are tested at that height. 0: rays go every way. |
| `min_energy` | 0.03 | A ray stops once its three bands sum below this. |

Each `bau_path`:

| Field | Meaning |
|---|---|
| `kind` | `BAU_PATH_OUT` (from the listener), `BAU_PATH_FOUND` (a bounce to a source it can see), or `BAU_PATH_ECHO` (a bounce back to the listener). |
| `parent`, `start` | For branches: the index of the OUT path they leave, and how many metres along it they leave. To animate, draw a branch's head at the parent's head minus `start`. OUT paths have a `parent` of −1. |
| `source` | For FOUND paths: which source. Identity only. |
| `points`, `at`, `distance`, `energy` | Each point's world position, its distance along the path, and the energy carried on from it (low, mid, high; 1 is what it left with). |
| `open_end` | The last point is open air, where the ray ran out, not a bounce. |

Branches come right after their OUT path in `out`. When `max` is reached, the remaining paths are dropped.

### `bau_stats`

`grids`, `sources`, `bricks` (non-empty 4×4×4 blocks), `rays`, `bounces`, `shadow_rays` (last simulate), `simulate_ms` (last simulate).
