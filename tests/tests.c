/* BlueAudio's tests: worlds built of voxels (an open field, a wall, an L of
 * corridor, a shut room, a cliff), sources set about in them; what's heard
 * read back (muffle, direction, the echo), what's rendered checked. */

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "blueaudio/blueaudio.h"

static int failures = 0;

static void expect(bool ok, const char* what, const char* said) {
  if (said) printf("%s %s (%s)\n", ok ? "  ok  " : "  FAIL", what, said);
  else printf("%s %s\n", ok ? "  ok  " : "  FAIL", what);
  failures += !ok;
}

static char said[256];
#define SAY(...) (snprintf(said, sizeof said, __VA_ARGS__), said)

/* ----------------------------------------------------------- worlds */

enum { AIR = 0, CONCRETE = 1, GROUND = 2 };

typedef struct world {
  int nx, ny, nz;
  float cell;
  uint8_t* cells;
} world;

static world make(int nx, int ny, int nz, float cell, uint8_t fill) {
  world w = {nx, ny, nz, cell, (uint8_t*)malloc((size_t)nx * ny * nz)};
  memset(w.cells, fill, (size_t)nx * ny * nz);
  return w;
}

/* Cells x0..x1, y0..y1, z0..z1 (inclusive) set to m. */
static void box(world* w, int x0, int y0, int z0, int x1, int y1, int z1, uint8_t m) {
  for (int z = z0; z <= z1; ++z)
    for (int y = y0; y <= y1; ++y)
      for (int x = x0; x <= x1; ++x)
        if (x >= 0 && y >= 0 && z >= 0 && x < w->nx && y < w->ny && z < w->nz)
          w->cells[(size_t)x + (size_t)w->nx * ((size_t)y + (size_t)w->ny * z)] = m;
}

static bau_context* context(void) {
  bau_config c = {0};
  c.rays = 2048;
  bau_context* au = bau_create(&c);
  bau_set_material(au, CONCRETE, bau_material_preset(BAU_MATERIAL_CONCRETE));
  bau_set_material(au, GROUND, bau_material_preset(BAU_MATERIAL_GROUND));
  return au;
}

static bau_grid* add(bau_context* au, world* w) {
  bau_grid* g = bau_grid_create(au, w->nx, w->ny, w->nz, w->cell, w->cells);
  free(w->cells);
  return g;
}

static void listen_at(bau_context* au, float x, float y, float z) {
  const float pos[3] = {x, y, z}, ahead[3] = {0, 0, -1}, up[3] = {0, 1, 0};
  bau_set_listener(au, pos, ahead, up);
}

static int silence(void* user, float* mono, int frames) {
  (void)user;
  memset(mono, 0, (size_t)frames * sizeof(float));
  return frames;
}

static bau_source* source_at(bau_context* au, float x, float y, float z) {
  bau_source* s = bau_source_create(au, silence, NULL);
  const float p[3] = {x, y, z};
  bau_source_set_position(s, p);
  return s;
}

static void simulate(bau_context* au, int times) {
  for (int i = 0; i < times; ++i) bau_simulate(au);
}

static float angle(const float* a, float x, float y, float z) {
  const float l = sqrtf(x * x + y * y + z * z);
  float d = (a[0] * x + a[1] * y + a[2] * z) / l;
  d = d > 1.0f ? 1.0f : d < -1.0f ? -1.0f : d;
  return acosf(d) * 180.0f / 3.14159265f;
}

/* ----------------------------------------------------------- the tests */

static void open_field(void) {
  printf("open field\n");
  bau_context* au = context();
  listen_at(au, 0, 1.7f, 0);
  bau_source* s = source_at(au, 0, 1.7f, -10);
  simulate(au, 4);
  bau_heard h;
  bau_source_heard(s, &h);
  expect(h.simulated && !h.occluded, "traced, nothing in the way", NULL);
  expect(h.muffle < 0.1f, "not muffled", SAY("muffle %.3f", h.muffle));
  expect(angle(h.direction, 0, 0, -1) < 2.0f, "heard straight ahead", SAY("%.1f deg off", angle(h.direction, 0, 0, -1)));
  expect(fabsf(h.delay - 10.0f / 343.0f) < 0.001f, "its delay, its distance's", SAY("%.4f s", h.delay));
  bau_echo e;
  bau_listener_echo(au, &e);
  expect(e.level == 0.0f && e.taps == 0, "no echo with nothing about", SAY("level %.3f", e.level));
  bau_destroy(au);
}

static void wall_between(void) {
  printf("a wall between\n");
  bau_context* au = context();
  world w = make(60, 20, 60, 0.5f, AIR);
  box(&w, 0, 0, 30, 59, 19, 30, CONCRETE); /* (z 15 m, all across) */
  add(au, &w);
  listen_at(au, 15, 2, 25);
  bau_source* s = source_at(au, 15, 2, 5);
  simulate(au, 6);
  bau_heard h;
  bau_source_heard(s, &h);
  expect(h.occluded, "occluded", NULL);
  expect(h.muffle > 0.8f, "muffled", SAY("muffle %.3f", h.muffle));
  expect(h.gain < 0.03f, "hardly heard", SAY("gain %.4f", h.gain));
  bau_destroy(au);
}

static void round_a_corner(void) {
  printf("round a corner\n");
  bau_context* au = context();
  world w = make(64, 8, 64, 0.5f, CONCRETE);
  box(&w, 28, 1, 20, 31, 6, 50, AIR); /* down -z: x 14..16 m, z 10..25 m */
  box(&w, 28, 1, 20, 56, 6, 23, AIR); /* then +x: z 10..12 m, out to x 28 m */
  add(au, &w);
  listen_at(au, 15, 2, 24);
  bau_source* s = source_at(au, 26, 2, 11);
  simulate(au, 10);
  bau_heard h;
  bau_source_heard(s, &h);
  const float corner = angle(h.direction, 0, 0, -13), straight = angle(h.direction, 11, 0, -13);
  expect(h.occluded, "the wall in the way", NULL);
  expect(h.reach > 0.0f && h.reflected > 0.0f, "found by the rays", SAY("reach %.3f", h.reach));
  expect(corner < 25.0f && corner < straight, "heard from the corner, not through the wall",
         SAY("%.1f deg from the corner, %.1f from the source", corner, straight));
  expect(h.muffle > 0.05f, "muffled a little", SAY("muffle %.3f", h.muffle));
  bau_destroy(au);
}

static void room_and_field(void) {
  printf("a shut room, an open field\n");
  bau_context* room = context();
  world w = make(26, 10, 26, 0.5f, CONCRETE);
  box(&w, 1, 1, 1, 24, 8, 24, AIR);
  add(room, &w);
  listen_at(room, 6.5f, 2.5f, 6.5f);
  simulate(room, 12);
  bau_echo r;
  bau_listener_echo(room, &r);

  bau_context* field = context();
  world g = make(80, 1, 80, 0.5f, GROUND);
  add(field, &g);
  listen_at(field, 20, 1.7f, 20);
  simulate(field, 12);
  bau_echo f;
  bau_listener_echo(field, &f);
  expect(r.enclosure > 0.9f && f.enclosure < 0.2f, "the room shut in, the field open",
         SAY("%.2f, %.2f", r.enclosure, f.enclosure));
  expect(r.level > 0.3f && r.level > f.level * 3.0f, "the room echoes", SAY("level %.3f, field %.3f", r.level, f.level));
  expect(r.decay > 1.0f && f.decay < 0.3f, "the room rings", SAY("RT60 %.2f s, field %.2f s", r.decay, f.decay));
  bau_destroy(room);
  bau_destroy(field);
}

static void cliff(void) {
  printf("a cliff answering\n");
  bau_context* au = context();
  world w = make(120, 60, 4, 0.5f, CONCRETE); /* 60 m wide, 30 tall, at z -36..-34 */
  bau_grid* g = add(au, &w);
  const float at[3] = {-30, -10, -36};
  bau_grid_set_pose(g, at, 0.0f);
  listen_at(au, 0, 1.7f, 0);
  simulate(au, 12);
  bau_echo e;
  bau_listener_echo(au, &e);
  int found = 0;
  for (int t = 0; t < e.taps; ++t)
    if (fabsf(e.tap_delay[t] - 68.0f / 343.0f) < 0.015f && e.tap_gain[t] > 0.0f) found = 1;
  expect(found, "an echo back off it", SAY("%d taps, the first %.3f s (expected %.3f)", e.taps, e.taps ? e.tap_delay[0] : 0.0f, 68.0f / 343.0f));
  expect(found && angle(e.tap_direction[0], 0, 0, -1) < 30.0f, "from ahead", NULL);
  bau_destroy(au);
}

static void movers_and_edits(void) {
  printf("moved, edited\n");
  bau_context* au = context();
  world w = make(40, 20, 1, 0.5f, CONCRETE); /* a 20 x 10 m panel */
  bau_grid* g = add(au, &w);
  const float away[3] = {100, 0, 100}, between[3] = {-10, -5, -5};
  bau_grid_set_pose(g, away, 0.0f);
  listen_at(au, 0, 0, 0);
  bau_source* s = source_at(au, 0, 0, -10);
  simulate(au, 1);
  bau_heard h;
  bau_source_heard(s, &h);
  expect(!h.occluded, "the panel elsewhere", NULL);
  bau_grid_set_pose(g, between, 0.0f);
  simulate(au, 1);
  bau_source_heard(s, &h);
  expect(h.occluded, "moved between", NULL);
  /* Turned a quarter about y: edge on (thin along the line? no: 20 m deep along x). */
  const float turned[3] = {0.1f, -5, 0};
  bau_grid_set_pose(g, turned, 1.5707963f);
  simulate(au, 1);
  bau_source_heard(s, &h);
  expect(!h.occluded, "turned out of the way", NULL);
  bau_grid_set_pose(g, between, 0.0f);
  uint8_t hole[4 * 4] = {0};
  bau_grid_update(g, 18, 8, 0, 4, 4, 1, hole); /* (a hole where the line crosses: x 10 m, y 5 m) */
  simulate(au, 1);
  bau_source_heard(s, &h);
  expect(!h.occluded, "a hole knocked through", NULL);
  bau_destroy(au);
}

static void layouts(void) {
  printf("layouts\n");
  static const bau_layout all[] = {BAU_LAYOUT_MONO, BAU_LAYOUT_STEREO, BAU_LAYOUT_HEADPHONES, BAU_LAYOUT_QUAD,
                                   BAU_LAYOUT_SURROUND_51, BAU_LAYOUT_SURROUND_71};
  static const int channels[] = {1, 2, 2, 4, 6, 8};
  for (int l = 0; l < 6; ++l) {
    bau_config c = {0};
    c.layout = all[l];
    bau_context* au = bau_create(&c);
    expect(bau_channels(au) == channels[l], "its channels", SAY("layout %d: %d", l, bau_channels(au)));
    bau_destroy(au);
  }
}

/* An impulse at the start, then silence; ends after `length` frames. */
typedef struct impulse { int at, length; } impulse;
static int fill_impulse(void* user, float* mono, int frames) {
  impulse* im = (impulse*)user;
  int n = 0;
  for (; n < frames && im->at < im->length; ++n, ++im->at) mono[n] = im->at == 0 ? 1.0f : 0.0f;
  return n;
}

static void rendering(void) {
  printf("rendering\n");
  bau_context* au = context();
  listen_at(au, 0, 0, 0);
  float* out = (float*)calloc(16384 * 8, sizeof(float));
  bau_render(au, out, 4096);
  float peak = 0.0f;
  for (int i = 0; i < 4096 * 2; ++i) peak = fmaxf(peak, fabsf(out[i]));
  expect(peak == 0.0f, "nothing: silence", NULL);

  impulse im = {0, 24000};
  bau_source* s = bau_source_create(au, fill_impulse, &im);
  const float p[3] = {0, 0, -34.3f};
  bau_source_set_position(s, p);
  simulate(au, 2);
  bau_render(au, out, 16384);
  int at = -1;
  peak = 0.0f;
  for (int i = 0; i < 16384; ++i)
    if (out[i * 2] > peak) peak = out[i * 2], at = i;
  expect(abs(at - 4800) <= 4, "delayed by its distance", SAY("peak at %d (expected 4800)", at));
  expect(fabsf(out[at * 2] - out[at * 2 + 1]) < 1e-4f, "ahead: both sides alike", NULL);
  expect(!bau_source_finished(s), "still going", NULL);
  bau_render(au, out, 16384);
  expect(bau_source_finished(s), "finished, played out", NULL);

  /* Hard left: all in the left. */
  bau_source_destroy(s);
  im.at = 0, im.length = 1 << 30;
  s = bau_source_create(au, fill_impulse, &im);
  const float left[3] = {-5, 0, 0};
  bau_source_set_position(s, left);
  simulate(au, 2);
  bau_render(au, out, 4096);
  float l = 0.0f, r = 0.0f;
  for (int i = 0; i < 4096; ++i) l += fabsf(out[i * 2]), r += fabsf(out[i * 2 + 1]);
  expect(l > 0.0f && r < l * 0.01f, "left: in the left", SAY("%.4f, %.4f", l, r));

  bau_set_layout(au, BAU_LAYOUT_SURROUND_71);
  im.at = 0;
  bau_render(au, out, 4096);
  float e[8] = {0};
  for (int i = 0; i < 4096; ++i)
    for (int c = 0; c < 8; ++c) e[c] += fabsf(out[i * 8 + c]);
  expect(e[6] > 0.0f && e[1] + e[5] + e[7] + e[3] < e[6] * 0.01f, "7.1: in the side left", NULL);
  free(out);
  bau_destroy(au);
}

static void deterministic(void) {
  printf("the same seed, the same\n");
  bau_heard h[2];
  for (int k = 0; k < 2; ++k) {
    bau_context* au = context();
    world w = make(26, 10, 26, 0.5f, CONCRETE);
    box(&w, 1, 1, 1, 24, 8, 24, AIR);
    box(&w, 12, 1, 1, 12, 8, 20, CONCRETE);
    add(au, &w);
    listen_at(au, 3, 2, 3);
    bau_source* s = source_at(au, 10, 2, 3);
    simulate(au, 3);
    bau_source_heard(s, &h[k]);
    bau_destroy(au);
  }
  expect(memcmp(&h[0], &h[1], sizeof h[0]) == 0, "identical", NULL);
}

static void debug_paths(void) {
  printf("debug paths\n");
  bau_context* au = context();
  world w = make(64, 8, 64, 0.5f, CONCRETE);
  box(&w, 28, 1, 20, 31, 6, 50, AIR);
  box(&w, 28, 1, 20, 56, 6, 23, AIR);
  add(au, &w);
  listen_at(au, 15, 2, 24);
  bau_source* s = source_at(au, 26, 2, 11);
  static bau_path out[4000];
  bau_path_config c = {0};
  c.flat = 1, c.rays = 64;
  const int n = bau_debug_paths(au, &c, out, 4000);
  int outs = 0, found = 0, echoes = 0, ordered = 1, level = 1, falling = 1;
  for (int i = 0; i < n; ++i) {
    const bau_path* p = &out[i];
    outs += p->kind == BAU_PATH_OUT, found += p->kind == BAU_PATH_FOUND && p->source == s, echoes += p->kind == BAU_PATH_ECHO;
    if (p->kind != BAU_PATH_OUT && (p->parent < 0 || p->parent >= i || out[p->parent].kind != BAU_PATH_OUT ||
                                    p->start > out[p->parent].distance[out[p->parent].points - 1] + 1e-3f))
      ordered = 0;
    for (int k = 0; k < p->points; ++k) {
      if (fabsf(p->at[k][1] - 2.0f) > 1e-3f) level = 0;
      if (k && p->energy[k][1] > p->energy[k - 1][1] + 1e-6f) falling = 0;
    }
  }
  expect(outs == 64 && found > 0 && echoes > 0, "a fan out, branches to the source round the corner and back",
         SAY("%d out, %d found it, %d echoes", outs, found, echoes));
  expect(ordered, "each branch after its own ray, leaving it along its length", NULL);
  expect(level, "flat: every point at the listener's height", NULL);
  expect(falling, "the energy only falls along a path", NULL);
  /* Sampled halfway along a first segment: halfway between its points. */
  const bau_path* p = &out[0];
  float at[3], e[3];
  bau_path_sample(p, p->distance[1] * 0.5f, at, e);
  expect(fabsf(at[0] - 0.5f * (p->at[0][0] + p->at[1][0])) < 1e-3f && fabsf(e[1] - 0.5f * (p->energy[0][1] + p->energy[1][1])) < 1e-4f,
         "sampled along it: lerped", NULL);
  /* Colours: every band, cool; the highs gone, warm; found, gold. */
  const float all[3] = {1, 1, 1}, dull[3] = {1, 0.6f, 0.05f};
  float cool[4], warm[4], gold[4];
  bau_path_color(BAU_PATH_OUT, all, cool), bau_path_color(BAU_PATH_OUT, dull, warm), bau_path_color(BAU_PATH_FOUND, all, gold);
  expect(cool[2] > cool[0] && warm[0] > warm[2] && gold[0] > gold[2] && cool[3] > 0.99f, "coloured by what they carry", NULL);
  /* Every way (not flat): some go up and down. */
  c.flat = 0;
  const int m = bau_debug_paths(au, &c, out, 4000);
  int vertical = 0;
  for (int i = 0; i < m; ++i)
    if (out[i].kind == BAU_PATH_OUT && out[i].points > 1 && fabsf(out[i].at[1][1] - 2.0f) > 0.4f) vertical = 1;
  expect(m > 0 && vertical, "every way: out of the plane too", NULL);
  bau_destroy(au);
}

/* ----------------------------------------------------------- smoothness */

/* A steady 440 Hz tone (its phase its own: each source a `tone`). */
typedef struct tone { double phase; } tone;
static int fill_tone(void* user, float* mono, int frames) {
  tone* t = (tone*)user;
  for (int i = 0; i < frames; ++i) mono[i] = (float)sin(t->phase), t->phase += 2.0 * 3.14159265358979 * 440.0 / 48000.0;
  return frames;
}

/* Still, in a shut concrete hall: the tone's loudness (10 ms at a time)
 * steady, though each simulate's rays are different. */
static void still_in_a_hall(void) {
  printf("still, in a hall\n");
  bau_context* au = context();
  world w = make(60, 14, 40, 0.5f, AIR);
  box(&w, 0, 0, 0, 59, 13, 39, CONCRETE);
  box(&w, 1, 1, 1, 58, 12, 38, AIR);
  add(au, &w);
  listen_at(au, 10, 1.7f, 10);
  tone t = {0};
  bau_source* s = bau_source_create(au, fill_tone, &t);
  const float at[3] = {20, 1.2f, 4};
  bau_source_set_position(s, at);
  const int block = 1600, sims = 80;
  float* out = (float*)malloc(sizeof(float) * block * 2 * sims);
  float d0 = 1e9f, d1 = 0.0f;
  for (int k = 0; k < sims; ++k) {
    bau_simulate(au);
    bau_render(au, out + (size_t)k * block * 2, block);
    bau_heard h;
    bau_source_heard(s, &h);
    if (k >= 20) d0 = fminf(d0, h.delay), d1 = fmaxf(d1, h.delay);
  }
  double lo = 1e9, hi = 0.0;
  for (int w10 = 20 * block / 480; w10 < sims * block / 480; ++w10) {
    double e = 0.0;
    for (int i = 0; i < 480; ++i) e += (double)out[(w10 * 480 + i) * 2] * out[(w10 * 480 + i) * 2];
    e = sqrt(e / 480.0), lo = fmin(lo, e), hi = fmax(hi, e);
  }
  const float straight = sqrtf(100.0f + 0.25f + 36.0f) / 343.0f;
  expect(fabsf(d0 - straight) < 0.001f && d1 - d0 < 0.0002f, "its delay the straight line's, steady",
         SAY("%.2f..%.2f ms (straight %.2f)", d0 * 1000.0f, d1 * 1000.0f, straight * 1000.0f));
  expect(20.0 * log10(hi / lo) < 2.0, "its loudness steady", SAY("%.1f dB from 10 ms to 10 ms", 20.0 * log10(hi / lo)));
  free(out);
  bau_destroy(au);
}

/* A tone driving past at 10 m/s, 8 m off (moved every 60th of a second,
 * simulated every 30th, rendered 512 frames at a time): its pitch through
 * the pass a smooth Doppler bend, no bursts. */
static void driving_past(void) {
  printf("driving past\n");
  bau_context* au = context();
  listen_at(au, 0, 1.7f, 0);
  tone t = {0};
  bau_source* s = bau_source_create(au, fill_tone, &t);
  const int total = 48000 * 3;
  float* out = (float*)malloc(sizeof(float) * total * 2);
  double frame = 0.0, sim = 0.0;
  for (int at = 0; at < total; at += 512) {
    const double now = at / 48000.0;
    if (now >= frame) {
      const float p[3] = {(float)(-15.0 + 10.0 * now), 1.7f, -8.0f};
      bau_source_set_position(s, p);
      frame += 1.0 / 60.0;
    }
    if (now >= sim) bau_simulate(au), sim += 1.0 / 30.0;
    bau_render(au, out + (size_t)at * 2, 512);
  }
  /* Its pitch (zero crossings, 5 ms at a time) against the smooth Doppler,
   * through the middle second; how much it moves from one 5 ms to the next
   * beyond the bend's own. */
  double worst = 0.0, jitter = 0.0, prev = -1.0, prev_smooth = 0.0;
  for (int w5 = 200; w5 < 400; ++w5) {
    const int a = w5 * 240, b = a + 240;
    double first = -1.0, last = -1.0;
    int crossings = 0;
    for (int i = a + 1; i < b; ++i) {
      const float x0 = out[(i - 1) * 2], x1 = out[i * 2];
      if (x0 <= 0.0f && x1 > 0.0f) {
        const double z = i - 1 + x0 / (x0 - x1);
        if (first < 0.0) first = z;
        else ++crossings;
        last = z;
      }
    }
    if (crossings < 1) continue;
    const double f = crossings * 48000.0 / (last - first);
    const double now = (a + 120) / 48000.0, x = -15.0 + 10.0 * now, d = sqrt(x * x + 64.0), smooth = 440.0 * 343.0 / (343.0 + 10.0 * x / d);
    worst = fmax(worst, fabs(f - smooth));
    if (prev > 0.0) jitter = fmax(jitter, fabs((f - prev) - (smooth - prev_smooth)));
    prev = f, prev_smooth = smooth;
  }
  expect(worst < 3.0 && jitter < 1.0, "its pitch a smooth Doppler bend",
         SAY("%.2f Hz off it at worst, %.2f Hz from 5 ms to 5 ms past its own (a semitone: 26 Hz)", worst, jitter));
  free(out);
  bau_destroy(au);
}

/* Round a corner, still, fewer rays (as a game short of time): what's
 * published hardly wanders from one simulate to the next. */
static void steady_round_a_corner(void) {
  printf("steady round a corner\n");
  bau_config c = {0};
  c.rays = 512;
  bau_context* au = bau_create(&c);
  bau_set_material(au, CONCRETE, bau_material_preset(BAU_MATERIAL_CONCRETE));
  world w = make(64, 8, 64, 0.5f, CONCRETE);
  box(&w, 28, 1, 20, 31, 6, 50, AIR);
  box(&w, 28, 1, 20, 56, 6, 23, AIR);
  add(au, &w);
  listen_at(au, 15, 2, 24);
  bau_source* s = source_at(au, 26, 2, 11);
  float g0 = 1e9f, g1 = 0.0f, turn = 0.0f, prev[3] = {0};
  for (int k = 0; k < 100; ++k) {
    bau_simulate(au);
    bau_heard h;
    bau_source_heard(s, &h);
    if (k >= 20) {
      g0 = fminf(g0, h.gain), g1 = fmaxf(g1, h.gain);
      turn = fmaxf(turn, angle(prev, h.direction[0], h.direction[1], h.direction[2]));
    }
    memcpy(prev, h.direction, sizeof prev);
  }
  expect(20.0f * log10f(g1 / g0) < 1.5f && turn < 3.0f, "its loudness and direction steady",
         SAY("%.1f dB swing, %.1f deg steps at most", 20.0f * log10f(g1 / g0), turn));
  bau_destroy(au);
}

/* A source going behind a pillar, a step a simulate: faded, never
 * snapped (its edge passed by a ring of rays, not one). */
static void behind_a_pillar(void) {
  printf("behind a pillar\n");
  bau_context* au = context();
  world w = make(80, 10, 80, 0.5f, GROUND);
  box(&w, 0, 1, 0, 79, 9, 79, AIR);
  box(&w, 38, 1, 30, 41, 9, 33, CONCRETE); /* (2 m square, z 15..17 m, x 19..21 m) */
  add(au, &w);
  listen_at(au, 20, 1.7f, 25);
  bau_source* s = source_at(au, 14, 1.7f, 5);
  /* (A step's size against its loudness in the open: a snap, all of it at
   * once; faded, a little at a time.) */
  float worst = 0.0f, prev = -1.0f, lo = 1e9f, hi = 0.0f;
  for (int k = 0; k < 160; ++k) {
    const float p[3] = {14.0f + 12.0f * (float)k / 160.0f, 1.7f, 5.0f};  /* (7.5 cm a simulate, across behind it) */
    bau_source_set_position(s, p);
    bau_simulate(au);
    bau_heard h;
    bau_source_heard(s, &h);
    if (prev >= 0.0f) worst = fmaxf(worst, fabsf(h.gain - prev));
    prev = h.gain, lo = fminf(lo, h.gain), hi = fmaxf(hi, h.gain);
  }
  expect(20.0f * log10f(hi / lo) > 6.0f && worst < 0.15f * hi, "hidden and back, faded",
         SAY("%.1f dB in all, its biggest step %.0f%% of its loudness", 20.0f * log10f(hi / lo), 100.0f * worst / hi));
  bau_destroy(au);
}

/* A source inside its own vehicle's voxels: tied to that grid, not in its
 * own way. */
static void its_own_grid(void) {
  printf("its own grid\n");
  bau_context* au = context();
  world w = make(16, 6, 30, 0.5f, CONCRETE); /* a closed box: 8 x 3 x 15 m */
  box(&w, 1, 1, 1, 14, 4, 28, AIR);
  bau_grid* truck = add(au, &w);
  const float corner[3] = {-4, 0, -30};
  bau_grid_set_pose(truck, corner, 0.0f);
  listen_at(au, 0, 1.5f, 0);
  bau_source* engine = source_at(au, 0, 1.5f, -22.5f);  /* (in the box's middle, 7.5 m from its ends) */
  bau_simulate(au);
  bau_heard h;
  bau_source_heard(engine, &h);
  expect(h.occluded, "untied: shut in by the box", NULL);
  bau_source_set_grid(engine, truck);
  bau_simulate(au);
  bau_source_heard(engine, &h);
  expect(!h.occluded && h.muffle < 0.2f, "tied to it: clear", SAY("muffle %.2f", h.muffle));
  bau_grid_destroy(truck);
  bau_simulate(au);
  bau_source_heard(engine, &h);
  expect(!h.occluded, "its grid gone: untied, nothing in the way", NULL);
  bau_destroy(au);
}

int main(void) {
  open_field();
  wall_between();
  round_a_corner();
  room_and_field();
  cliff();
  movers_and_edits();
  layouts();
  rendering();
  deterministic();
  debug_paths();
  still_in_a_hall();
  driving_past();
  steady_round_a_corner();
  behind_a_pillar();
  its_own_grid();
  if (failures) printf("%d FAILED\n", failures);
  else printf("all passed\n");
  return failures ? 1 : 0;
}
