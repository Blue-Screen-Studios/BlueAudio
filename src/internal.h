#ifndef BLUEAUDIO_INTERNAL_H
#define BLUEAUDIO_INTERNAL_H

/* BlueAudio's insides: the context, grids, sources; the vector helpers. */

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "blueaudio/blueaudio.h"

/* ----------------------------------------------------------- locks */

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
typedef SRWLOCK bau__mutex;
#  define bau__mutex_init(m) InitializeSRWLock(m)
#  define bau__mutex_free(m) ((void)(m))
#  define bau__lock(m) AcquireSRWLockExclusive(m)
#  define bau__unlock(m) ReleaseSRWLockExclusive(m)
#else
#  include <pthread.h>
typedef pthread_mutex_t bau__mutex;
#  define bau__mutex_init(m) pthread_mutex_init((m), NULL)
#  define bau__mutex_free(m) pthread_mutex_destroy(m)
#  define bau__lock(m) pthread_mutex_lock(m)
#  define bau__unlock(m) pthread_mutex_unlock(m)
#endif

/* ----------------------------------------------------------- constants */

#define BAU__BANDS 3
#define BAU__BRICK 4                 /* cells a brick's side */
#define BAU__SPEED 343.0f            /* m/s */
#define BAU__BIN 0.005f              /* s: the echo's histogram */
#define BAU__BINS 400                /* (2 s of it) */
#define BAU__CHUNK 256               /* frames rendered at a time */
#define BAU__MAX_CHANNELS 8
#define BAU__DEBUG_RAYS 96
#define BAU__BLEND 0.35f             /* each simulate's share of what's published */
#define BAU__PI 3.14159265358979f

/* ----------------------------------------------------------- vectors */

typedef struct bau__v3 { float x, y, z; } bau__v3;

static inline bau__v3 bau__v(float x, float y, float z) { bau__v3 r = {x, y, z}; return r; }
static inline bau__v3 bau__vp(const float* p) { return bau__v(p[0], p[1], p[2]); }
static inline void bau__store(bau__v3 v, float* p) { p[0] = v.x, p[1] = v.y, p[2] = v.z; }
static inline bau__v3 bau__add(bau__v3 a, bau__v3 b) { return bau__v(a.x + b.x, a.y + b.y, a.z + b.z); }
static inline bau__v3 bau__sub(bau__v3 a, bau__v3 b) { return bau__v(a.x - b.x, a.y - b.y, a.z - b.z); }
static inline bau__v3 bau__mul(bau__v3 a, float s) { return bau__v(a.x * s, a.y * s, a.z * s); }
static inline float bau__dot(bau__v3 a, bau__v3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static inline bau__v3 bau__cross(bau__v3 a, bau__v3 b) {
  return bau__v(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}
static inline float bau__len(bau__v3 a) { return sqrtf(bau__dot(a, a)); }
static inline bau__v3 bau__norm(bau__v3 a) {
  const float l = bau__len(a);
  return l > 1e-12f ? bau__mul(a, 1.0f / l) : bau__v(0.0f, 0.0f, 0.0f);
}
static inline float bau__clamp(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
static inline float bau__lerp(float a, float b, float t) { return a + (b - a) * t; }

/* A random number (xorshift32): 0..1. */
static inline float bau__rand(uint32_t* s) {
  uint32_t x = *s;
  x ^= x << 13, x ^= x >> 17, x ^= x << 5;
  *s = x ? x : 0x9e3779b9u;
  return (float)(x >> 8) * (1.0f / 16777216.0f);
}

/* Air: intensity lost a metre, each band (trace.c). */
extern const float bau__air[BAU__BANDS];

/* ----------------------------------------------------------- the world */

struct bau_grid {
  bau_context* au;
  int nx, ny, nz;
  int bx, by, bz;      /* bricks */
  float cell;
  uint32_t* bricks;    /* each: 0 empty, else 1 + its block in `pool` (64 cells) */
  uint8_t* pool;
  size_t blocks, cap;  /* (blocks in use, room for) */
  /* Its pose: the simulation's (world lock), the game's last (state lock). */
  float origin[3], yaw, cy, sy;
  float next_origin[3], next_yaw;
  int moved;
  bau_grid* next;
};

typedef struct bau__hit {
  float t;          /* m along the ray */
  bau__v3 normal;   /* world, facing the ray */
  uint8_t material;
} bau__hit;

/* world.c: the nearest solid along o + d t (d unit), tmin < t < tmax: 1 and
 * `hit`, else 0. (World lock held.) */
int bau__trace(const bau_context* au, bau__v3 o, bau__v3 d, float tmin, float tmax, bau__hit* hit);
/* Nothing solid between: 1. */
int bau__clear(const bau_context* au, bau__v3 o, bau__v3 d, float tmin, float tmax);
/* What gets through, each band (every solid cell crossed its material's
 * transmission); its solid cells counted (0: clear). */
int bau__transmit(const bau_context* au, bau__v3 o, bau__v3 d, float tmin, float tmax, float t[BAU__BANDS]);
void bau__grid_apply_pose(bau_grid* grid);

/* ----------------------------------------------------------- sources */

struct bau_source {
  bau_context* au;
  bau_fill_fn fill;
  void* user;
  /* The game's (state lock). */
  float pos[3], gain, ref, max, radius;
  int dead;
  /* The simulation's, published (state lock). */
  int simulated, occluded, reached;
  float distance, reach;
  float direct[BAU__BANDS];   /* intensity relative to free field, straight */
  float reflected[BAU__BANDS];/* off the walls */
  float arrive[3];            /* the reflections' directions, summed (weighted) */
  float path;                 /* the reflections' mean length (m) */
  /* The render thread's. */
  float* ring;
  int ring_mask, ring_at;
  int started, ended, tail;   /* (tail: frames still to play out once ended) */
  float delay, amp, cut;
  /* (Its targets, this render call's.) */
  float to_delay, to_amp, to_cut, to_gains[BAU__MAX_CHANNELS], to_itd[2], to_shadow[2];
  float lp[2];                /* the muffle: two one-poles */
  float gains[BAU__MAX_CHANNELS];
  float itd[2], shadow[2], ear[2];
  bau_source* next;
};

/* ----------------------------------------------------------- the reverb */

typedef struct bau__reverb bau__reverb;
bau__reverb* bau__reverb_create(int rate);
void bau__reverb_destroy(bau__reverb* r);
/* Its settings, from the echo (render thread). */
void bau__reverb_set(bau__reverb* r, const bau_echo* echo, const float listener[9]);
/* `send` (mono, frames) in; its ring out, added into `out` (channels). */
void bau__reverb_process(bau__reverb* r, const float* send, float* out, int frames, int channels, bau_layout layout);

/* ----------------------------------------------------------- the context */

struct bau_context {
  bau_config cfg;
  int channels;
  /* Locks, taken in this order: world, render, state. */
  bau__mutex world, render, state;
  bau_material materials[256];
  bau_grid* grids;
  bau_source* sources;
  float listener[9];          /* pos, ahead, up (state lock) */
  uint32_t rng;
  /* Published (state lock). */
  bau_echo echo;
  float hist[BAU__BINS];
  float hist_dir[BAU__BINS][3];
  int echo_set;
  float stat_absorb, stat_escape; /* (the rays': blended) */
  bau_ray rays[BAU__DEBUG_RAYS];
  int nrays;
  bau_stats stats;
  /* The render thread's. */
  bau__reverb* reverb;
  float mono[BAU__CHUNK], send[BAU__CHUNK], wet[BAU__CHUNK * BAU__MAX_CHANNELS];
};

/* context.c: what a source is heard as (state lock held). */
void bau__heard(const bau_source* src, bau_heard* out);

/* render.c: a source's render state for the context's rate; freed. */
int bau__source_init_render(bau_source* src);
void bau__source_free_render(bau_source* src);
/* A speaker layout's directions: azimuths (radians, 0 ahead, + right), -1000 for LFE. */
int bau__speakers(bau_layout layout, float* azimuths);
/* Gains for a direction (listener space: x right, y up, z ahead) and spread,
 * power kept. */
void bau__pan(bau_layout layout, bau__v3 dir, float spread, float* gains);

#endif
