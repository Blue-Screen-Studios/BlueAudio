# BlueAudio

**Ray-traced game audio mixing in C.**

BlueAudio works out how a game's world sounds from where the player stands. It fires rays from the listener, bounces them off the world's voxels, and turns what they find into a final mix with one waveform for each output channel.

BlueAudio does not play anything and holds no audio data. The game declares sources, each with a callback that fills its waveform, and hands over its world as voxel grids. BlueAudio returns the mixed channels for the game to play through its own audio device.

| In | Out |
|---|---|
| **Sources:** a callback per source that fills mono samples, plus its position | **The final mix:** `bau_render` fills interleaved channels (mono, stereo, headphones, quad, 5.1 or 7.1) |
| **The world:** voxel grids with a material per cell, each grid posed and movable | **What was heard:** each source's muffle, direction, gain and delay, plus the listener's echo (`bau_source_heard`, `bau_listener_echo`) |
| **The listener:** position and facing | **Debug paths:** a fan of rays to draw, with each bounce's position and the energy it carries, plus branches to the sources and back to you (`bau_debug_paths`) |

## What it works out

- **Muffle.** How much of a source's sound reaches the listener, from the rays that find it and from what comes straight through walls. Each material lets through its own share in three bands, and highs always go first. A muffled source loses its high frequencies as well as volume.
- **Direction.** Where each sound is heard from. Rays leave the listener in every direction; when a bounce can see a source, that sound is heard from the direction the ray *first* went. Music round a corner is heard from the corner, not through the wall.
- **Echo.** Rays that bounce back to the listener tell BlueAudio how enclosed the place is, how long it rings (RT60), and whether a distant wall answers back with a distinct echo. Every source's sound goes through a reverb tuned to these, leaning toward where the echo came from.
- **Distance and delay.** Each path's length becomes a delay, smoothly eased so a moving source gets Doppler for free. Air absorbs highs over distance.

## Features

- **Voxel worlds.** Grids are stored sparsely in 4×4×4 bricks, and rays skip empty bricks. A grid can be moved and turned every frame (a vehicle) or edited in place (a wall knocked down).
- **Materials.** Absorption, transmission and scattering in three bands, with presets for concrete, brick, metal, glass, wood, ground, foliage, carpet, mesh and water.
- **Output layouts.** Mono, stereo speakers, headphones (each ear with its own delay and head shadow), quad, 5.1 and 7.1.
- **Stable.** Each simulate turns its rays randomly, then blends the results with the previous ones, so the sound settles and doesn't jitter. A fixed seed gives identical results.
- **Thread friendly.** Simulate on a worker thread and render on the audio thread. A source's callback is never called again once `bau_source_destroy` returns.
- **C11 with no dependencies.** A single shared library exporting only `bau_*` symbols, with headers that work from C and C++.

## Quick start

```c
#include <blueaudio/blueaudio.h>

bau_context* au = bau_create(NULL);  /* 48 kHz stereo, 1024 rays, 4 bounces */
bau_set_material(au, 1, bau_material_preset(BAU_MATERIAL_CONCRETE));

/* The world: nx * ny * nz cells, 0.2 m each, cells[x + nx * (y + ny * z)] a material (0: air). */
bau_grid* level = bau_grid_create(au, nx, ny, nz, 0.2f, cells);

/* A source: BlueAudio pulls its waveform when it renders. */
static int fill_engine(void* user, float* mono, int frames) {
  engine_synth(user, mono, frames);
  return frames;  /* fewer than asked: finished */
}
bau_source* engine = bau_source_create(au, fill_engine, &my_engine);

/* Every frame (the game's thread): */
bau_set_listener(au, camera_pos, camera_ahead, camera_up);
bau_source_set_position(engine, engine_pos);

/* About 30 times a second (any one thread, e.g. a worker): */
bau_simulate(au);

/* The audio device's callback: */
bau_render(au, out, frames);  /* frames * bau_channels(au) floats, interleaved */
```

## Building

Requirements:

- CMake 3.21 or newer
- A C11 compiler (GCC, Clang or MSVC)
- For the demo only: SDL3, [BlueUI](https://github.com/Blue-Screen-Studios/BlueUI), and network access on first configure to download dr_mp3

```sh
cmake -B build
cmake --build build
ctest --test-dir build        # run the test suite
./build/blueaudio_demo        # walk about a level and hear it (SDL3)
```

| Option | Default | Description |
|---|---|---|
| `BLUEAUDIO_SHARED` | `ON` | Build `libblueaudio.so`. Turn off for a static library. |
| `BLUEAUDIO_BUILD_DEMO` | `ON` when top level | Build `blueaudio_demo`. Requires SDL3 and BlueUI. |
| `BLUEAUDIO_BUILD_TESTS` | `ON` when top level | Build `blueaudio_tests` and register it with CTest. |

To use BlueAudio in a game, add it with `add_subdirectory` or `FetchContent` and link the `blueaudio` target. See [Integration](docs/integration.md).

## The demo

`blueaudio_demo` is a level seen from above at a slant, with a white capsule you walk with WASD. Music plays from one speaker and a chime rings from another. Pick a room with 1–6 or from the panel: an open field, a concrete hall, corridors, a room next door, a canyon, or a glass box.

The rays drawn come from BlueAudio's `bau_debug_paths`, slowed down and kept flat at ear height so they can be followed by eye. The simulation's own rays go in every direction at the speed of sound. Each drawn ray carries its energy in three bands and changes colour as it goes:

- **Brightness:** the energy it has left.
- **Cool white to orange:** the highs being soaked up, by carpet or earth for example.
- **Gold:** a bounce that found a source.
- **Violet:** sound coming back to you, the echo.

The arrows show BlueAudio's answer: where each source is heard from.

The demo's panel, with its narrator, is built with [**BlueUI**](https://github.com/Blue-Screen-Studios/BlueUI), our accessible, self-narrating game UI library. BlueUI is a dependency of the demo only, not of BlueAudio. The demo uses the BlueUI repository beside this one (`../BlueUI`) if it is there, and fetches it otherwise.

## Documentation

| Guide | Contents |
|---|---|
| [Getting started](docs/getting-started.md) | A complete setup: world, sources, the simulate thread, the audio callback. |
| [Concepts](docs/concepts.md) | How the rays work out muffle, direction and echo; how the mix is rendered; threading. |
| [Integration](docs/integration.md) | CMake, shared and static builds, shipping, C++ use. |
| [API reference](docs/api-reference.md) | Every type and function. |

## Project layout

```
include/blueaudio/blueaudio.h   The whole API
src/                            Implementation: world (voxels, rays), trace (simulation), render, reverb
tests/tests.c                   Test suite (CTest): synthetic worlds, what's heard, what's rendered
demo/                           The demo (SDL3, BlueUI) and its music
docs/                           Documentation
```

## License

BlueAudio is released under the [MIT License](LICENSE.md).

## Acknowledgements

- The demo's music is **Music by Joystock - https://www.joystock.org** ("Corporate Pop"). It is compiled into the demo only, not into the library.
- The demo decodes MP3 with [dr_mp3](https://github.com/mackron/dr_libs) by David Reid (public domain / MIT No Attribution).
- The demo's panel uses [BlueUI](https://github.com/Blue-Screen-Studios/BlueUI) (MIT), whose voice is CMU's [Flite](https://github.com/festvox/flite).
