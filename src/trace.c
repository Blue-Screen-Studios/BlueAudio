/* The simulation: rays from the listener out every way (a Fibonacci sphere,
 * turned at random each time), bouncing off the voxels. At each bounce:
 *
 *   - each source it can see: a path the sound comes in by, heard from the
 *     way the ray first went out (round a corner: from the corner), its
 *     length the delay, each surface on the way soaking some up;
 *   - the listener, if it can see it: the echo (the sound coming back).
 *
 * Each source also straight to the listener, through whatever's between
 * (each solid cell letting its transmission through). Energies relative to
 * the source heard with nothing in the way (1/d^2): a diffuse bounce seen
 * by one ray of N, 4 cos(theta) d^2 / (N r^2) of it (r: the bounce to the
 * source; the radiance of a surface the same from any distance). */

#include <time.h>

#include "internal.h"

/* Air: intensity lost a metre, each band. */
const float bau__air[BAU__BANDS] = {0.00023f, 0.00115f, 0.0069f};

typedef struct bau__acc {
  bau_source* src;
  bau__v3 pos;
  float radius, reach, distance;
  int active, occluded, last_ray, rays;
  float direct[BAU__BANDS], reflected[BAU__BANDS];
  bau__v3 arrive;
  float path;  /* (summed: length * mid energy) */
} bau__acc;

static double bau__now_ms(void) {
  struct timespec ts;
  timespec_get(&ts, TIME_UTC);
  return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

/* A direction about `n`, cosine-weighted. */
static bau__v3 bau__diffuse(bau__v3 n, uint32_t* rng) {
  const bau__v3 t = bau__norm(fabsf(n.x) > 0.5f ? bau__cross(n, bau__v(0.0f, 1.0f, 0.0f)) : bau__cross(n, bau__v(1.0f, 0.0f, 0.0f)));
  const bau__v3 b = bau__cross(n, t);
  const float phi = 2.0f * BAU__PI * bau__rand(rng), r2 = bau__rand(rng), sr = sqrtf(r2);
  return bau__norm(bau__add(bau__add(bau__mul(t, cosf(phi) * sr), bau__mul(b, sinf(phi) * sr)), bau__mul(n, sqrtf(1.0f - r2))));
}

/* A rotation at random (a unit quaternion, uniformly: Shoemake's). */
static void bau__random_rotation(uint32_t* rng, float m[9]) {
  const float u1 = bau__rand(rng), u2 = bau__rand(rng) * 2.0f * BAU__PI, u3 = bau__rand(rng) * 2.0f * BAU__PI;
  const float a = sqrtf(1.0f - u1), b = sqrtf(u1);
  const float x = a * sinf(u2), y = a * cosf(u2), z = b * sinf(u3), w = b * cosf(u3);
  m[0] = 1 - 2 * (y * y + z * z), m[1] = 2 * (x * y - z * w), m[2] = 2 * (x * z + y * w);
  m[3] = 2 * (x * y + z * w), m[4] = 1 - 2 * (x * x + z * z), m[5] = 2 * (y * z - x * w);
  m[6] = 2 * (x * z - y * w), m[7] = 2 * (y * z + x * w), m[8] = 1 - 2 * (x * x + y * y);
}

/* A path that found a source, kept for the debug rays (each as likely:
 * a reservoir). */
static void bau__keep_ray(bau_ray* rays, int* n, long* seen, uint32_t* rng, const bau_source* src, const bau__v3* pts, int npts,
                          float gain) {
  int slot;
  ++*seen;
  if (*n < BAU__DEBUG_RAYS) slot = (*n)++;
  else {
    slot = (int)(bau__rand(rng) * (float)*seen);
    if (slot >= BAU__DEBUG_RAYS) return;
  }
  bau_ray* r = &rays[slot];
  r->source = src, r->points = npts, r->gain = gain;
  for (int i = 0; i < npts; ++i) bau__store(pts[i], r->at[i]);
}

/* The echo's numbers from its (blended) histogram and ray statistics. */
static void bau__echo_from(bau_context* au, float absorb, float escape, float free_path, float enclosure) {
  bau_echo* e = &au->echo;
  e->enclosure = enclosure, e->free_path = free_path;
  /* RT60 (Eyring's way, from the rays): each bounce keeps (1 - absorption)
   * of what hits and loses what escapes; a bounce every free path. */
  const float keep = (1.0f - absorb) * (1.0f - escape);
  e->decay = keep > 1e-4f && keep < 0.9999f ? bau__clamp(logf(1e-6f) / logf(keep) * free_path / BAU__SPEED, 0.0f, 10.0f) : 0.0f;
  /* What came back after the first 25 ms (the ground right below no echo). */
  float late = 0.0f;
  bau__v3 dir = bau__v(0.0f, 0.0f, 0.0f);
  const int first = (int)(0.025f / BAU__BIN);
  for (int i = first; i < BAU__BINS; ++i) late += au->hist[i], dir = bau__add(dir, bau__vp(au->hist_dir[i]));
  e->level = bau__clamp(1.0f - expf(-3.0f * late), 0.0f, 1.0f) * bau__clamp(enclosure * 1.5f, 0.0f, 1.0f);
  const float l = bau__len(dir);
  if (l > 1e-12f) bau__store(bau__mul(dir, 1.0f / l), e->direction);
  e->focus = late > 1e-12f ? bau__clamp(l / late, 0.0f, 1.0f) : 0.0f;
  /* Distinct echoes: peaks (past 60 ms) standing well out of what's around
   * them (a far wall, a cliff answering back), the strongest four. */
  e->taps = 0;
  const float floor_ = late * 0.01f;
  for (int i = (int)(0.06f / BAU__BIN); i < BAU__BINS - 1; ++i) {
    const float v = au->hist[i - 1] + au->hist[i] + au->hist[i + 1];
    if (au->hist[i] < au->hist[i - 1] || au->hist[i] < au->hist[i + 1] || v < floor_ || v < 1e-6f) continue;
    float around = 0.0f;
    int count = 0;
    for (int j = i - 16; j <= i + 16; ++j) {
      if (j < 0 || j >= BAU__BINS || (j >= i - 2 && j <= i + 2)) continue;
      around += au->hist[j], ++count;
    }
    around = count ? around / (float)count * 3.0f : 0.0f;
    if (v < 4.0f * around) continue;
    const float gain = bau__clamp(sqrtf(v) * 6.0f, 0.0f, 0.6f);
    int at = e->taps < 4 ? e->taps++ : -1;
    if (at < 0) {  /* (full: in place of the weakest, if stronger) */
      at = 0;
      for (int k = 1; k < 4; ++k)
        if (e->tap_gain[k] < e->tap_gain[at]) at = k;
      if (e->tap_gain[at] >= gain) continue;
    }
    e->tap_delay[at] = ((float)i + 0.5f) * BAU__BIN, e->tap_gain[at] = gain;
    const bau__v3 td = bau__norm(bau__vp(au->hist_dir[i]));
    bau__store(td, e->tap_direction[at]);
  }
}

void bau_simulate(bau_context* au) {
  if (!au) return;
  const double started = bau__now_ms();
  bau__lock(&au->world);
  bau__lock(&au->state);
  for (bau_grid* g = au->grids; g; g = g->next) bau__grid_apply_pose(g);
  const bau__v3 L = bau__vp(au->listener);
  const int N = au->cfg.rays, B = au->cfg.bounces;
  const float reach = au->cfg.max_distance, lr = au->cfg.listener_radius;
  int ns = 0;
  for (bau_source* s = au->sources; s; s = s->next) ++ns;
  bau__acc* acc = (bau__acc*)calloc(ns ? (size_t)ns : 1, sizeof *acc);
  if (!acc) {
    bau__unlock(&au->state);
    bau__unlock(&au->world);
    return;
  }
  {
    int i = 0;
    for (bau_source* s = au->sources; s; s = s->next, ++i) {
      acc[i].src = s, acc[i].pos = bau__vp(s->pos), acc[i].radius = s->radius;
      acc[i].reach = s->max < reach ? s->max : reach, acc[i].last_ray = -1;
    }
  }
  uint32_t rng = au->rng;
  bau__unlock(&au->state);

  /* Straight: each source to the listener, through what's between. */
  long shadow = 0;
  for (int j = 0; j < ns; ++j) {
    bau__acc* a = &acc[j];
    const bau__v3 to = bau__sub(a->pos, L);
    a->distance = bau__len(to);
    a->active = a->distance <= a->reach;
    if (!a->active) continue;
    float t[BAU__BANDS];
    const bau__v3 dir = a->distance > 1e-6f ? bau__mul(to, 1.0f / a->distance) : bau__v(0.0f, 0.0f, -1.0f);
    a->occluded = bau__transmit(au, L, dir, lr, a->distance - a->radius, t) > 0;
    ++shadow;
    for (int b = 0; b < BAU__BANDS; ++b) a->direct[b] = t[b] * expf(-bau__air[b] * a->distance);
  }

  /* The rays. */
  float rot[9];
  bau__random_rotation(&rng, rot);
  float hist[BAU__BINS] = {0}, hist_dir[BAU__BINS][3] = {{0}};
  bau_ray rays[BAU__DEBUG_RAYS];
  int nrays = 0;
  long seen = 0;
  double absorbed = 0.0, path = 0.0;
  long hits = 0, segments = 0, misses = 0, later = 0, later_misses = 0;
  const float golden = BAU__PI * (3.0f - sqrtf(5.0f));
  for (int k = 0; k < N; ++k) {
    const float y = 1.0f - 2.0f * ((float)k + 0.5f) / (float)N, r = sqrtf(1.0f - y * y), phi = golden * (float)k;
    const bau__v3 f = bau__v(cosf(phi) * r, y, sinf(phi) * r);
    const bau__v3 first = bau__v(rot[0] * f.x + rot[1] * f.y + rot[2] * f.z, rot[3] * f.x + rot[4] * f.y + rot[5] * f.z,
                                 rot[6] * f.x + rot[7] * f.y + rot[8] * f.z);
    bau__v3 o = L, dir = first, pts[BAU_RAY_POINTS];
    float w[BAU__BANDS] = {1.0f, 1.0f, 1.0f}, length = 0.0f, tmin = lr;
    pts[0] = L;
    for (int bounce = 0; bounce < B; ++bounce) {
      bau__hit hit;
      ++segments, later += bounce > 0;
      if (!bau__trace(au, o, dir, tmin, reach, &hit)) {
        ++misses, later_misses += bounce > 0;
        break;
      }
      const bau__v3 p = bau__add(o, bau__mul(dir, hit.t)), n = hit.normal;
      const bau__v3 off = bau__add(p, bau__mul(n, 0.01f));
      const bau_material* m = &au->materials[hit.material];
      length += hit.t;
      ++hits, absorbed += m->absorption[1], path += hit.t;
      pts[bounce + 1] = p;
      const float invN = 4.0f / (float)N;
      /* The sources this bounce can see. */
      for (int j = 0; j < ns; ++j) {
        bau__acc* a = &acc[j];
        if (!a->active) continue;
        const bau__v3 to = bau__sub(a->pos, off);
        const float rr = bau__len(to);
        if (rr < 1e-4f || length + rr > reach * 2.0f) continue;
        const bau__v3 u = bau__mul(to, 1.0f / rr);
        const float cosine = bau__dot(n, u);
        if (cosine <= 0.0f) continue;
        ++shadow;
        if (!bau__clear(au, off, u, 0.0f, rr - a->radius)) continue;
        const float r1 = rr < 1.0f ? 1.0f : rr, d2 = a->distance * a->distance;
        float base = invN * cosine * d2 / (r1 * r1);
        if (base > 1.0f) base = 1.0f; /* (a bounce right beside the source: no one ray's spike) */
        const float total = length + rr;
        float c[BAU__BANDS];
        for (int b = 0; b < BAU__BANDS; ++b) {
          c[b] = base * w[b] * (1.0f - m->absorption[b]) * expf(-bau__air[b] * total);
          a->reflected[b] += c[b];
        }
        a->arrive = bau__add(a->arrive, bau__mul(first, c[1]));
        a->path += c[1] * total;
        if (a->last_ray != k) a->last_ray = k, ++a->rays;
        pts[bounce + 2] = a->pos;
        bau__keep_ray(rays, &nrays, &seen, &rng, a->src, pts, bounce + 3, sqrtf(c[1]));
      }
      /* The listener: the echo. */
      {
        const bau__v3 to = bau__sub(L, off);
        const float rr = bau__len(to);
        const bau__v3 u = bau__mul(to, 1.0f / (rr > 1e-6f ? rr : 1.0f));
        const float cosine = bau__dot(n, u);
        if (cosine > 0.0f && rr > 1e-4f) {
          ++shadow;
          if (bau__clear(au, off, u, 0.0f, rr - lr)) {
            const float r1 = rr < 1.0f ? 1.0f : rr;
            const float e = invN * cosine / (r1 * r1) * w[1] * (1.0f - m->absorption[1]);
            const int bin = (int)((length + rr) / BAU__SPEED / BAU__BIN);
            if (bin < BAU__BINS) {
              hist[bin] += e;
              hist_dir[bin][0] += first.x * e, hist_dir[bin][1] += first.y * e, hist_dir[bin][2] += first.z * e;
            }
          }
        }
      }
      /* On: some soaked up; reflected (a mirror's way, some scattered). */
      for (int b = 0; b < BAU__BANDS; ++b) w[b] *= 1.0f - m->absorption[b];
      if (w[0] + w[1] + w[2] < 3e-3f) break;
      const bau__v3 spec = bau__sub(dir, bau__mul(n, 2.0f * bau__dot(dir, n)));
      dir = bau__norm(bau__add(bau__mul(spec, 1.0f - m->scattering), bau__mul(bau__diffuse(n, &rng), m->scattering)));
      if (bau__dot(dir, n) <= 0.0f) dir = bau__diffuse(n, &rng);
      o = off, tmin = 0.0f;
    }
  }

  /* Published: blended with what was. */
  const float mean_absorb = hits ? (float)(absorbed / (double)hits) : 1.0f;
  const float escape = segments ? (float)misses / (float)segments : 1.0f;
  const float free_path = hits ? (float)(path / (double)hits) : 0.0f;
  const float enclosure = later ? 1.0f - (float)later_misses / (float)later : 0.0f;
  bau__lock(&au->state);
  for (int j = 0; j < ns; ++j) {
    bau__acc* a = &acc[j];
    bau_source* s = au->sources;
    while (s && s != a->src) s = s->next;
    if (!s) continue; /* (destroyed meanwhile) */
    const float blend = s->simulated ? BAU__BLEND : 1.0f;
    s->distance = a->distance, s->occluded = a->active ? a->occluded : 1;
    s->reached = a->rays;
    s->reach = bau__lerp(s->reach, (float)a->rays / (float)N, blend);
    float mid_before = s->reflected[1];
    for (int b = 0; b < BAU__BANDS; ++b) {
      s->direct[b] = a->active ? a->direct[b] : 0.0f;
      s->reflected[b] = bau__lerp(s->reflected[b], a->active ? a->reflected[b] : 0.0f, blend);
    }
    for (int i = 0; i < 3; ++i) {
      const float v = i == 0 ? a->arrive.x : i == 1 ? a->arrive.y : a->arrive.z;
      s->arrive[i] = bau__lerp(s->arrive[i], v, blend);
    }
    if (a->reflected[1] > 1e-12f) {
      const float mean_path = a->path / a->reflected[1];
      s->path = mid_before > 1e-12f ? bau__lerp(s->path, mean_path, blend) : mean_path;
    }
    s->simulated = 1;
  }
  const float blend = au->echo_set ? BAU__BLEND : 1.0f;
  for (int i = 0; i < BAU__BINS; ++i) {
    au->hist[i] = bau__lerp(au->hist[i], hist[i], blend);
    for (int c = 0; c < 3; ++c) au->hist_dir[i][c] = bau__lerp(au->hist_dir[i][c], hist_dir[i][c], blend);
  }
  /* (The ray statistics blended too: kept in the echo's own fields.) */
  if (!au->echo_set) au->echo.free_path = free_path, au->echo.enclosure = enclosure;
  au->stat_absorb = au->echo_set ? bau__lerp(au->stat_absorb, mean_absorb, blend) : mean_absorb;
  au->stat_escape = au->echo_set ? bau__lerp(au->stat_escape, escape, blend) : escape;
  bau__echo_from(au, au->stat_absorb, au->stat_escape, bau__lerp(au->echo.free_path, free_path, blend),
                 bau__lerp(au->echo.enclosure, enclosure, blend));
  au->echo_set = 1;
  memcpy(au->rays, rays, (size_t)nrays * sizeof *rays);
  au->nrays = nrays;
  au->rng = rng;
  au->stats.shadow_rays = shadow;
  au->stats.simulate_ms = bau__now_ms() - started;
  bau__unlock(&au->state);
  bau__unlock(&au->world);
  free(acc);
}
