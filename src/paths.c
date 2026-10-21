/* The debug paths: a fan of rays from the listener, each bounced a
 * mirror's way through the world, its energy in three bands at each point;
 * branches where a bounce sees a source (FOUND) or the listener (ECHO). For
 * drawing, slowed down: not the simulation's rays, the same sums. */

#include "internal.h"

typedef struct bau__target {
  const bau_source* src;
  bau__v3 pos;
  float radius;
} bau__target;

/* Nothing between a and b (the last `skip` m of it ignored: the source's
 * own size, the listener's). */
static int bau__sees(const bau_context* au, bau__v3 a, bau__v3 b, float skip) {
  const bau__v3 d = bau__sub(b, a);
  const float len = bau__len(d);
  if (len < 0.5f) return 1;
  return bau__clear(au, a, bau__mul(d, 1.0f / len), 0.0f, len - skip);
}

/* A branch off path `parent` at its point `k`, straight to `to`. */
static void bau__branch(bau_path* out, int* n, int max, bau_path_kind kind, int parent, int k, bau__v3 from, bau__v3 to,
                        const bau_source* src) {
  if (*n >= max) return;
  const bau_path* p = &out[parent];
  bau_path* b = &out[(*n)++];
  memset(b, 0, sizeof *b);
  b->kind = kind, b->parent = parent, b->start = p->distance[k], b->source = src, b->points = 2;
  bau__store(from, b->at[0]), bau__store(to, b->at[1]);
  b->distance[1] = bau__len(bau__sub(to, from));
  memcpy(b->energy[0], p->energy[k], sizeof b->energy[0]);
  memcpy(b->energy[1], p->energy[k], sizeof b->energy[1]);
}

int bau_debug_paths(bau_context* au, const bau_path_config* config, bau_path* out, int max) {
  if (!au || !out || max <= 0) return 0;
  bau_path_config c;
  memset(&c, 0, sizeof c);
  if (config) c = *config;
  if (c.rays <= 0) c.rays = 40;
  if (c.bounces <= 0) c.bounces = 5;
  if (c.bounces > BAU_PATH_POINTS - 1) c.bounces = BAU_PATH_POINTS - 1;
  if (!(c.reach > 0.0f)) c.reach = 40.0f;
  if (!(c.min_energy > 0.0f)) c.min_energy = 0.03f;

  bau__lock(&au->world);
  bau__lock(&au->state);
  const bau__v3 L = bau__vp(au->listener);
  const float lr = au->cfg.listener_radius;
  int ns = 0;
  for (const bau_source* s = au->sources; s; s = s->next) ++ns;
  bau__target* targets = (bau__target*)malloc((size_t)(ns ? ns : 1) * sizeof *targets);
  if (!targets) {
    bau__unlock(&au->state);
    bau__unlock(&au->world);
    return 0;
  }
  {
    int i = 0;
    for (const bau_source* s = au->sources; s; s = s->next, ++i) {
      targets[i].src = s, targets[i].pos = bau__vp(s->pos), targets[i].radius = s->radius > 0.4f ? s->radius : 0.4f;
      if (c.flat) targets[i].pos.y = L.y;  /* (seen at the listener's height) */
    }
  }
  bau__unlock(&au->state);

  int n = 0;
  const float golden = BAU__PI * (3.0f - sqrtf(5.0f)), ct = cosf(c.turn), st = sinf(c.turn);
  for (int r = 0; r < c.rays && n < max; ++r) {
    bau__v3 d;
    if (c.flat) {
      const float a = c.turn + (float)r / (float)c.rays * 2.0f * BAU__PI;
      d = bau__v(cosf(a), 0.0f, sinf(a));
    } else {
      const float y = 1.0f - 2.0f * ((float)r + 0.5f) / (float)c.rays, rr = sqrtf(1.0f - y * y), phi = golden * (float)r;
      const float x = cosf(phi) * rr, z = sinf(phi) * rr;
      d = bau__v(ct * x - st * z, y, st * x + ct * z);
    }
    const int pi = n++;
    bau_path* p = &out[pi];
    memset(p, 0, sizeof *p);
    p->kind = BAU_PATH_OUT, p->parent = -1, p->points = 1;
    bau__store(L, p->at[0]);
    float e[BAU__BANDS] = {1.0f, 1.0f, 1.0f}, along = 0.0f, tmin = lr;
    memcpy(p->energy[0], e, sizeof e);
    bau__v3 o = L;
    while (p->points < c.bounces + 1) {
      bau__hit h;
      const int hit = bau__trace(au, o, d, tmin, c.reach, &h);
      const float len = hit ? h.t : c.reach;
      along += len;
      for (int b = 0; b < BAU__BANDS; ++b) e[b] *= expf(-bau__air[b] * len);
      const int k = p->points++;
      const bau__v3 at = bau__add(o, bau__mul(d, len));
      bau__store(at, p->at[k]);
      p->distance[k] = along;
      if (!hit) {
        memcpy(p->energy[k], e, sizeof e);
        p->open_end = 1;
        break;
      }
      const bau_material* m = &au->materials[h.material];
      for (int b = 0; b < BAU__BANDS; ++b) e[b] *= 1.0f - m->absorption[b];
      memcpy(p->energy[k], e, sizeof e);
      bau__v3 off = bau__add(at, bau__mul(h.normal, 0.01f));
      if (c.flat) off.y = L.y;
      /* What this bounce sees: the sources, the listener (branches,
       * written after this path in `out`). */
      for (int s = 0; s < ns; ++s)
        if (bau__sees(au, off, targets[s].pos, targets[s].radius))
          bau__branch(out, &n, max, BAU_PATH_FOUND, pi, k, off, targets[s].pos, targets[s].src);
      if (bau__sees(au, off, L, lr > 0.4f ? lr : 0.4f)) bau__branch(out, &n, max, BAU_PATH_ECHO, pi, k, off, L, NULL);
      /* On: a mirror's way (kept level, flat). */
      d = bau__sub(d, bau__mul(h.normal, 2.0f * bau__dot(d, h.normal)));
      if (c.flat) d.y = 0.0f;
      d = bau__norm(d);
      o = off, tmin = 0.0f;
      if (e[0] + e[1] + e[2] < c.min_energy || bau__len(d) < 0.5f) break;
    }
  }
  bau__unlock(&au->world);
  free(targets);
  return n;
}

void bau_path_sample(const bau_path* p, float s, float pos[3], float energy[3]) {
  if (!p || p->points <= 0) return;
  const int last = p->points - 1;
  int k = 0;
  if (s <= 0.0f || last == 0) k = 0, s = 0.0f;
  else if (s >= p->distance[last]) k = last - 1, s = p->distance[last];
  else
    while (k + 1 < last && p->distance[k + 1] < s) ++k;
  const int k1 = last == 0 ? 0 : k + 1;
  const float span = p->distance[k1] - p->distance[k], t = span > 1e-6f ? (s - p->distance[k]) / span : 0.0f;
  for (int i = 0; i < 3; ++i) {
    if (pos) pos[i] = bau__lerp(p->at[k][i], p->at[k1][i], t);
    if (energy) energy[i] = bau__lerp(p->energy[k][i], p->energy[k1][i], t);
  }
}

void bau_path_color(bau_path_kind kind, const float e[3], float rgba[4]) {
  if (!e || !rgba) return;
  const float total = (e[0] + e[1] + e[2]) / 3.0f, bright = sqrtf(total < 1.0f ? (total > 0.0f ? total : 0.0f) : 1.0f);
  const float hi = bau__clamp(e[0] > 1e-5f ? e[2] / e[0] : 0.0f, 0.0f, 1.0f);
  float c[3] = {bau__lerp(1.0f, 0.62f, hi), bau__lerp(0.42f, 0.92f, hi), bau__lerp(0.12f, 1.0f, hi)};
  static const float gold[3] = {1.0f, 0.82f, 0.25f}, violet[3] = {0.78f, 0.5f, 1.0f};
  const float* tint = kind == BAU_PATH_FOUND ? gold : kind == BAU_PATH_ECHO ? violet : NULL;
  for (int k = 0; k < 3; ++k) rgba[k] = tint ? c[k] * 0.35f + tint[k] * 0.65f : c[k];
  rgba[3] = 0.12f + 0.88f * bright;
}
