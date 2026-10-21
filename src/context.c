/* The context: its config, the listener, the sources; what the simulation
 * worked out, read back. */

#include <stdio.h>

#include "internal.h"

static bau_config bau__filled(const bau_config* c) {
  bau_config f;
  memset(&f, 0, sizeof f);
  if (c) f = *c;
  if (f.sample_rate <= 0) f.sample_rate = 48000;
  if ((int)f.layout < 0 || f.layout > BAU_LAYOUT_SURROUND_71) f.layout = BAU_LAYOUT_STEREO;
  if (f.rays <= 0) f.rays = 1024;
  if (f.bounces <= 0) f.bounces = 4;
  if (f.bounces > BAU_RAY_POINTS - 2) f.bounces = BAU_RAY_POINTS - 2;
  if (!(f.max_distance > 0.0f)) f.max_distance = 90.0f;
  if (!(f.listener_radius > 0.0f)) f.listener_radius = 0.25f;
  if (!f.seed) f.seed = 1;
  return f;
}

static int bau__channels_of(bau_layout layout) {
  switch (layout) {
    case BAU_LAYOUT_MONO: return 1;
    case BAU_LAYOUT_QUAD: return 4;
    case BAU_LAYOUT_SURROUND_51: return 6;
    case BAU_LAYOUT_SURROUND_71: return 8;
    default: return 2;
  }
}

bau_context* bau_create(const bau_config* config) {
  bau_context* au = (bau_context*)calloc(1, sizeof *au);
  if (!au) return NULL;
  au->cfg = bau__filled(config);
  au->channels = bau__channels_of(au->cfg.layout);
  au->rng = au->cfg.seed;
  bau__mutex_init(&au->world);
  bau__mutex_init(&au->render);
  bau__mutex_init(&au->state);
  for (int i = 1; i < 256; ++i) au->materials[i] = bau_material_preset(BAU_MATERIAL_GENERIC);
  au->listener[5] = -1.0f; /* (ahead: -z) */
  au->listener[7] = 1.0f;  /* (up: +y) */
  au->reverb = bau__reverb_create(au->cfg.sample_rate);
  if (!au->reverb) {
    bau_destroy(au);
    return NULL;
  }
  return au;
}

void bau_destroy(bau_context* au) {
  if (!au) return;
  while (au->sources) bau_source_destroy(au->sources);
  while (au->grids) bau_grid_destroy(au->grids);
  bau__reverb_destroy(au->reverb);
  bau__mutex_free(&au->world);
  bau__mutex_free(&au->render);
  bau__mutex_free(&au->state);
  free(au);
}

void bau_get_config(const bau_context* au, bau_config* out) {
  if (au && out) *out = au->cfg;
}

int bau_channels(const bau_context* au) { return au ? au->channels : 0; }

void bau_set_layout(bau_context* au, bau_layout layout) {
  if (!au) return;
  if ((int)layout < 0 || layout > BAU_LAYOUT_SURROUND_71) layout = BAU_LAYOUT_STEREO;
  bau__lock(&au->render);
  au->cfg.layout = layout, au->channels = bau__channels_of(layout);
  for (bau_source* s = au->sources; s; s = s->next) memset(s->gains, 0, sizeof s->gains);
  bau__unlock(&au->render);
}

void bau_set_rays(bau_context* au, int rays, int bounces) {
  if (!au) return;
  bau__lock(&au->state);
  if (rays > 0) au->cfg.rays = rays;
  if (bounces > 0) au->cfg.bounces = bounces > BAU_RAY_POINTS - 2 ? BAU_RAY_POINTS - 2 : bounces;
  bau__unlock(&au->state);
}

void bau_set_listener(bau_context* au, const float pos[3], const float ahead[3], const float up[3]) {
  if (!au || !pos || !ahead || !up) return;
  const bau__v3 a = bau__norm(bau__vp(ahead));
  /* (up made square to ahead) */
  const bau__v3 u = bau__norm(bau__sub(bau__vp(up), bau__mul(a, bau__dot(bau__vp(up), a))));
  bau__lock(&au->state);
  memcpy(au->listener, pos, 3 * sizeof(float));
  if (bau__len(a) > 0.5f && bau__len(u) > 0.5f) bau__store(a, au->listener + 3), bau__store(u, au->listener + 6);
  bau__unlock(&au->state);
}

/* ----------------------------------------------------------- sources */

bau_source* bau_source_create(bau_context* au, bau_fill_fn fill, void* user) {
  if (!au || !fill) return NULL;
  bau_source* s = (bau_source*)calloc(1, sizeof *s);
  if (!s) return NULL;
  s->au = au, s->fill = fill, s->user = user;
  s->gain = 1.0f, s->ref = 7.0f, s->max = au->cfg.max_distance, s->radius = 0.5f;
  for (int b = 0; b < BAU__BANDS; ++b) s->through[b] = 1.0f;  /* (not traced yet: as if clear) */
  if (!bau__source_init_render(s)) {
    free(s);
    return NULL;
  }
  bau__lock(&au->render);
  bau__lock(&au->state);
  s->next = au->sources, au->sources = s;
  bau__unlock(&au->state);
  bau__unlock(&au->render);
  return s;
}

void bau_source_set_position(bau_source* s, const float pos[3]) {
  if (!s || !pos) return;
  bau__lock(&s->au->state);
  memcpy(s->pos, pos, sizeof s->pos);
  bau__unlock(&s->au->state);
}

void bau_source_set_gain(bau_source* s, float gain) {
  if (!s) return;
  bau__lock(&s->au->state);
  s->gain = gain < 0.0f ? 0.0f : gain;
  bau__unlock(&s->au->state);
}

void bau_source_set_rolloff(bau_source* s, float ref, float max) {
  if (!s) return;
  bau__lock(&s->au->state);
  if (ref > 0.0f) s->ref = ref;
  if (max > 0.0f) s->max = max;
  bau__unlock(&s->au->state);
}

void bau_source_set_radius(bau_source* s, float radius) {
  if (!s) return;
  bau__lock(&s->au->state);
  s->radius = radius < 0.0f ? 0.0f : radius;
  bau__unlock(&s->au->state);
}

void bau_source_set_grid(bau_source* s, const bau_grid* grid) {
  if (!s) return;
  bau__lock(&s->au->state);
  s->grid = grid;
  bau__unlock(&s->au->state);
}

void* bau_source_user(const bau_source* s) { return s ? s->user : NULL; }

int bau_source_finished(const bau_source* s) {
  if (!s) return 1;
  bau__lock(&s->au->render);
  const int done = s->ended && s->tail <= 0;
  bau__unlock(&s->au->render);
  return done;
}

void bau_source_destroy(bau_source* s) {
  if (!s) return;
  bau_context* au = s->au;
  bau__lock(&au->render);
  bau__lock(&au->state);
  for (bau_source** at = &au->sources; *at; at = &(*at)->next)
    if (*at == s) {
      *at = s->next;
      break;
    }
  /* (The debug rays that found it: theirs no more.) */
  for (int i = 0; i < au->nrays; ++i)
    if (au->rays[i].source == s) au->rays[i] = au->rays[--au->nrays], --i;
  bau__unlock(&au->state);
  bau__unlock(&au->render);
  bau__source_free_render(s);
  free(s);
}

/* ----------------------------------------------------------- read back */

/* The gain a source is heard at: the intensity that reached it (relative to
 * nothing in the way), its own rolloff. */
static float bau__rolloff(const bau_source* s, float d) {
  const float r = d / s->ref;
  float g = d <= s->ref ? 1.0f : 1.0f / (1.0f + (r - 1.0f) * (r + 1.0f));
  const float fade = s->max * 0.8f;
  if (d > fade) g *= bau__clamp(1.0f - (d - fade) / (s->max - fade), 0.0f, 1.0f);
  return g;
}

void bau_source_heard(const bau_source* s, bau_heard* out) {
  if (!s || !out) return;
  bau__lock(&s->au->state);
  bau__heard(s, out);
  bau__unlock(&s->au->state);
}

void bau__heard(const bau_source* s, bau_heard* out) {
  const bau_context* au = s->au;
  memset(out, 0, sizeof *out);
  out->simulated = s->simulated, out->occluded = s->occluded, out->reach = s->reach;
  /* Where it is, where the listener is: now. */
  const bau__v3 to = bau__sub(bau__vp(s->pos), bau__vp(au->listener));
  const float d = bau__len(to);
  const bau__v3 way = d > 1e-6f ? bau__mul(to, 1.0f / d) : bau__v(0.0f, 0.0f, -1.0f);
  out->distance = d;
  float direct[BAU__BANDS];
  for (int b = 0; b < BAU__BANDS; ++b) direct[b] = s->through[b] * expf(-bau__air[b] * d);
  const float roll = d > s->max || d > au->cfg.max_distance ? 0.0f : bau__rolloff(s, d) * s->gain;
  out->direct = sqrtf(direct[1]) * roll;
  out->reflected = sqrtf(s->reflected[1]) * roll;
  const float mid = direct[1] + s->reflected[1], high = direct[2] + s->reflected[2];
  out->gain = sqrtf(mid > 4.0f ? 4.0f : mid) * roll;
  out->muffle = 1.0f - bau__clamp(sqrtf(high), 0.0f, 1.0f);
  /* Heard from: straight at it (the direct's share), the way the rays went
   * out that found it (the reflections'). */
  const bau__v3 dir = bau__add(bau__mul(way, direct[1]), bau__vp(s->arrive));
  const float l = bau__len(dir);
  if (mid > 1e-12f && l > 1e-12f) {
    bau__store(bau__mul(dir, 1.0f / l), out->direction);
    out->spread = bau__clamp(1.0f - l / mid, 0.0f, 1.0f);
  } else {
    bau__store(way, out->direction);
  }
  /* When it first arrives: straight, once enough comes straight through;
   * all but nothing straight (hidden), the shortest way round; between,
   * between (no jump as it goes behind something). */
  const float straight = direct[1] / (direct[1] + s->reflected[1] + 1e-12f);
  const float round = s->reflected[1] > 1e-9f ? 1.0f - bau__clamp((straight - 0.05f) / 0.2f, 0.0f, 1.0f) : 0.0f;
  out->delay = (d + s->detour * round) / BAU__SPEED;
}

void bau_listener_echo(const bau_context* au, bau_echo* out) {
  if (!au || !out) return;
  bau__lock((bau__mutex*)&au->state);
  *out = au->echo;
  bau__unlock((bau__mutex*)&au->state);
}

int bau_debug_rays(const bau_context* au, bau_ray* out, int max) {
  if (!au || !out || max <= 0) return 0;
  bau__lock((bau__mutex*)&au->state);
  const int n = au->nrays < max ? au->nrays : max;
  memcpy(out, au->rays, (size_t)n * sizeof *out);
  bau__unlock((bau__mutex*)&au->state);
  return n;
}

void bau_get_stats(const bau_context* au, bau_stats* out) {
  if (!au || !out) return;
  bau__lock((bau__mutex*)&au->world);
  bau__lock((bau__mutex*)&au->state);
  *out = au->stats;
  out->grids = 0, out->bricks = 0, out->sources = 0;
  for (const bau_grid* g = au->grids; g; g = g->next) {
    ++out->grids;
    const size_t n = (size_t)g->bx * g->by * g->bz;
    for (size_t i = 0; i < n; ++i) out->bricks += g->bricks[i] != 0;
  }
  for (const bau_source* s = au->sources; s; s = s->next) ++out->sources;
  out->rays = au->cfg.rays, out->bounces = au->cfg.bounces;
  bau__unlock((bau__mutex*)&au->state);
  bau__unlock((bau__mutex*)&au->world);
}
