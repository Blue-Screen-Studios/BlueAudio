# Integration

## Adding BlueAudio to a CMake project

### As a subdirectory

```cmake
set(BLUEAUDIO_BUILD_DEMO OFF CACHE BOOL "" FORCE)
set(BLUEAUDIO_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(../BlueAudio blueaudio)
target_link_libraries(my_game PRIVATE blueaudio)
```

### With FetchContent

```cmake
include(FetchContent)
FetchContent_Declare(
  blueaudio
  GIT_REPOSITORY <repository URL>
  GIT_TAG        <tag or commit>
)
FetchContent_MakeAvailable(blueaudio)
target_link_libraries(my_game PRIVATE blueaudio)
```

When BlueAudio is not the top-level project, its demo and tests are not built.

## Shared or static

`BLUEAUDIO_SHARED` (on by default) builds `libblueaudio.so` (`blueaudio.dll` on Windows) with only the `bau_*` symbols exported. Ship it beside the game's executable. With CMake:

```cmake
add_custom_command(TARGET my_game POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_SONAME_FILE:blueaudio> $<TARGET_FILE_DIR:my_game>)
set_target_properties(my_game PROPERTIES BUILD_RPATH "$ORIGIN")
```

Set `BLUEAUDIO_SHARED OFF` before adding it to link it statically.

## Shipping

BlueAudio is MIT licensed. Credit it, and ship its `LICENSE.md` with the game. It has no third-party code. The demo's music, dr_mp3 and BlueUI belong to the demo only and are never linked into the library.

## From C++

The header is wrapped in `extern "C"`. Structs zero any field a brace initialiser leaves out (`BAU_ZERO`), so `bau_config c{}; c.rays = 512;` works.

A typical wrapper keeps the context, the grids built from the level, and a simulate thread:

```cpp
struct Soundscape {
  bau_context* au = bau_create(nullptr);
  std::thread worker;
  std::atomic<bool> running{true};
  Soundscape() : worker([this] {
    while (running) {
      bau_simulate(au);
      std::this_thread::sleep_for(std::chrono::milliseconds(33));
    }
  }) {}
  ~Soundscape() { running = false; worker.join(); bau_destroy(au); }
};
```

## Mixing with sounds outside the world

`bau_render` overwrites its buffer with the world's mix. Sounds that don't belong in the world, such as the UI, a narrator or a cockpit voice, are added afterwards by the game, followed by its volume and limiter.

## Performance

A simulate costs roughly `rays × bounces × (sources + 1)` shadow rays plus the bounces themselves. Each shadow ray is a voxel walk that skips empty 4×4×4 bricks. To make it cheaper:

- Use coarser grids. 20 cm cells are plenty for sound.
- Use fewer rays (`bau_set_rays`). Results blend over calls, so 512 rays at 30 Hz settle about as well as 2048 at 8 Hz.
- Keep `max_distance` tight, so far sources are skipped entirely.

`bau_get_stats` reports the last simulate's time and shadow-ray count.
