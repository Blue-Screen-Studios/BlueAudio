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
  if (failures) printf("%d FAILED\n", failures);
  else printf("all passed\n");
  return failures ? 1 : 0;
}
