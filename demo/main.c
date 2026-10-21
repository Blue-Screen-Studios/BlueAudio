/* BlueAudio demo.
 *
 * A level seen from above at a slant (orthographic), a white capsule you
 * walk about with WASD, music playing somewhere in it and a chime somewhere
 * else. Pick a room (1-6, or the panel): an open field, a concrete hall,
 * corridors, the room next door, a canyon, a glass box. What you hear is
 * BlueAudio's mix: muffled through walls, round corners from the corner,
 * echoing in the hall, answered by the canyon's walls.
 *
 * The rays drawn are BlueAudio's debug paths (bau_debug_paths), slowed
 * right down and kept flat (XZ, at ear height) to be read at a glance; its
 * simulation's own rays go every way, at the speed of sound. Each carries its sound's energy in three bands: it
 * fades as surfaces soak it up, going from cool white (every band) to
 * orange (the highs gone: carpet, earth). Gold where one finds a source,
 * violet where one comes back to you (the echo). The arrows are BlueAudio's
 * answer: where each source is heard from.
 *
 * SDL3 belongs to the demo alone, as does BlueUI (the panel, its narrator):
 * https://github.com/Blue-Screen-Studios/BlueUI. Music by Joystock -
 * https://www.joystock.org */

#include <SDL3/SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "blueaudio/blueaudio.h"
#include "blueui/widgets.h"

#define DR_MP3_IMPLEMENTATION
#define DR_MP3_NO_STDIO
#include "dr_mp3.h"

#define RATE 48000

extern const unsigned char demo_music[];
extern const unsigned long demo_music_size;

/* ---------------------------------------------------------------- the level */

/* 48 x 8 x 32 m in 0.5 m cells; y 0 the floor. */
#define NX 96
#define NY 16
#define NZ 64
#define CELL 0.5f
#define EAR 1.7f

enum { AIR, CONCRETE, CARPET, GLASS, WOOD, GROUND, MATERIALS };
static const float kColors[MATERIALS][3] = {
  {0, 0, 0}, {0.55f, 0.56f, 0.58f}, {0.55f, 0.22f, 0.26f}, {0.55f, 0.8f, 0.9f}, {0.6f, 0.42f, 0.25f}, {0.28f, 0.36f, 0.2f},
};

static uint8_t cells[NX * NY * NZ];
static uint8_t wall_h[NX * NZ];  /* each column's wall: cells above the floor */
static uint8_t roofed[NX * NZ];

static uint8_t* cell(int x, int y, int z) { return &cells[x + NX * (y + NY * z)]; }

/* Walls (h m tall, standing on the floor) over x0..x1, z0..z1 (m). */
static void wall(float x0, float z0, float x1, float z1, float h, uint8_t m) {
  for (int z = (int)(z0 / CELL); z < (int)(z1 / CELL) && z < NZ; ++z)
    for (int x = (int)(x0 / CELL); x < (int)(x1 / CELL) && x < NX; ++x) {
      if (x < 0 || z < 0) continue;
      const int top = (int)(h / CELL);
      for (int y = 1; y <= top && y < NY; ++y) *cell(x, y, z) = m;
      if (top > wall_h[x + NX * z]) wall_h[x + NX * z] = (uint8_t)(top < NY - 1 ? top : NY - 1);
    }
}

/* Air carved back out (a doorway, a corridor), up to h m. */
static void carve(float x0, float z0, float x1, float z1, float h) {
  for (int z = (int)(z0 / CELL); z < (int)(z1 / CELL) && z < NZ; ++z)
    for (int x = (int)(x0 / CELL); x < (int)(x1 / CELL) && x < NX; ++x) {
      if (x < 0 || z < 0) continue;
      for (int y = 1; y <= (int)(h / CELL) && y < NY; ++y) *cell(x, y, z) = AIR;
      wall_h[x + NX * z] = 0;
    }
}

static void floor_of(float x0, float z0, float x1, float z1, uint8_t m) {
  for (int z = (int)(z0 / CELL); z < (int)(z1 / CELL) && z < NZ; ++z)
    for (int x = (int)(x0 / CELL); x < (int)(x1 / CELL) && x < NX; ++x) *cell(x, 0, z) = m;
}

/* A roof at h m (not drawn: the rooms seen into). */
static void roof(float x0, float z0, float x1, float z1, float h, uint8_t m) {
  const int y = (int)(h / CELL);
  for (int z = (int)(z0 / CELL); z < (int)(z1 / CELL) && z < NZ; ++z)
    for (int x = (int)(x0 / CELL); x < (int)(x1 / CELL) && x < NX; ++x) *cell(x, y, z) = m, roofed[x + NX * z] = 1;
}

enum { ROOM_FIELD, ROOM_HALL, ROOM_CORRIDORS, ROOM_NEXT_DOOR, ROOM_CANYON, ROOM_GLASS, ROOMS };
static const char* const kRooms[ROOMS] = {"Open field", "Concrete hall", "Corridors", "Next door", "Canyon", "Glass box"};
static const char* const kAbout[ROOMS] = {
  "Nothing but grass. Every sound straight from where it is, no echo",
  "A shut concrete hall with pillars. It rings for seconds",
  "Concrete corridors. The music is round the corner: heard from the corner",
  "Two rooms and a doorway. Through the wall it's muffled; at the door it opens up",
  "Grass between tall rock walls, open to the sky. They answer back",
  "The music shut in a glass box. Glass lets some through, its highs least",
};

typedef struct room {
  float player[2], music[2], chime[2];
} room;

static room build(int which) {
  memset(cells, 0, sizeof cells), memset(wall_h, 0, sizeof wall_h), memset(roofed, 0, sizeof roofed);
  room r = {{24, 24}, {24, 8}, {38, 18}};
  floor_of(0, 0, 48, 32, GROUND);
  switch (which) {
    case ROOM_HALL:
      floor_of(6, 4, 42, 28, CONCRETE);
      wall(6, 4, 42, 5, 6, CONCRETE), wall(6, 27, 42, 28, 6, CONCRETE);
      wall(6, 4, 7, 28, 6, CONCRETE), wall(41, 4, 42, 28, 6, CONCRETE);
      for (float x = 14; x < 40; x += 8)
        for (float z = 11; z < 24; z += 8) wall(x, z, x + 1, z + 1, 6, CONCRETE);
      roof(6, 4, 42, 28, 6, CONCRETE);
      r = (room){{24, 22}, {12, 9}, {36, 23}};
      break;
    case ROOM_CORRIDORS:
      floor_of(0, 0, 48, 32, CONCRETE);
      wall(0, 0, 48, 32, 3, CONCRETE);
      carve(10, 6, 13, 28, 2.5f);   /* up from the start */
      carve(10, 6, 40, 9, 2.5f);    /* right at the top */
      carve(37, 6, 40, 24, 2.5f);   /* down the far side */
      carve(20, 15, 37, 18, 2.5f);  /* a branch back in */
      roof(0, 0, 48, 32, 3, CONCRETE);
      r = (room){{11.5f, 26}, {38.5f, 22}, {21, 16.5f}};
      break;
    case ROOM_NEXT_DOOR:
      floor_of(4, 4, 23, 28, CARPET), floor_of(25, 4, 44, 28, CONCRETE);
      wall(4, 4, 44, 5, 3, CONCRETE), wall(4, 27, 44, 28, 3, CONCRETE);
      wall(4, 4, 5, 28, 3, CONCRETE), wall(43, 4, 44, 28, 3, CONCRETE);
      wall(23.5f, 4, 24.5f, 28, 3, WOOD);
      carve(23.5f, 22, 24.5f, 24, 2.2f); /* the doorway */
      roof(4, 4, 44, 28, 3, CONCRETE);
      r = (room){{12, 12}, {38, 10}, {34, 24}};
      break;
    case ROOM_CANYON:
      wall(0, 0, 48, 2, 7, CONCRETE);
      wall(46, 0, 48, 32, 7, CONCRETE);
      wall(0, 0, 2, 20, 7, CONCRETE);
      r = (room){{14, 26}, {18, 22}, {40, 8}};
      break;
    case ROOM_GLASS:
      wall(18, 6, 30, 6.5f, 3, GLASS), wall(18, 15.5f, 30, 16, 3, GLASS);
      wall(18, 6, 18.5f, 16, 3, GLASS), wall(29.5f, 6, 30, 16, 3, GLASS);
      floor_of(18, 6, 30, 16, WOOD);
      roof(18, 6, 30, 16, 3, GLASS);
      r = (room){{24, 26}, {24, 11}, {8, 24}};
      break;
    default: break;
  }
  return r;
}

static bool solid_at(float x, float z) {
  const int cx = (int)floorf(x / CELL), cz = (int)floorf(z / CELL);
  if (cx < 0 || cz < 0 || cx >= NX || cz >= NZ) return true;
  return *cell(cx, 2, cz) != AIR;
}

static bool blocked(float x, float z, float r) {
  return solid_at(x - r, z - r) || solid_at(x + r, z - r) || solid_at(x - r, z + r) || solid_at(x + r, z + r);
}

/* ---------------------------------------------------------------- sounds */

typedef struct music {
  float* pcm;
  size_t frames, at;
  bool on;
} music;

/* The music: decoded, mixed down to mono (a speaker in the world), at RATE. */
static music load_music(void) {
  music m = {0};
  drmp3_config cfg;
  drmp3_uint64 frames = 0;
  float* pcm = drmp3_open_memory_and_read_pcm_frames_f32(demo_music, (size_t)demo_music_size, &cfg, &frames, NULL);
  if (!pcm || !frames || !cfg.channels || !cfg.sampleRate) return m;
  const double step = (double)cfg.sampleRate / RATE;
  m.frames = (size_t)((double)frames / step);
  m.pcm = (float*)malloc(m.frames * sizeof(float));
  for (size_t i = 0; i < m.frames; ++i) {
    const double at = (double)i * step;
    const size_t a = (size_t)at, b = a + 1 < frames ? a + 1 : a;
    const float f = (float)(at - (double)a);
    float va = 0.0f, vb = 0.0f;
    for (unsigned c = 0; c < cfg.channels; ++c) va += pcm[a * cfg.channels + c], vb += pcm[b * cfg.channels + c];
    m.pcm[i] = (va + (vb - va) * f) / (float)cfg.channels;
  }
  drmp3_free(pcm, NULL);
  m.on = true;
  return m;
}

static int fill_music(void* user, float* mono, int frames) {
  music* m = (music*)user;
  for (int i = 0; i < frames; ++i) {
    mono[i] = m->on && m->frames ? m->pcm[m->at] : 0.0f;
    if (m->frames && ++m->at >= m->frames) m->at = 0;
  }
  return frames;
}

/* A chime every two seconds: struck, ringing out (short: its echoes heard). */
typedef struct chime {
  long at;
  bool on;
} chime;

static int fill_chime(void* user, float* mono, int frames) {
  chime* c = (chime*)user;
  for (int i = 0; i < frames; ++i, ++c->at) {
    const float t = (float)(c->at % (2 * RATE)) / RATE, ring = expf(-t * 9.0f), w = 2.0f * 3.14159265f * t;
    mono[i] = c->on ? ring * (0.6f * sinf(880.0f * w) + 0.3f * sinf(1320.0f * w) + 0.15f * sinf(2640.0f * w)) : 0.0f;
  }
  return frames;
}

/* ---------------------------------------------------------------- the rays drawn */

/* BlueAudio's debug paths (bau_debug_paths: flat, at ear height), each
 * with how far along it its head is: a fan of them out every so often from
 * where you stand, their branches (to a source, back to you) setting off as
 * the head passes the bounce they branch at. */
#define MAX_RAYS 6000
#define TRAIL 3.0f

typedef struct vray {
  bau_path path;
  float head;
} vray;

static vray rays[MAX_RAYS];
static int nrays;
static bau_path fan[MAX_RAYS];

static void launch(bau_context* au, float turn) {
  bau_path_config c = {0};
  c.rays = 40, c.bounces = 5, c.reach = 40.0f, c.turn = turn, c.flat = 1;
  const int n = bau_debug_paths(au, &c, fan, MAX_RAYS - nrays);
  for (int i = 0; i < n; ++i) rays[nrays + i].path = fan[i], rays[nrays + i].head = -fan[i].start;
  nrays += n;
}

/* On by `dt` at `speed` m/s; the spent ones dropped. */
static void advance(float dt, float speed) {
  int kept = 0;
  for (int i = 0; i < nrays; ++i) {
    vray* r = &rays[i];
    r->head += dt * speed;
    if (r->head - TRAIL < r->path.distance[r->path.points - 1]) rays[kept++] = *r;
  }
  nrays = kept;
}

/* ---------------------------------------------------------------- drawing */

typedef struct view {
  float s, ox, oy; /* px a metre, where (0, 0, 0) lands */
} view;

/* Oblique orthographic: x across, z down the screen, y up it. */
static SDL_FPoint project(const view* v, float x, float y, float z) {
  return (SDL_FPoint){v->ox + x * v->s, v->oy + (z - y * 0.55f) * v->s};
}

static SDL_Vertex* verts;
static int nverts, cap_verts;

static void vert(SDL_FPoint p, SDL_FColor c) {
  if (nverts == cap_verts) {
    cap_verts = cap_verts ? cap_verts * 2 : 4096;
    verts = (SDL_Vertex*)realloc(verts, (size_t)cap_verts * sizeof *verts);
  }
  verts[nverts++] = (SDL_Vertex){p, c, {0, 0}};
}

static void tri(SDL_FPoint a, SDL_FPoint b, SDL_FPoint c, SDL_FColor ca, SDL_FColor cb, SDL_FColor cc) {
  vert(a, ca), vert(b, cb), vert(c, cc);
}

static void quad(SDL_FPoint a, SDL_FPoint b, SDL_FPoint c, SDL_FPoint d, SDL_FColor col) {
  tri(a, b, c, col, col, col), tri(a, c, d, col, col, col);
}

/* A line `w` px wide, its colour going from ca to cb. */
static void line(SDL_FPoint a, SDL_FPoint b, float w, SDL_FColor ca, SDL_FColor cb) {
  const float dx = b.x - a.x, dy = b.y - a.y, l = sqrtf(dx * dx + dy * dy);
  if (l < 1e-3f) return;
  const float nx = -dy / l * w * 0.5f, ny = dx / l * w * 0.5f;
  const SDL_FPoint p0 = {a.x + nx, a.y + ny}, p1 = {b.x + nx, b.y + ny}, p2 = {b.x - nx, b.y - ny}, p3 = {a.x - nx, a.y - ny};
  tri(p0, p1, p2, ca, cb, cb), tri(p0, p2, p3, ca, cb, ca);
}

static void disc(SDL_FPoint c, float rx, float ry, SDL_FColor col) {
  const int n = 24;
  for (int i = 0; i < n; ++i) {
    const float a0 = (float)i / n * 6.2831853f, a1 = (float)(i + 1) / n * 6.2831853f;
    tri(c, (SDL_FPoint){c.x + cosf(a0) * rx, c.y + sinf(a0) * ry}, (SDL_FPoint){c.x + cosf(a1) * rx, c.y + sinf(a1) * ry}, col, col, col);
  }
}

static void ring(SDL_FPoint c, float rx, float ry, float w, SDL_FColor col) {
  const int n = 48;
  for (int i = 0; i < n; ++i) {
    const float a0 = (float)i / n * 6.2831853f, a1 = (float)(i + 1) / n * 6.2831853f;
    line((SDL_FPoint){c.x + cosf(a0) * rx, c.y + sinf(a0) * ry}, (SDL_FPoint){c.x + cosf(a1) * rx, c.y + sinf(a1) * ry}, w, col, col);
  }
}

static SDL_FColor rgba(float r, float g, float b, float a) { return (SDL_FColor){r, g, b, a}; }

static SDL_FColor shade(const float* c, float k) { return rgba(c[0] * k, c[1] * k, c[2] * k, 1.0f); }

/* A ray's colour: BlueAudio's for what it carries (bau_path_color), faded
 * toward its trail's tail. */
static SDL_FColor ray_color(bau_path_kind kind, const float* e, float fade) {
  float c[4];
  bau_path_color(kind, e, c);
  return rgba(c[0], c[1], c[2], c[3] * fade);
}

static void draw_ray(const view* v, const vray* r, float width) {
  const bau_path* p = &r->path;
  const float tail = r->head - TRAIL;
  for (int k = 0; k + 1 < p->points; ++k) {
    const float a0 = p->distance[k], a1 = p->distance[k + 1];
    const float s0 = tail > a0 ? tail : a0, s1 = r->head < a1 ? r->head : a1;
    if (s1 <= s0 || a1 <= a0) continue;
    /* (Where, and what it carries, lerped along the segment; the trail
     * fading toward its tail.) */
    float q0[3], q1[3], e0[3], e1[3];
    bau_path_sample(p, s0 + 1e-4f, q0, e0);
    bau_path_sample(p, s1 - 1e-4f, q1, e1);
    line(project(v, q0[0], EAR, q0[2]), project(v, q1[0], EAR, q1[2]), width, ray_color(p->kind, e0, (s0 - tail) / TRAIL),
         ray_color(p->kind, e1, (s1 - tail) / TRAIL));
  }
}

/* The level, back row to front; `between` called after each row (the
 * capsule, the sources standing in it). */
typedef void (*row_fn)(int z, void* user);

static void draw_level(const view* v, row_fn between, void* user) {
  for (int z = 0; z < NZ; ++z)
    for (int x = 0; x < NX; ++x) {
      const uint8_t m = *cell(x, 0, z);
      const float x0 = (float)x * CELL, z0 = (float)z * CELL, k = roofed[x + NX * z] ? 0.55f : ((x + z) & 1 ? 0.95f : 1.0f);
      quad(project(v, x0, 0, z0), project(v, x0 + CELL, 0, z0), project(v, x0 + CELL, 0, z0 + CELL), project(v, x0, 0, z0 + CELL),
           shade(kColors[m], 0.45f * k));
    }
  for (int z = 0; z < NZ; ++z) {
    for (int x = 0; x < NX; ++x) {
      const int h = wall_h[x + NX * z];
      if (!h) continue;
      uint8_t m = AIR;
      for (int y = 1; y <= h && m == AIR; ++y) m = *cell(x, y, z);
      if (m == AIR) continue;
      const float x0 = (float)x * CELL, z0 = (float)z * CELL, top = (float)h * CELL, a = m == GLASS ? 0.45f : 1.0f;
      /* Its front (if nothing as tall stands in front of it), its top. */
      if (z + 1 >= NZ || wall_h[x + NX * (z + 1)] < h) {
        SDL_FColor c = shade(kColors[m], 0.62f);
        c.a = a;
        quad(project(v, x0, 0, z0 + CELL), project(v, x0 + CELL, 0, z0 + CELL), project(v, x0 + CELL, top, z0 + CELL),
             project(v, x0, top, z0 + CELL), c);
      }
      SDL_FColor c = shade(kColors[m], 0.95f);
      c.a = a;
      quad(project(v, x0, top, z0), project(v, x0 + CELL, top, z0), project(v, x0 + CELL, top, z0 + CELL), project(v, x0, top, z0 + CELL), c);
    }
    if (between) between(z, user);
  }
}

/* The capsule: its shadow, its body, white. */
static void draw_capsule(const view* v, float x, float z) {
  const float r = 0.32f, s = v->s;
  disc(project(v, x, 0, z), r * s * 1.2f, r * s * 0.6f, rgba(0, 0, 0, 0.35f));
  const SDL_FPoint lo = project(v, x, r, z), hi = project(v, x, 1.8f - r, z);
  const SDL_FColor white = rgba(0.93f, 0.94f, 0.96f, 1);
  quad((SDL_FPoint){lo.x - r * s, lo.y}, (SDL_FPoint){lo.x + r * s, lo.y}, (SDL_FPoint){hi.x + r * s, hi.y}, (SDL_FPoint){hi.x - r * s, hi.y}, white);
  disc(lo, r * s, r * s, white);
  disc(hi, r * s, r * s, rgba(1, 1, 1, 1));
  quad((SDL_FPoint){lo.x + r * s * 0.55f, lo.y}, (SDL_FPoint){lo.x + r * s, lo.y}, (SDL_FPoint){hi.x + r * s, hi.y},
       (SDL_FPoint){hi.x + r * s * 0.55f, hi.y}, rgba(0.78f, 0.8f, 0.84f, 1));
}

/* A source: a speaker on the floor, its colour. */
static void draw_source(const view* v, float x, float z, const float* c) {
  const float s = v->s;
  disc(project(v, x, 0, z), 0.5f * s, 0.25f * s, rgba(0, 0, 0, 0.35f));
  const SDL_FPoint a = project(v, x - 0.35f, 0.0f, z), b = project(v, x + 0.35f, 1.4f, z);
  quad((SDL_FPoint){a.x, a.y}, (SDL_FPoint){b.x, a.y}, (SDL_FPoint){b.x, b.y}, (SDL_FPoint){a.x, b.y}, rgba(c[0] * 0.5f, c[1] * 0.5f, c[2] * 0.5f, 1));
  disc(project(v, x, 1.0f, z), 0.22f * s, 0.22f * s, rgba(c[0], c[1], c[2], 1));
  disc(project(v, x, 0.45f, z), 0.13f * s, 0.13f * s, rgba(c[0], c[1], c[2], 1));
}

/* ---------------------------------------------------------------- the panel */

static bui_input input;
static bool click;

static void key_down(SDL_Keycode k, bool shift) {
  switch (k) {
    case SDLK_UP: bui_input_key(&input, BUI_KEY_UP); break;
    case SDLK_DOWN: bui_input_key(&input, BUI_KEY_DOWN); break;
    case SDLK_LEFT: bui_input_key(&input, BUI_KEY_LEFT); break;
    case SDLK_RIGHT: bui_input_key(&input, BUI_KEY_RIGHT); break;
    case SDLK_TAB: bui_input_key(&input, shift ? BUI_KEY_PREV : BUI_KEY_NEXT); break;
    case SDLK_RETURN:
    case SDLK_SPACE: bui_input_key(&input, BUI_KEY_ACTIVATE); break;
    case SDLK_ESCAPE: bui_input_key(&input, BUI_KEY_BACK); break;
    case SDLK_R: bui_input_key(&input, BUI_KEY_REPEAT); break;
    default: break;
  }
}

static void render_ui(SDL_Renderer* r, const bui_draw_cmd* cmds, size_t n) {
  SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
  for (size_t i = 0; i < n; ++i) {
    const bui_draw_cmd* c = &cmds[i];
    SDL_SetRenderDrawColorFloat(r, powf(c->r, 1.0f / 2.2f), powf(c->g, 1.0f / 2.2f), powf(c->b, 1.0f / 2.2f), c->a);
    const SDL_FRect f = {c->x0, c->y0, c->x1 - c->x0, c->y1 - c->y0};
    SDL_RenderFillRect(r, &f);
  }
}

/* BlueUI's audio: speech into one stream, its GUI sounds into another. */
static void play_ui(SDL_AudioStream* speech, SDL_AudioStream* sounds, bui_context* ui) {
  bui_audio a;
  while (bui_next_audio(ui, &a)) {
    if (a.op == BUI_AUDIO_SOUND) {
      if (sounds) SDL_ClearAudioStream(sounds), SDL_PutAudioStreamData(sounds, a.samples, (int)(a.sample_count * sizeof(int16_t)));
    } else {
      if (speech && (a.op == BUI_AUDIO_STOP || a.op == BUI_AUDIO_PLAY)) SDL_ClearAudioStream(speech);
      if (speech && a.samples) SDL_PutAudioStreamData(speech, a.samples, (int)(a.sample_count * sizeof(int16_t)));
    }
    bui_audio_free(&a);
  }
}

static void speed_text(float v, char* out, size_t size, void* user) {
  (void)user;
  snprintf(out, size, "%d M/S", (int)lroundf(v));
}
static void speed_spoken(float v, char* out, size_t size, void* user) {
  (void)user;
  snprintf(out, size, "Ray speed, %d metres a second", (int)lroundf(v));
}
static void percent(float v, char* out, size_t size, void* user) {
  (void)user;
  snprintf(out, size, "%d%%", (int)lroundf(v * 100.0f));
}

/* ---------------------------------------------------------------- audio out */

typedef struct out {
  bau_context* au;
  float volume;
  float* buf;
  int cap;
} out;

/* The device's pull: BlueAudio's mix (its layout's channels; mono doubled
 * out to both), the volume, a soft limit. (SDL holds the stream's lock
 * meanwhile: the layout can't change under it.) */
static void SDLCALL pull(void* user, SDL_AudioStream* stream, int additional, int total) {
  (void)total;
  out* o = (out*)user;
  const int frames = additional / (int)(2 * sizeof(float));
  if (frames <= 0) return;
  const int ch = bau_channels(o->au);
  if (frames * 2 > o->cap) o->cap = frames * 2, o->buf = (float*)realloc(o->buf, (size_t)o->cap * sizeof(float));
  bau_render(o->au, o->buf, frames);
  if (ch == 1)
    for (int i = frames - 1; i >= 0; --i) o->buf[i * 2] = o->buf[i * 2 + 1] = o->buf[i];
  for (int i = 0; i < frames * 2; ++i) o->buf[i] = tanhf(o->buf[i] * o->volume);
  SDL_PutAudioStreamData(stream, o->buf, frames * (int)(2 * sizeof(float)));
}

/* ---------------------------------------------------------------- the frame */

typedef struct demo {
  bool running;
  int room, output;
  bool show_rays, show_heard, turn, music_on, chime_on;
  float ray_speed, volume;
  room where;
  float px, pz, face;
  bau_grid* grid;
} demo;

static const float kMusicColor[3] = {0.35f, 0.75f, 1.0f}, kChimeColor[3] = {1.0f, 0.55f, 0.85f};

typedef struct row_draw {
  const view* v;
  const demo* d;
} row_draw;

static void draw_row(int z, void* user) {
  const row_draw* rd = (const row_draw*)user;
  const demo* d = rd->d;
  if ((int)(d->where.music[1] / CELL) == z) draw_source(rd->v, d->where.music[0], d->where.music[1], kMusicColor);
  if ((int)(d->where.chime[1] / CELL) == z) draw_source(rd->v, d->where.chime[0], d->where.chime[1], kChimeColor);
  if ((int)(d->pz / CELL) == z) draw_capsule(rd->v, d->px, d->pz);
}

static void set_room(bau_context* au, demo* d, int which) {
  d->room = which;
  d->where = build(which);
  d->px = d->where.player[0], d->pz = d->where.player[1];
  if (d->grid) bau_grid_destroy(d->grid);
  d->grid = bau_grid_create(au, NX, NY, NZ, CELL, cells);
  nrays = 0;
}

/* The heard arrow: from the capsule toward where BlueAudio says the source
 * is heard from, as far as its sound travelled, a ring there its loudness. */
static void draw_heard(const view* v, const demo* d, const bau_heard* h, const float* c) {
  if (!h->simulated || h->gain <= 1e-4f) return;
  const float dx = h->direction[0], dz = h->direction[2], l = hypotf(dx, dz);
  if (l < 1e-3f) return;
  const float reach = fminf(h->delay * 343.0f, 30.0f);
  const float hx = d->px + dx / l * reach, hz = d->pz + dz / l * reach;
  const float a = 0.35f + 0.65f * fminf(1.0f, h->gain * 4.0f);
  const SDL_FPoint p0 = project(v, d->px, EAR, d->pz), p1 = project(v, hx, EAR, hz);
  line(p0, p1, 3.0f, rgba(c[0], c[1], c[2], 0.0f), rgba(c[0], c[1], c[2], a));
  const float rr = (0.25f + fminf(1.0f, h->gain * 3.0f) * 0.9f) * v->s;
  ring(p1, rr, rr, 2.5f, rgba(c[0], c[1], c[2], a));
  disc(p1, 0.12f * v->s, 0.12f * v->s, rgba(1, 1, 1, a));
}

static void readout(bui_context* ui, float x, float* y, float p, const char* name, const bau_heard* h, bui_color c) {
  const bui_theme* t = bui_get_theme(ui);
  bui_label(ui, name, x, *y, c, 0.0f, 0.0f), *y += 10 * p;
  if (!h->simulated) {
    bui_label(ui, "NOT TRACED YET", x, *y, t->dim, 0.0f, 0.0f), *y += 14 * p;
    return;
  }
  bui_label(ui, bui_format(ui, "MUFFLE %3d%%  GAIN %.2f", (int)lroundf(h->muffle * 100.0f), h->gain), x, *y, t->text, 0.0f, 0.0f), *y += 10 * p;
  bui_label(ui, bui_format(ui, "%s  REACH %.1f%%  %.1f M", h->occluded ? "BLOCKED" : "IN SIGHT", h->reach * 100.0f, h->distance), x, *y, t->dim,
            0.0f, 0.0f);
  *y += 14 * p;
}

static void panel(bui_context* ui, bau_context* au, demo* d, SDL_AudioStream* stream, const bau_heard* music_h, const bau_heard* chime_h,
                  bui_rect r) {
  const bui_theme* t = bui_get_theme(ui);
  const float p = t->pixel;
  const bui_hints hints = {"W A S D to walk. Up and down to move through the panel, left and right to change. 1 to 6 pick a room. "
                           "Escape quits",
                           NULL};
  bui_screen(ui, "demo", "BlueAudio demo", &hints);
  bui_window(ui, r, "BLUEAUDIO DEMO", bui_format(ui, "%d %s", d->room + 1, kRooms[d->room]));
  float y = r.y0 + 22 * p;
  const float x0 = r.x0 + 6 * p, x1 = r.x1 - 6 * p;
#define ROW() (y += 18 * p, bui_rect_make(x0, y - 18 * p, x1, y - 2 * p))
  bui_option rooms[ROOMS];
  for (int k = 0; k < ROOMS; ++k) rooms[k] = (bui_option){kRooms[k], kAbout[k], NULL};
  const bui_field room_f = {"Room", NULL, NULL, false};
  int room = d->room;
  if (bui_choice_row(ui, "room", ROW(), &room_f, &room, rooms, ROOMS)) set_room(au, d, room);
  const bui_option outs[] = {{"SPEAKERS", "Stereo, panned", NULL}, {"HEADPHONES", "Each ear its own delay and shadow", NULL},
                             {"MONO", "One channel", NULL}};
  const bui_field out_f = {"Output", NULL, NULL, false};
  if (bui_choice_row(ui, "output", ROW(), &out_f, &d->output, outs, 3)) {
    static const bau_layout layouts[] = {BAU_LAYOUT_STEREO, BAU_LAYOUT_HEADPHONES, BAU_LAYOUT_MONO};
    if (stream) SDL_LockAudioStream(stream);
    bau_set_layout(au, layouts[d->output]);
    if (stream) SDL_UnlockAudioStream(stream);
  }
  const bui_field rays_f = {"Rays", "BlueAudio's debug paths, slowed down and flat", NULL, false};
  bui_toggle_row(ui, "rays", ROW(), &rays_f, &d->show_rays, NULL, NULL);
  const bui_field speed_f = {"Ray speed", "Sound goes 343 metres a second", NULL, false};
  bui_slider_row(ui, "speed", ROW(), &speed_f, &d->ray_speed, 2.0f, 30.0f, 1.0f, speed_text, speed_spoken, NULL);
  const bui_field heard_f = {"Heard from", "Arrows to where each source is heard from", NULL, false};
  bui_toggle_row(ui, "heard", ROW(), &heard_f, &d->show_heard, NULL, NULL);
  const bui_field turn_f = {"Turn as you walk", "Off: you always face up the screen", NULL, false};
  bui_toggle_row(ui, "turn", ROW(), &turn_f, &d->turn, NULL, NULL);
  const bui_field music_f = {"Music", "Music by Joystock", NULL, false};
  bui_toggle_row(ui, "music", ROW(), &music_f, &d->music_on, NULL, NULL);
  const bui_field chime_f = {"Chime", "Struck every two seconds", NULL, false};
  bui_toggle_row(ui, "chime", ROW(), &chime_f, &d->chime_on, NULL, NULL);
  const bui_field volume_f = {"Volume", NULL, NULL, false};
  bui_slider_row(ui, "volume", ROW(), &volume_f, &d->volume, 0.0f, 1.0f, 0.05f, percent, NULL, NULL);
#undef ROW

  /* What BlueAudio worked out. */
  y += 6 * p;
  bui_fill(ui, bui_rect_make(x0, y, x1, y + p), t->rule), y += 6 * p;
  readout(ui, x0, &y, p, "MUSIC", music_h, bui_rgb(kMusicColor[0], kMusicColor[1], kMusicColor[2]));
  readout(ui, x0, &y, p, "CHIME", chime_h, bui_rgb(kChimeColor[0], kChimeColor[1], kChimeColor[2]));
  bau_echo e;
  bau_listener_echo(au, &e);
  bui_label(ui, "ECHO", x0, y, bui_rgb(0.78f, 0.5f, 1.0f), 0.0f, 0.0f), y += 10 * p;
  bui_label(ui, bui_format(ui, "LEVEL %3d%%  RINGS %.2f S", (int)lroundf(e.level * 100.0f), e.decay), x0, y, t->text, 0.0f, 0.0f), y += 10 * p;
  bui_label(ui, bui_format(ui, "SHUT IN %3d%%  ECHOES %d", (int)lroundf(e.enclosure * 100.0f), e.taps), x0, y, t->dim, 0.0f, 0.0f);
  y += 14 * p;
  bau_stats st;
  bau_get_stats(au, &st);
  bui_label(ui, bui_format(ui, "%d RAYS X %d BOUNCES  %.1f MS", st.rays, st.bounces, st.simulate_ms), x0, y, t->dim, 0.0f, 0.0f);

  /* The legend, the credit; Quit. */
  float yb = r.y1 - 54 * p;
  bui_label(ui, "RAYS: WHITE ALL BANDS, ORANGE HIGHS LOST", x0, yb, t->blurb, 0.0f, 0.0f), yb += 10 * p;
  bui_label(ui, "GOLD FOUND A SOURCE, VIOLET CAME BACK", x0, yb, t->blurb, 0.0f, 0.0f), yb += 12 * p;
  bui_label(ui, "MUSIC BY JOYSTOCK - HTTPS://WWW.JOYSTOCK.ORG", x0, yb, t->value, 0.0f, 0.0f);
  const bui_button_opts quit = {"Quit the demo", false, true, 0.0f};
  if (bui_button(ui, "quit", "QUIT", bui_rect_make(x1 - 44 * p, r.y1 - 22 * p, x1, r.y1 - 8 * p), &quit)) d->running = false;
}

int main(void) {
  if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO)) return 1;
  SDL_Window* win = NULL;
  SDL_Renderer* ren = NULL;
  if (!SDL_CreateWindowAndRenderer("BlueAudio demo", 1440, 820, SDL_WINDOW_RESIZABLE, &win, &ren)) return 1;
  SDL_SetRenderVSync(ren, 1);

  /* BlueAudio: its materials, the level, the two sources. */
  bau_config cfg = {0};
  cfg.rays = 1024, cfg.max_distance = 80.0f;
  bau_context* au = bau_create(&cfg);
  bau_material materials[MATERIALS];
  materials[CONCRETE] = bau_material_preset(BAU_MATERIAL_CONCRETE);
  materials[CARPET] = bau_material_preset(BAU_MATERIAL_CARPET);
  materials[GLASS] = bau_material_preset(BAU_MATERIAL_GLASS);
  materials[WOOD] = bau_material_preset(BAU_MATERIAL_WOOD);
  materials[GROUND] = bau_material_preset(BAU_MATERIAL_GROUND);
  for (int m = 1; m < MATERIALS; ++m) bau_set_material(au, (uint8_t)m, materials[m]);

  music tune = load_music();
  chime bell = {0, true};
  bau_source* music_src = bau_source_create(au, fill_music, &tune);
  bau_source* chime_src = bau_source_create(au, fill_chime, &bell);
  bau_source_set_gain(music_src, 0.8f), bau_source_set_rolloff(music_src, 4.0f, 80.0f);
  bau_source_set_gain(chime_src, 0.6f), bau_source_set_rolloff(chime_src, 4.0f, 80.0f);

  demo d = {0};
  d.running = true, d.show_rays = true, d.show_heard = true, d.music_on = true, d.chime_on = true;
  d.ray_speed = 9.0f, d.volume = 0.8f;
  set_room(au, &d, ROOM_CORRIDORS);

  out o = {au, d.volume, NULL, 0};
  const SDL_AudioSpec spec = {SDL_AUDIO_F32, 2, RATE};
  SDL_AudioStream* stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, pull, &o);
  if (stream) SDL_ResumeAudioStreamDevice(stream);
  const SDL_AudioSpec voice = {SDL_AUDIO_S16, 1, RATE};
  SDL_AudioStream* speech = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &voice, NULL, NULL);
  SDL_AudioStream* sounds = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &voice, NULL, NULL);
  if (speech) SDL_ResumeAudioStreamDevice(speech);
  if (sounds) SDL_ResumeAudioStreamDevice(sounds);

  bui_config ui_cfg = {0};
  ui_cfg.sample_rate = RATE;
  bui_context* ui = bui_create(&ui_cfg);
  bui_input_clear(&input);

  double last = (double)SDL_GetTicksNS() * 1e-9, simulated = 0.0, launched = 0.0;
  float launch_turn = 0.0f;
  while (d.running) {
    const double now = (double)SDL_GetTicksNS() * 1e-9;
    const float dt = (float)fmin(now - last, 0.05);
    last = now;
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
      if (e.type == SDL_EVENT_QUIT) d.running = false;
      else if (e.type == SDL_EVENT_KEY_DOWN) {
        input.device = BUI_DEVICE_KEYBOARD;
        if (e.key.key >= SDLK_1 && e.key.key <= SDLK_6) set_room(au, &d, (int)(e.key.key - SDLK_1));
        else key_down(e.key.key, (e.key.mod & SDL_KMOD_SHIFT) != 0);
      } else if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && e.button.button == SDL_BUTTON_LEFT) {
        click = true, input.click_at.x = e.button.x, input.click_at.y = e.button.y;
      } else if (e.type == SDL_EVENT_MOUSE_WHEEL) {
        input.wheel += e.wheel.y;
      }
    }

    /* Walking. */
    const bool* keys = SDL_GetKeyboardState(NULL);
    float mx = (float)(keys[SDL_SCANCODE_D] - keys[SDL_SCANCODE_A]), mz = (float)(keys[SDL_SCANCODE_S] - keys[SDL_SCANCODE_W]);
    const float ml = hypotf(mx, mz);
    if (ml > 0.0f) {
      mx /= ml, mz /= ml;
      const float step = 4.5f * dt;
      if (!blocked(d.px + mx * step, d.pz, 0.3f)) d.px += mx * step;
      if (!blocked(d.px, d.pz + mz * step, 0.3f)) d.pz += mz * step;
      d.face = atan2f(mz, mx);
    }
    const float face = d.turn ? d.face : -1.5707963f;
    const float lp[3] = {d.px, EAR, d.pz}, ahead[3] = {cosf(face), 0.0f, sinf(face)}, up[3] = {0, 1, 0};
    bau_set_listener(au, lp, ahead, up);
    const float mp[3] = {d.where.music[0], 1.2f, d.where.music[1]}, cp[3] = {d.where.chime[0], 1.2f, d.where.chime[1]};
    bau_source_set_position(music_src, mp), bau_source_set_position(chime_src, cp);
    tune.on = d.music_on, bell.on = d.chime_on, o.volume = d.volume;
    if (now - simulated >= 1.0 / 30.0) bau_simulate(au), simulated = now;

    /* The demo's rays: a ring of them out every so often, on their way. */
    if (d.show_rays && now - launched >= 0.45) {
      launched = now, launch_turn += 0.37f;
      launch(au, launch_turn);
    }
    advance(dt, d.ray_speed);
    if (!d.show_rays) nrays = 0;

    /* The panel (BlueUI). */
    int w = 0, h = 0;
    SDL_GetRenderOutputSize(ren, &w, &h);
    float fx, fy;
    const SDL_MouseButtonFlags buttons = SDL_GetMouseState(&fx, &fy);
    input.mouse.x = fx, input.mouse.y = fy, input.mouse_down = (buttons & SDL_BUTTON_LMASK) != 0, input.clicked = click;
    input.time = now, input.screen_size.x = (float)w, input.screen_size.y = (float)h;
    input.speech_playing = speech && SDL_GetAudioStreamQueued(speech) > 0;
    bui_theme theme = bui_default_theme();
    theme.pixel = fmaxf(1.0f, roundf(fminf((float)h / 400.0f, (float)w / 720.0f)));
    bui_set_theme(ui, &theme);
    const float pw = 236.0f * theme.pixel;
    bau_heard music_h, chime_h;
    bau_source_heard(music_src, &music_h), bau_source_heard(chime_src, &chime_h);
    bui_begin_frame(ui, &input);
    bui_input_clear(&input), input.device = BUI_DEVICE_KEYBOARD, click = false;
    panel(ui, au, &d, stream, &music_h, &chime_h, bui_rect_make((float)w - pw - 8, 8, (float)w - 8, (float)h - 8));
    bui_end_frame(ui);
    play_ui(speech, sounds, ui);

    /* The level. */
    const float vw = (float)w - pw - 24, vh = (float)h - 16;
    view v;
    v.s = fminf(vw / (NX * CELL), vh / (NZ * CELL + 4.0f));
    v.ox = 8 + (vw - NX * CELL * v.s) * 0.5f, v.oy = 8 + 4.0f * v.s + (vh - (NZ * CELL + 4.0f) * v.s) * 0.5f;
    nverts = 0;
    row_draw rd = {&v, &d};
    draw_level(&v, draw_row, &rd);
    for (int i = 0; i < nrays; ++i) draw_ray(&v, &rays[i], rays[i].path.kind == BAU_PATH_OUT ? 2.5f : 2.0f);
    if (d.show_heard) draw_heard(&v, &d, &music_h, kMusicColor), draw_heard(&v, &d, &chime_h, kChimeColor);
    /* (Which way you face.) */
    const SDL_FPoint f0 = project(&v, d.px, 2.1f, d.pz), f1 = project(&v, d.px + cosf(face) * 0.7f, 2.1f, d.pz + sinf(face) * 0.7f);
    line(f0, f1, 3.0f, rgba(1, 1, 1, 0.2f), rgba(1, 1, 1, 0.9f));

    SDL_SetRenderDrawColor(ren, 12, 14, 18, 255);
    SDL_RenderClear(ren);
    SDL_RenderGeometry(ren, NULL, verts, nverts, NULL, 0);
    size_t n = 0;
    const bui_draw_cmd* cmds = bui_get_geometry(ui, &n);
    render_ui(ren, cmds, n);
    SDL_RenderPresent(ren);
  }

  if (stream) SDL_DestroyAudioStream(stream);
  if (speech) SDL_DestroyAudioStream(speech);
  if (sounds) SDL_DestroyAudioStream(sounds);
  bui_destroy(ui);
  bau_destroy(au);
  free(tune.pcm), free(o.buf), free(verts);
  SDL_DestroyRenderer(ren);
  SDL_DestroyWindow(win);
  SDL_Quit();
  return 0;
}
