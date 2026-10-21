/* The world: materials, voxel grids (sparse: 4^3 bricks, the empty ones
 * nothing), posed (turned about y, moved); rays walked through them a cell
 * at a time (a DDA), the empty bricks jumped. */

#include "internal.h"

/* ----------------------------------------------------------- materials */

/* Absorption (low, mid, high), scattering, transmission (low, mid, high): the
 * usual figures for each (transmission: a 20 cm cell of it). */
static const bau_material bau__presets[BAU_MATERIAL_KINDS] = {
  {{0.10f, 0.20f, 0.30f}, 0.30f, {0.100f, 0.050f, 0.030f}}, /* generic */
  {{0.05f, 0.07f, 0.08f}, 0.10f, {0.015f, 0.002f, 0.001f}}, /* concrete */
  {{0.03f, 0.04f, 0.07f}, 0.20f, {0.015f, 0.015f, 0.015f}}, /* brick */
  {{0.20f, 0.07f, 0.06f}, 0.05f, {0.200f, 0.025f, 0.010f}}, /* metal */
  {{0.06f, 0.03f, 0.02f}, 0.05f, {0.060f, 0.044f, 0.011f}}, /* glass */
  {{0.11f, 0.07f, 0.06f}, 0.15f, {0.070f, 0.014f, 0.005f}}, /* wood */
  {{0.60f, 0.70f, 0.80f}, 0.50f, {0.031f, 0.012f, 0.008f}}, /* ground */
  {{0.40f, 0.55f, 0.70f}, 0.70f, {0.400f, 0.250f, 0.150f}}, /* foliage */
  {{0.24f, 0.69f, 0.73f}, 0.30f, {0.020f, 0.005f, 0.003f}}, /* carpet */
  {{0.85f, 0.85f, 0.85f}, 0.30f, {0.900f, 0.850f, 0.800f}}, /* mesh */
  {{0.01f, 0.02f, 0.03f}, 0.05f, {0.010f, 0.005f, 0.002f}}, /* water */
};

bau_material bau_material_preset(bau_material_kind kind) {
  if ((int)kind < 0 || kind >= BAU_MATERIAL_KINDS) kind = BAU_MATERIAL_GENERIC;
  return bau__presets[kind];
}

void bau_set_material(bau_context* au, uint8_t index, bau_material m) {
  if (!au || !index) return;
  for (int b = 0; b < BAU__BANDS; ++b) {
    m.absorption[b] = bau__clamp(m.absorption[b], 0.0f, 1.0f);
    m.transmission[b] = bau__clamp(m.transmission[b], 0.0f, 1.0f);
  }
  m.scattering = bau__clamp(m.scattering, 0.0f, 1.0f);
  bau__lock(&au->world);
  au->materials[index] = m;
  bau__unlock(&au->world);
}

/* ----------------------------------------------------------- grids */

static uint8_t bau__cell(const bau_grid* g, int x, int y, int z) {
  const uint32_t b = g->bricks[(x >> 2) + g->bx * ((y >> 2) + g->by * (z >> 2))];
  if (!b) return 0;
  return g->pool[(size_t)(b - 1) * 64 + (x & 3) + 4 * ((y & 3) + 4 * (z & 3))];
}

/* A cell set (its brick given a block if it needs one); 0 if out of memory. */
static int bau__set(bau_grid* g, int x, int y, int z, uint8_t m) {
  uint32_t* b = &g->bricks[(x >> 2) + g->bx * ((y >> 2) + g->by * (z >> 2))];
  if (!*b) {
    if (!m) return 1;
    if (g->blocks == g->cap) {
      const size_t cap = g->cap ? g->cap * 2 : 64;
      uint8_t* pool = (uint8_t*)realloc(g->pool, cap * 64);
      if (!pool) return 0;
      g->pool = pool, g->cap = cap;
    }
    memset(g->pool + g->blocks * 64, 0, 64);
    *b = (uint32_t)++g->blocks;
  }
  g->pool[(size_t)(*b - 1) * 64 + (x & 3) + 4 * ((y & 3) + 4 * (z & 3))] = m;
  return 1;
}

/* Bricks left all air (after an update) marked empty again (their blocks
 * left where they are: reused by nothing, freed with the grid). */
static void bau__prune(bau_grid* g, int bx0, int by0, int bz0, int bx1, int by1, int bz1) {
  for (int bz = bz0; bz <= bz1; ++bz)
    for (int by = by0; by <= by1; ++by)
      for (int bx = bx0; bx <= bx1; ++bx) {
        uint32_t* b = &g->bricks[bx + g->bx * (by + g->by * bz)];
        if (!*b) continue;
        const uint8_t* block = g->pool + (size_t)(*b - 1) * 64;
        int any = 0;
        for (int i = 0; i < 64 && !any; ++i) any = block[i] != 0;
        if (!any) *b = 0;
      }
}

void bau__grid_apply_pose(bau_grid* g) {
  if (!g->moved) return;
  memcpy(g->origin, g->next_origin, sizeof g->origin);
  g->yaw = g->next_yaw, g->cy = cosf(g->yaw), g->sy = sinf(g->yaw);
  g->moved = 0;
}

bau_grid* bau_grid_create(bau_context* au, int nx, int ny, int nz, float cell, const uint8_t* cells) {
  if (!au || nx <= 0 || ny <= 0 || nz <= 0 || !(cell > 0.0f)) return NULL;
  bau_grid* g = (bau_grid*)calloc(1, sizeof *g);
  if (!g) return NULL;
  g->au = au, g->nx = nx, g->ny = ny, g->nz = nz, g->cell = cell;
  g->bx = (nx + 3) / 4, g->by = (ny + 3) / 4, g->bz = (nz + 3) / 4;
  g->bricks = (uint32_t*)calloc((size_t)g->bx * g->by * g->bz, sizeof(uint32_t));
  if (!g->bricks) {
    free(g);
    return NULL;
  }
  g->cy = 1.0f, g->sy = 0.0f;
  if (cells) {
    for (int z = 0; z < nz; ++z)
      for (int y = 0; y < ny; ++y)
        for (int x = 0; x < nx; ++x) {
          const uint8_t m = cells[(size_t)x + (size_t)nx * ((size_t)y + (size_t)ny * z)];
          if (m && !bau__set(g, x, y, z, m)) {
            free(g->pool), free(g->bricks), free(g);
            return NULL;
          }
        }
  }
  bau__lock(&au->world);
  g->next = au->grids, au->grids = g;
  bau__unlock(&au->world);
  return g;
}

void bau_grid_update(bau_grid* g, int x0, int y0, int z0, int sx, int sy, int sz, const uint8_t* cells) {
  if (!g || !cells || sx <= 0 || sy <= 0 || sz <= 0) return;
  bau__lock(&g->au->world);
  for (int z = 0; z < sz; ++z)
    for (int y = 0; y < sy; ++y)
      for (int x = 0; x < sx; ++x) {
        const int gx = x0 + x, gy = y0 + y, gz = z0 + z;
        if (gx < 0 || gy < 0 || gz < 0 || gx >= g->nx || gy >= g->ny || gz >= g->nz) continue;
        bau__set(g, gx, gy, gz, cells[(size_t)x + (size_t)sx * ((size_t)y + (size_t)sy * z)]);
      }
  const int bx0 = (x0 < 0 ? 0 : x0) >> 2, by0 = (y0 < 0 ? 0 : y0) >> 2, bz0 = (z0 < 0 ? 0 : z0) >> 2;
  const int bx1 = ((x0 + sx - 1 >= g->nx ? g->nx - 1 : x0 + sx - 1) >> 2);
  const int by1 = ((y0 + sy - 1 >= g->ny ? g->ny - 1 : y0 + sy - 1) >> 2);
  const int bz1 = ((z0 + sz - 1 >= g->nz ? g->nz - 1 : z0 + sz - 1) >> 2);
  bau__prune(g, bx0, by0, bz0, bx1, by1, bz1);
  bau__unlock(&g->au->world);
}

void bau_grid_set_pose(bau_grid* g, const float origin[3], float yaw) {
  if (!g || !origin) return;
  bau__lock(&g->au->state);
  memcpy(g->next_origin, origin, sizeof g->next_origin);
  g->next_yaw = yaw, g->moved = 1;
  bau__unlock(&g->au->state);
}

void bau_grid_destroy(bau_grid* g) {
  if (!g) return;
  bau_context* au = g->au;
  bau__lock(&au->world);
  for (bau_grid** at = &au->grids; *at; at = &(*at)->next)
    if (*at == g) {
      *at = g->next;
      break;
    }
  bau__unlock(&au->world);
  free(g->pool), free(g->bricks), free(g);
}

/* ----------------------------------------------------------- walking a ray */

enum { BAU__FIRST, BAU__ANY, BAU__THROUGH };

/* A ray through one grid (world o, d; t in m), as `mode` asks:
 *   FIRST    the nearest solid entered (hit), 1;
 *   ANY      any solid entered: 1;
 *   THROUGH  every solid cell crossed, its transmission into `through`, counted.
 * (A solid the ray starts inside isn't "entered": a listener, a source in a
 * wall walks out of it.) */
static int bau__walk(const bau_context* au, const bau_grid* g, bau__v3 ow, bau__v3 dw, float tmin, float tmax, int mode,
                     bau__hit* hit, float* through) {
  const float c = g->cy, s = g->sy, inv = 1.0f / g->cell;
  const bau__v3 q = bau__sub(ow, bau__vp(g->origin));
  const float o[3] = {(c * q.x - s * q.z) * inv, q.y * inv, (s * q.x + c * q.z) * inv};
  const float d[3] = {(c * dw.x - s * dw.z) * inv, dw.y * inv, (s * dw.x + c * dw.z) * inv};
  const int n[3] = {g->nx, g->ny, g->nz};
  /* Clipped to the grid's box. */
  float t0 = tmin, t1 = tmax;
  int axis = -1;
  for (int a = 0; a < 3; ++a) {
    if (fabsf(d[a]) < 1e-12f) {
      if (o[a] < 0.0f || o[a] > (float)n[a]) return 0;
      continue;
    }
    float ta = (0.0f - o[a]) / d[a], tb = ((float)n[a] - o[a]) / d[a];
    if (ta > tb) {
      const float tt = ta;
      ta = tb, tb = tt;
    }
    if (ta > t0) t0 = ta, axis = a;
    if (tb < t1) t1 = tb;
    if (t0 > t1) return 0;
  }
  int entered = axis >= 0; /* (the box entered past tmin: its first cell entered too) */
  int step[3], i[3], count = 0;
  float next[3], delta[3];
  for (int a = 0; a < 3; ++a) step[a] = d[a] > 0.0f ? 1 : -1, delta[a] = d[a] != 0.0f ? fabsf(1.0f / d[a]) : 1e30f;
  float t = t0;
  int fresh = 1; /* (the cells, next crossings to work out from t) */
  while (t < t1) {
    if (fresh) {
      const float te = t + 1e-4f * g->cell;
      for (int a = 0; a < 3; ++a) {
        int v = (int)floorf(o[a] + d[a] * te);
        i[a] = v < 0 ? 0 : v >= n[a] ? n[a] - 1 : v;
        next[a] = d[a] != 0.0f ? ((float)(i[a] + (step[a] > 0)) - o[a]) / d[a] : 1e30f;
      }
      fresh = 0;
    }
    if (!g->bricks[(i[0] >> 2) + g->bx * ((i[1] >> 2) + g->by * (i[2] >> 2))]) {
      /* An empty brick: on to where the ray leaves it. */
      float exit = 1e30f;
      int ea = 0;
      for (int a = 0; a < 3; ++a) {
        if (d[a] == 0.0f) continue;
        const float edge = (float)(((i[a] >> 2) + (step[a] > 0)) * BAU__BRICK);
        const float te = (edge - o[a]) / d[a];
        if (te < exit) exit = te, ea = a;
      }
      if (exit <= t) exit = t + 1e-4f;
      t = exit, axis = ea, entered = 1, fresh = 1;
      /* (Left the grid that way: done.) */
      const int ni = (i[ea] >> 2) * BAU__BRICK + (step[ea] > 0 ? BAU__BRICK : -1);
      if (ni < 0 || ni >= n[ea]) break;
      continue;
    }
    const uint8_t m = bau__cell(g, i[0], i[1], i[2]);
    if (m) {
      if (mode == BAU__THROUGH) {
        const bau_material* mat = &au->materials[m];
        for (int b = 0; b < BAU__BANDS; ++b) through[b] *= mat->transmission[b];
        ++count;
      } else if (entered) {
        if (mode == BAU__ANY) return 1;
        hit->t = t, hit->material = m;
        float nl[3] = {0.0f, 0.0f, 0.0f};
        nl[axis] = (float)-step[axis];
        hit->normal = bau__v(c * nl[0] + s * nl[2], nl[1], -s * nl[0] + c * nl[2]);
        return 1;
      }
    }
    entered = 1;
    axis = next[0] < next[1] ? (next[0] < next[2] ? 0 : 2) : (next[1] < next[2] ? 1 : 2);
    t = next[axis];
    i[axis] += step[axis];
    if (i[axis] < 0 || i[axis] >= n[axis]) break;
    next[axis] += delta[axis];
  }
  return mode == BAU__THROUGH ? count : 0;
}

int bau__trace(const bau_context* au, bau__v3 o, bau__v3 d, float tmin, float tmax, bau__hit* hit) {
  int found = 0;
  for (const bau_grid* g = au->grids; g; g = g->next) {
    bau__hit h;
    if (bau__walk(au, g, o, d, tmin, tmax, BAU__FIRST, &h, NULL)) *hit = h, tmax = h.t, found = 1;
  }
  return found;
}

int bau__clear(const bau_context* au, bau__v3 o, bau__v3 d, float tmin, float tmax) {
  if (tmax <= tmin) return 1;
  for (const bau_grid* g = au->grids; g; g = g->next)
    if (bau__walk(au, g, o, d, tmin, tmax, BAU__ANY, NULL, NULL)) return 0;
  return 1;
}

int bau__transmit(const bau_context* au, bau__v3 o, bau__v3 d, float tmin, float tmax, float t[BAU__BANDS]) {
  int cells = 0;
  for (int b = 0; b < BAU__BANDS; ++b) t[b] = 1.0f;
  if (tmax <= tmin) return 0;
  for (const bau_grid* g = au->grids; g; g = g->next) cells += bau__walk(au, g, o, d, tmin, tmax, BAU__THROUGH, NULL, t);
  return cells;
}

int bau_trace_ray(bau_context* au, const float origin[3], const float dir[3], float max_distance, bau_hit* out) {
  if (!au || !origin || !dir) return 0;
  const bau__v3 o = bau__vp(origin), d = bau__norm(bau__vp(dir));
  if (bau__len(d) < 0.5f) return 0;
  bau__hit h = {0};
  bau__lock(&au->world);
  const int found = bau__trace(au, o, d, 0.0f, max_distance, &h);
  bau__unlock(&au->world);
  if (found && out) {
    out->distance = h.t, out->material = h.material;
    bau__store(bau__add(o, bau__mul(d, h.t)), out->point);
    bau__store(h.normal, out->normal);
  }
  return found;
}
