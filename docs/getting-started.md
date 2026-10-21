# Getting started

This guide sets up BlueAudio in a game end to end: the materials, the world, sources, the listener, the simulate thread and the audio callback.

## 1. Create a context

```c
bau_config config = {0};
config.sample_rate = 48000;            /* your device's rate */
config.layout = BAU_LAYOUT_HEADPHONES; /* or STEREO, MONO, QUAD, SURROUND_51, SURROUND_71 */
config.rays = 1024;                    /* fired from the listener each simulate */
config.bounces = 4;
config.max_distance = 90.0f;           /* metres: sources further off are silent */
bau_context* au = bau_create(&config);
```

Any field left at 0 takes its default. `bau_channels(au)` tells you how many channels `bau_render` writes.

## 2. Describe the materials

Each voxel holds a material index from 1 to 255; 0 is air. Give each index a material, either from a preset or your own figures:

```c
bau_set_material(au, 1, bau_material_preset(BAU_MATERIAL_CONCRETE));
bau_set_material(au, 2, bau_material_preset(BAU_MATERIAL_GLASS));

bau_material hay = {{0.4f, 0.6f, 0.8f}, 0.7f, {0.3f, 0.2f, 0.1f}};  /* absorption, scattering, transmission */
bau_set_material(au, 3, hay);
```

Indices you never set behave as `BAU_MATERIAL_GENERIC`.

## 3. Hand over the world

```c
/* cells[x + nx * (y + ny * z)], y up */
bau_grid* level = bau_grid_create(au, nx, ny, nz, 0.2f, cells);
const float corner[3] = {-100.0f, 0.0f, -100.0f};
bau_grid_set_pose(level, corner, 0.0f);  /* cell 0's low corner, and a turn about y */
```

BlueAudio copies the cells, so you can free your array afterwards. For a large world, create the grid empty (`cells = NULL`) and fill it in chunks with `bau_grid_update`. A moving object such as a vehicle gets its own grid, and you call `bau_grid_set_pose` every frame.

## 4. Declare sources

A source is a callback that BlueAudio calls from `bau_render` to fill the next block of mono samples:

```c
typedef struct engine { float phase, hz; } engine;

static int fill_engine(void* user, float* mono, int frames) {
  engine* e = (engine*)user;
  for (int i = 0; i < frames; ++i) {
    mono[i] = sinf(e->phase);
    e->phase += 6.2831853f * e->hz / 48000.0f;
  }
  return frames;
}

engine truck = {0, 55.0f};
bau_source* src = bau_source_create(au, fill_engine, &truck);
bau_source_set_gain(src, 0.8f);          /* its loudness up close */
bau_source_set_rolloff(src, 7.0f, 90.0f); /* full within 7 m, inverse square past it, gone by 90 m */
bau_source_set_radius(src, 1.0f);        /* ignore the truck's own voxels around it */
```

A one-shot sound returns fewer frames than asked when it ends. Once `bau_source_finished` reports that its delayed tail has played out, destroy it.

## 5. Each frame

```c
bau_set_listener(au, camera_pos, camera_ahead, camera_up);
bau_source_set_position(src, truck_pos);
bau_grid_set_pose(truck_grid, truck_corner, truck_yaw);
```

## 6. Simulate on a worker thread

```c
while (running) {
  bau_simulate(au);  /* a few ms */
  sleep_ms(33);      /* ~30 Hz */
}
```

Each simulate blends with the ones before, so the sound settles over a few calls. Calling it more often makes it settle faster.

## 7. Render in the audio callback

```c
void audio_callback(float* out, int frames) {
  bau_render(au, out, frames);  /* frames * bau_channels(au), interleaved, overwritten */
  for (int i = 0; i < frames * bau_channels(au); ++i) out[i] = tanhf(out[i] * volume);
}
```

The output is a plain sum. Apply your own volume and limiter, and mix in any sounds that aren't in the world, such as the UI or a narrator, after it.

## 8. Shut down

```c
bau_destroy(au);  /* its grids and sources too */
```
