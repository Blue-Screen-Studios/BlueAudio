#ifndef BLUEAUDIO_H
#define BLUEAUDIO_H

/* BlueAudio: the sound of a world, traced, in C. It plays nothing and holds
 * no sound of its own: the game declares its sources (each a callback that
 * fills a mono waveform), hands over its world as voxel grids, and says
 * where the listener is. BlueAudio fires rays out from the listener, bounces
 * them off the voxels, and works out, for each source:
 *
 *   - its muffle: how much of it reaches the listener (through walls, round
 *     corners: the rays that found it), its highs cut the more it's muffled;
 *   - its direction: where it's heard from. A sound round a corner is heard
 *     from the corner the rays went out to, not through the wall;
 *   - its distance and delay (the paths' lengths);
 *
 * and, for the listener, the echo: the rays that bounce back to the listener
 * (how enclosed the place is, how long its sound rings, any distant wall
 * answering back).
 *
 * Out comes the final mix: one waveform per channel (stereo, headphones,
 * quad, 5.1, 7.1), interleaved, for the game to play however it plays.
 *
 *   bau_context* au = bau_create(NULL);
 *   bau_set_material(au, 1, bau_material_preset(BAU_MATERIAL_CONCRETE));
 *   bau_grid* level = bau_grid_create(au, nx, ny, nz, 0.2f, cells);
 *   bau_source* car = bau_source_create(au, fill_engine, &engine);
 *   ...each frame:
 *   bau_set_listener(au, camera_pos, camera_ahead, camera_up);
 *   bau_source_set_position(car, car_pos);
 *   ...a worker thread, ~30 times a second:
 *   bau_simulate(au);
 *   ...the audio device's callback:
 *   bau_render(au, out, frames);   (out: frames * bau_channels(au) floats)
 *
 * Threads: bau_simulate on any one thread, bau_render on any one (the
 * device's), the rest on the game's. A source's callback is called from
 * bau_render only, and never again once bau_source_destroy returns. */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32)
#  if defined(BLUEAUDIO_BUILDING)
#    define BAU_API __declspec(dllexport)
#  elif defined(BLUEAUDIO_DLL)
#    define BAU_API __declspec(dllimport)
#  else
#    define BAU_API
#  endif
#elif defined(__GNUC__) && __GNUC__ >= 4
#  define BAU_API __attribute__((visibility("default")))
#else
#  define BAU_API
#endif

/* (C++: the fields a brace list leaves out zeroed, without a warning; C
 * zeroes them anyway.) */
#ifdef __cplusplus
#define BAU_ZERO = {}
#else
#define BAU_ZERO
#endif

typedef struct bau_context bau_context;
typedef struct bau_grid bau_grid;
typedef struct bau_source bau_source;

/* ------------------------------------------------------------ the context */

/* The channels out, in SDL's / WAV's order:
 *   MONO        C
 *   STEREO      L R               (speakers: panned)
 *   HEADPHONES  L R               (each ear its own delay, its own shadow)
 *   QUAD        FL FR BL BR
 *   SURROUND_51 FL FR C LFE BL BR (LFE left silent)
 *   SURROUND_71 FL FR C LFE BL BR SL SR */
typedef enum bau_layout {
  BAU_LAYOUT_STEREO = 0,
  BAU_LAYOUT_MONO,
  BAU_LAYOUT_HEADPHONES,
  BAU_LAYOUT_QUAD,
  BAU_LAYOUT_SURROUND_51,
  BAU_LAYOUT_SURROUND_71
} bau_layout;

/* Any field 0: its default. */
typedef struct bau_config {
  int sample_rate BAU_ZERO;    /* 48000 */
  bau_layout layout BAU_ZERO;  /* stereo */
  int rays BAU_ZERO;           /* fired from the listener each simulate: 1024 */
  int bounces BAU_ZERO;        /* each ray's, at most: 4 */
  float max_distance BAU_ZERO; /* m: sources further off silent, rays no further: 90 */
  float listener_radius BAU_ZERO; /* m: solids this close to the listener ignored (a camera in a wall): 0.25 */
  uint32_t seed BAU_ZERO;      /* the rays' (the same seed, the same results): 1 */
} bau_config;

BAU_API bau_context* bau_create(const bau_config* config); /* (NULL: the defaults) */
BAU_API void bau_destroy(bau_context* au);
BAU_API void bau_get_config(const bau_context* au, bau_config* out); /* (the defaults filled in) */
BAU_API int bau_channels(const bau_context* au);
BAU_API void bau_set_layout(bau_context* au, bau_layout layout);
BAU_API void bau_set_rays(bau_context* au, int rays, int bounces);

/* ------------------------------------------------------------ the world */

/* What a voxel's made of, in three bands (low ~250 Hz, mid ~1 kHz, high
 * ~4 kHz): absorption (0..1, of what hits it, soaked up), transmission
 * (0..1, let through one cell of it), scattering (0: a mirror, 1: every
 * which way). */
typedef struct bau_material {
  float absorption[3];
  float scattering;
  float transmission[3];
} bau_material;

typedef enum bau_material_kind {
  BAU_MATERIAL_GENERIC = 0,
  BAU_MATERIAL_CONCRETE,
  BAU_MATERIAL_BRICK,
  BAU_MATERIAL_METAL,
  BAU_MATERIAL_GLASS,
  BAU_MATERIAL_WOOD,
  BAU_MATERIAL_GROUND, /* earth, mud, grass, gravel */
  BAU_MATERIAL_FOLIAGE,
  BAU_MATERIAL_CARPET, /* soft: fabric, plastic */
  BAU_MATERIAL_MESH,   /* chain link, grilles: mostly holes */
  BAU_MATERIAL_WATER,
  BAU_MATERIAL_KINDS
} bau_material_kind;

BAU_API bau_material bau_material_preset(bau_material_kind kind);
/* Material `index` (1..255; 0 is air). Unset ones: generic. */
BAU_API void bau_set_material(bau_context* au, uint8_t index, bau_material material);

/* A voxel grid: nx * ny * nz cells `cell` m on a side, each a material index
 * (0: air), x fastest: cells[x + nx * (y + ny * z)]. y is up. `cells` may be
 * NULL (all air, filled in after with bau_grid_update). Stored sparsely (4^3
 * blocks, the empty ones nothing), copied: the game's array is its own. */
BAU_API bau_grid* bau_grid_create(bau_context* au, int nx, int ny, int nz, float cell, const uint8_t* cells);
/* Cells changed (shattered, built): the box at x0, y0, z0, sx * sy * sz of
 * them, `cells` that box's (x fastest). */
BAU_API void bau_grid_update(bau_grid* grid, int x0, int y0, int z0, int sx, int sy, int sz, const uint8_t* cells);
/* Where it is: cell 0's low corner at `origin` (world), turned `yaw`
 * radians about y (about that corner). Cheap: a vehicle's, every frame. */
BAU_API void bau_grid_set_pose(bau_grid* grid, const float origin[3], float yaw);
BAU_API void bau_grid_destroy(bau_grid* grid);

/* One ray through the world as it stands (a game's own line of sight, a
 * demo's rays drawn): the nearest solid along origin + dir * t, 0 < t <=
 * max_distance; 1 and `hit`, else 0. */
typedef struct bau_hit {
  float distance BAU_ZERO;
  float point[3] BAU_ZERO;
  float normal[3] BAU_ZERO; /* (facing back along the ray) */
  uint8_t material BAU_ZERO;
} bau_hit;
BAU_API int bau_trace_ray(bau_context* au, const float origin[3], const float dir[3], float max_distance, bau_hit* hit);

/* ------------------------------------------------------------ the listener */

/* Where it is, which way it faces (ahead, up: unit, at right angles). */
BAU_API void bau_set_listener(bau_context* au, const float pos[3], const float ahead[3], const float up[3]);

/* ------------------------------------------------------------ the sources */

/* Fill `mono` with the source's next `frames` samples (at the sample rate);
 * return how many it filled: fewer than `frames`, and it's finished (the
 * rest silence, never called again). Called from bau_render. */
typedef int (*bau_fill_fn)(void* user, float* mono, int frames);

BAU_API bau_source* bau_source_create(bau_context* au, bau_fill_fn fill, void* user);
BAU_API void bau_source_set_position(bau_source* src, const float pos[3]);
/* Its loudness up close (1: as filled). */
BAU_API void bau_source_set_gain(bau_source* src, float gain);
/* How it falls off: full within `ref` m, inverse square past it, faded out
 * to nothing at `max` (defaults 7, the context's max_distance). */
BAU_API void bau_source_set_rolloff(bau_source* src, float ref, float max);
/* Its size: solids within `radius` m of it ignored (an engine inside its
 * own truck's voxels). Default 0.5. */
BAU_API void bau_source_set_radius(bau_source* src, float radius);
BAU_API void* bau_source_user(const bau_source* src);
/* Its callback's ended and what's left of it played out (its delay): the
 * game can destroy it. */
BAU_API int bau_source_finished(const bau_source* src);
BAU_API void bau_source_destroy(bau_source* src);

/* ------------------------------------------------------------ simulate, render */

/* Trace: rays from the listener, the sources' muffle, direction, distance,
 * the echo. Blends with what was traced before (rays a little different
 * each time: smooth, accumulating), so call it often (~30 Hz). */
BAU_API void bau_simulate(bau_context* au);
/* The final mix: `frames` frames into `out` (frames * bau_channels floats,
 * interleaved), overwritten. Each source's callback called for its block,
 * delayed, muffled, panned from where it's heard; the echo added. */
BAU_API void bau_render(bau_context* au, float* out, int frames);

/* ------------------------------------------------------------ what it worked out */

typedef struct bau_heard {
  int simulated BAU_ZERO;   /* (traced at least once) */
  float distance BAU_ZERO;  /* m, straight */
  int occluded BAU_ZERO;    /* nothing straight through */
  float muffle BAU_ZERO;    /* 0: clear .. 1: all but gone (the highs cut by it) */
  float reach BAU_ZERO;     /* the fraction of rays that found it */
  float direct BAU_ZERO;    /* gain, straight (through anything in the way) */
  float reflected BAU_ZERO; /* gain, off the walls */
  float gain BAU_ZERO;      /* gain, both, its rolloff in it */
  float direction[3] BAU_ZERO; /* world: from the listener, the way it's heard from */
  float spread BAU_ZERO;    /* 0: all from that way .. 1: all about */
  float delay BAU_ZERO;     /* s */
} bau_heard;
BAU_API void bau_source_heard(const bau_source* src, bau_heard* out);

typedef struct bau_echo {
  float level BAU_ZERO;     /* how much comes back (0..1): the reverb's */
  float decay BAU_ZERO;     /* s: RT60, the ring dying away by 60 dB */
  float enclosure BAU_ZERO; /* 0: open sky .. 1: shut in */
  float free_path BAU_ZERO; /* m: the mean between bounces */
  float direction[3] BAU_ZERO; /* world: where most of it comes back from */
  float focus BAU_ZERO;     /* 0: all about .. 1: all from there */
  int taps BAU_ZERO;        /* distinct echoes (a far wall answering) */
  float tap_delay[4] BAU_ZERO, tap_gain[4] BAU_ZERO, tap_direction[4][3] BAU_ZERO;
} bau_echo;
BAU_API void bau_listener_echo(const bau_context* au, bau_echo* out);

/* Some of the last simulate's paths that found a source: the listener, the
 * bounces, the source. */
#define BAU_RAY_POINTS 8
typedef struct bau_ray {
  const bau_source* source BAU_ZERO; /* (its identity only) */
  int points BAU_ZERO;
  float at[BAU_RAY_POINTS][3] BAU_ZERO;
  float gain BAU_ZERO;
} bau_ray;
BAU_API int bau_debug_rays(const bau_context* au, bau_ray* out, int max);

/* Paths to draw: a fan of rays fired from the listener now, through the
 * world as it stands, each bouncing a mirror's way (no scattering: steady
 * from one call to the next), carrying its energy in three bands (soaked up
 * at each bounce by its material, thinned by the air). Where a bounce sees
 * a source, a FOUND path branches off to it; where it sees the listener, an
 * ECHO path back. A debug view, slowed down to be read: not the
 * simulation's own rays (those go every way at random, many more of them),
 * but the same world, the same materials, the same sums.
 *
 * Out: OUT paths, each followed by its branches (their `parent` its index,
 * `start` how far along it they branch: draw a branch's head at the
 * parent's head less `start`). Returns how many were written (at most
 * `max`; branches past it dropped). */
#define BAU_PATH_POINTS 8
typedef enum bau_path_kind { BAU_PATH_OUT = 0, BAU_PATH_FOUND, BAU_PATH_ECHO } bau_path_kind;

typedef struct bau_path_config {
  int rays BAU_ZERO;    /* 0: 40 */
  int bounces BAU_ZERO; /* 0: 5 (at most BAU_PATH_POINTS - 1) */
  float reach BAU_ZERO; /* m a ray goes between bounces at most, 0: 40 */
  float turn BAU_ZERO;  /* radians: the fan turned about up (a different fan each call) */
  int flat BAU_ZERO;    /* 1: the fan flat, at the listener's height (bounces kept level,
                         * sources seen at that height): a view from above. 0: every way */
  float min_energy BAU_ZERO; /* a ray ends once its bands sum under this, 0: 0.03 */
} bau_path_config;

typedef struct bau_path {
  bau_path_kind kind BAU_ZERO;
  int parent BAU_ZERO;               /* (an OUT path: -1) */
  float start BAU_ZERO;              /* m along the parent this branches at */
  const bau_source* source BAU_ZERO; /* (FOUND: the one; its identity only) */
  int points BAU_ZERO;
  int open_end BAU_ZERO;             /* its last point open air (it ran out), not a bounce */
  float at[BAU_PATH_POINTS][3] BAU_ZERO;     /* world */
  float distance[BAU_PATH_POINTS] BAU_ZERO;  /* m along it to each point (0 at the first) */
  float energy[BAU_PATH_POINTS][3] BAU_ZERO; /* low, mid, high: what it carries on from each point (1: as it left) */
} bau_path;

BAU_API int bau_debug_paths(bau_context* au, const bau_path_config* config, bau_path* out, int max);
/* A point `distance` m along a path (clamped to it): where, and the energy
 * there (lerped between its points). */
BAU_API void bau_path_sample(const bau_path* path, float distance, float pos[3], float energy[3]);
/* A colour for drawing what a path carries: its brightness (alpha) the
 * energy left, its hue the bands (cool white: all of them; orange: the
 * highs gone); a FOUND path gold, an ECHO violet. rgba 0..1. */
BAU_API void bau_path_color(bau_path_kind kind, const float energy[3], float rgba[4]);

typedef struct bau_stats {
  int grids BAU_ZERO, sources BAU_ZERO;
  size_t bricks BAU_ZERO;    /* 4^3 blocks with anything in them */
  int rays BAU_ZERO, bounces BAU_ZERO;
  long shadow_rays BAU_ZERO; /* the last simulate's */
  double simulate_ms BAU_ZERO;
} bau_stats;
BAU_API void bau_get_stats(const bau_context* au, bau_stats* out);

#ifdef __cplusplus
}
#endif

#endif
