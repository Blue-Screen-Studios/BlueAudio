/* The mix: each source's waveform pulled, muffled (its highs cut), delayed
 * (its path's length: moving, Doppler), panned from where it's heard into
 * the layout's channels (over headphones: each ear its own delay, the far
 * one in the head's shadow); its share sent to the echo.
 *
 * Each render call's targets come from where the source and the listener
 * are then (the game's frames) and the last simulation's slower part; every
 * one of them glided to, never jumped: the delay (and each ear's) through
 * two one-poles (its rate, the pitch, never stepping), the loudness through
 * one, the muffle and the panning eased chunk by chunk. */

#include "internal.h"

/* ----------------------------------------------------------- panning */

#define BAU__D2R (BAU__PI / 180.0f)
#define BAU__LFE (-1000.0f)

int bau__speakers(bau_layout layout, float* az) {
  static const float stereo[] = {-30, 30}, quad[] = {-45, 45, -135, 135};
  static const float s51[] = {-30, 30, 0, BAU__LFE, -110, 110};
  static const float s71[] = {-30, 30, 0, BAU__LFE, -150, 150, -90, 90};
  const float* src;
  int n;
  switch (layout) {
    case BAU_LAYOUT_MONO: az[0] = 0.0f; return 1;
    case BAU_LAYOUT_QUAD: src = quad, n = 4; break;
    case BAU_LAYOUT_SURROUND_51: src = s51, n = 6; break;
    case BAU_LAYOUT_SURROUND_71: src = s71, n = 8; break;
    default: src = stereo, n = 2; break;
  }
  for (int i = 0; i < n; ++i) az[i] = src[i] == BAU__LFE ? BAU__LFE : src[i] * BAU__D2R;
  return n;
}

static float bau__wrap(float a) {
  while (a > BAU__PI) a -= 2.0f * BAU__PI;
  while (a <= -BAU__PI) a += 2.0f * BAU__PI;
  return a;
}

void bau__pan(bau_layout layout, bau__v3 dir, float spread, float* gains) {
  float az[BAU__MAX_CHANNELS];
  const int n = bau__speakers(layout, az);
  for (int i = 0; i < BAU__MAX_CHANNELS; ++i) gains[i] = 0.0f;
  if (n == 1) {
    gains[0] = 1.0f;
    return;
  }
  /* (Straight up or down: no way round to pan it, so all about.) */
  const float h = sqrtf(dir.x * dir.x + dir.z * dir.z);
  spread = 1.0f - (1.0f - bau__clamp(spread, 0.0f, 1.0f)) * bau__clamp(h * 1.5f, 0.0f, 1.0f);
  int speakers = 0;
  if (n == 2) {
    /* Left to right by how far right it is (behind too: speakers can't). */
    const float p = h > 1e-6f ? bau__clamp(dir.x / h, -1.0f, 1.0f) : 0.0f;
    gains[0] = cosf((p + 1.0f) * BAU__PI * 0.25f), gains[1] = sinf((p + 1.0f) * BAU__PI * 0.25f);
    speakers = 2;
  } else {
    /* Between the two speakers either side of it (2D VBAP: the angle split,
     * power kept). */
    const float a = atan2f(dir.x, dir.z);
    int lo = -1, hi = -1;
    float dlo = -1e9f, dhi = 1e9f;
    for (int i = 0; i < n; ++i) {
      if (az[i] == BAU__LFE) continue;
      ++speakers;
      const float d = bau__wrap(az[i] - a);
      if (d <= 0.0f && d > dlo) dlo = d, lo = i;
      if (d >= 0.0f && d < dhi) dhi = d, hi = i;
    }
    if (lo < 0 || hi < 0 || lo == hi) {
      gains[lo >= 0 ? lo : hi] = 1.0f;
    } else {
      const float t = -dlo / (dhi - dlo);
      gains[lo] = cosf(t * BAU__PI * 0.5f), gains[hi] = sinf(t * BAU__PI * 0.5f);
    }
  }
  /* Spread: toward every speaker the same (power kept). */
  for (int i = 0; i < n; ++i) {
    if (az[i] == BAU__LFE) continue;
    gains[i] = sqrtf((1.0f - spread) * gains[i] * gains[i] + spread / (float)speakers);
  }
}

/* ----------------------------------------------------------- sources */

int bau__source_init_render(bau_source* s) {
  const bau_config* c = &s->au->cfg;
  const float seconds = 2.0f * c->max_distance / BAU__SPEED + 0.1f;
  int size = 1024;
  while ((float)size < seconds * (float)c->sample_rate) size *= 2;
  s->ring = (float*)calloc((size_t)size, sizeof(float));
  if (!s->ring) return 0;
  s->ring_mask = size - 1;
  return 1;
}

void bau__source_free_render(bau_source* s) {
  free(s->ring);
  s->ring = NULL;
}

/* A one-pole's coefficient for cutoff `fc` (at or past 18 kHz: open). */
static float bau__pole(float fc, int rate) {
  return fc >= 18000.0f ? 1.0f : 1.0f - expf(-2.0f * BAU__PI * fc / (float)rate);
}

/* The targets for this render call: the source and the listener where
 * they are now, what's published (state lock). (Not traced yet: as if
 * nothing were in the way.) */
static void bau__targets(bau_context* au, bau_source* s, const bau__v3 right, const bau__v3 up, const bau__v3 ahead) {
  bau_heard h;
  bau__heard(s, &h);
  const float rate = (float)au->cfg.sample_rate;
  s->to_amp = h.gain;
  s->to_cut = 20000.0f * powf(300.0f / 20000.0f, bau__clamp(h.muffle, 0.0f, 1.0f));
  s->to_delay = bau__clamp(h.delay * rate, 1.0f, (float)(s->ring_mask - 2048));
  const bau__v3 dw = bau__vp(h.direction);
  bau__v3 d = bau__v(bau__dot(dw, right), bau__dot(dw, up), bau__dot(dw, ahead));
  if (bau__len(d) < 1e-6f) d = bau__v(0.0f, 0.0f, 1.0f), h.spread = 1.0f;
  bau__pan(au->cfg.layout, d, h.spread, s->to_gains);
  /* Over headphones: the far ear later (Woodworth's: up to ~0.66 ms) and in
   * the head's shadow; behind, both a little duller. */
  const float hz = sqrtf(d.x * d.x + d.z * d.z);
  const float p = hz > 1e-6f ? bau__clamp(d.x / hz, -1.0f, 1.0f) * (1.0f - h.spread) : 0.0f;
  const float theta = asinf(fabsf(p)), itd = 0.0875f / BAU__SPEED * (theta + sinf(theta)) * rate;
  const float behind = d.z < 0.0f ? -d.z * (1.0f - h.spread) : 0.0f;
  const float dull = 20000.0f - 12000.0f * behind, shade = bau__lerp(dull, 1800.0f, powf(fabsf(p), 1.5f));
  s->to_itd[0] = p > 0.0f ? itd : 0.0f, s->to_itd[1] = p < 0.0f ? itd : 0.0f;
  s->to_shadow[0] = p > 0.0f ? shade : dull, s->to_shadow[1] = p < 0.0f ? shade : dull;
}

/* A sample `delay` samples back from the ring's write head (linear). */
static float bau__tap(const bau_source* s, int at, float delay) {
  const float back = (float)at - delay;
  const float f = floorf(back);
  const int i = (int)f;
  const float frac = back - f;
  const float a = s->ring[i & s->ring_mask], b = s->ring[(i + 1) & s->ring_mask];
  return a + (b - a) * frac;
}

/* One source's chunk (`n` frames) into `out`, its share into `send`. */
static void bau__source_chunk(bau_context* au, bau_source* s, float* out, float* send, int n) {
  const int rate = au->cfg.sample_rate, ch = au->channels;
  const int headphones = au->cfg.layout == BAU_LAYOUT_HEADPHONES;
  float* mono = au->mono;
  if (!s->ended) {
    int got = s->fill(s->user, mono, n);
    if (got < 0) got = 0;
    if (got < n) {
      memset(mono + got, 0, (size_t)(n - got) * sizeof(float));
      s->ended = 1;
      s->tail = (int)fmaxf(s->delay, s->to_delay) + 64 + (n - got);
    }
  } else {
    memset(mono, 0, (size_t)n * sizeof(float));
    s->tail -= n;
  }
  /* (Per sample: one-poles' coefficients for these time constants.) */
  const float k_delay = 1.0f - expf(-1.0f / (0.025f * (float)rate)), k_itd = 1.0f - expf(-1.0f / (0.02f * (float)rate));
  const float k_amp = 1.0f - expf(-1.0f / (0.03f * (float)rate));
  /* (Per chunk: the muffle's, the ears' shadows' and the panning's easing.) */
  const float k_cut = 1.0f - expf(-(float)n / (0.05f * (float)rate)), k_pan = 1.0f - expf(-(float)n / (0.03f * (float)rate));
  if (!s->started) {
    s->started = 1;
    s->delay = s->delay_mid = s->to_delay, s->amp = s->to_amp, s->cut = s->to_cut;
    memcpy(s->gains, s->to_gains, sizeof s->gains);
    memcpy(s->itd, s->to_itd, sizeof s->itd), memcpy(s->itd_mid, s->to_itd, sizeof s->itd_mid);
    memcpy(s->shadow, s->to_shadow, sizeof s->shadow);
  }
  /* The muffle (two one-poles: 12 dB an octave), its cutoff eased. */
  s->cut += (s->to_cut - s->cut) * k_cut;
  const float pole = bau__pole(s->cut, rate);
  for (int i = 0; i < n; ++i) {
    s->lp[0] += (mono[i] - s->lp[0]) * pole;
    s->lp[1] += (s->lp[0] - s->lp[1]) * pole;
    s->ring[(s->ring_at + i) & s->ring_mask] = s->lp[1];
  }
  /* (Further off than a quarter second at once: a jump, not a glide.) */
  if (fabsf(s->to_delay - s->delay) > 0.25f * (float)rate) s->delay = s->delay_mid = s->to_delay;
  float ear_pole[2];
  for (int e = 0; e < 2; ++e) {
    s->shadow[e] += (s->to_shadow[e] - s->shadow[e]) * k_cut;
    ear_pole[e] = bau__pole(s->shadow[e], rate);
  }
  /* The panning: toward its target this chunk, ramped across it. */
  float g0[BAU__MAX_CHANNELS], dg[BAU__MAX_CHANNELS];
  for (int c = 0; c < ch; ++c) {
    const float to = s->gains[c] + (s->to_gains[c] - s->gains[c]) * k_pan;
    g0[c] = s->gains[c], dg[c] = (to - s->gains[c]) / (float)n, s->gains[c] = to;
  }
  /* (Its rate kept under 8%: a pitch never bent past that, whatever.) */
  const float most = 0.08f;
  for (int i = 0; i < n; ++i) {
    s->delay_mid += (s->to_delay - s->delay_mid) * k_delay;
    const float step = (s->delay_mid - s->delay) * k_delay;
    s->delay += step > most ? most : step < -most ? -most : step;
    s->amp += (s->to_amp - s->amp) * k_amp;
    const int at = s->ring_at + i;
    float* o = out + (size_t)i * ch;
    if (headphones) {
      float ear[2];
      for (int e = 0; e < 2; ++e) {
        s->itd_mid[e] += (s->to_itd[e] - s->itd_mid[e]) * k_itd;
        s->itd[e] += (s->itd_mid[e] - s->itd[e]) * k_itd;
        const float v = bau__tap(s, at, s->delay + s->itd[e]) * s->amp;
        s->ear[e] += (v - s->ear[e]) * ear_pole[e];
        ear[e] = s->ear[e];
      }
      o[0] += ear[0] * (g0[0] + dg[0] * (float)i);
      o[1] += ear[1] * (g0[1] + dg[1] * (float)i);
      send[i] += 0.5f * (ear[0] + ear[1]);
    } else {
      const float v = bau__tap(s, at, s->delay) * s->amp;
      for (int c = 0; c < ch; ++c) o[c] += v * (g0[c] + dg[c] * (float)i);
      send[i] += v;
    }
  }
  s->ring_at = (s->ring_at + n) & s->ring_mask;
}

void bau_render(bau_context* au, float* out, int frames) {
  if (!au || !out || frames <= 0) return;
  bau__lock(&au->render);
  const int ch = au->channels;
  memset(out, 0, (size_t)frames * ch * sizeof(float));
  /* The targets: what's published, the listener as it is now. */
  bau__lock(&au->state);
  const bau__v3 ahead = bau__vp(au->listener + 3), up = bau__vp(au->listener + 6), right = bau__cross(ahead, up);
  for (bau_source* s = au->sources; s; s = s->next) bau__targets(au, s, right, up, ahead);
  const bau_echo echo = au->echo;
  float listener[9];
  memcpy(listener, au->listener, sizeof listener);
  bau__unlock(&au->state);
  bau__reverb_set(au->reverb, &echo, listener);
  for (int done = 0; done < frames;) {
    const int n = frames - done < BAU__CHUNK ? frames - done : BAU__CHUNK;
    float* o = out + (size_t)done * ch;
    memset(au->send, 0, sizeof au->send);
    for (bau_source* s = au->sources; s; s = s->next) {
      if (s->ended && s->tail <= 0) continue;
      bau__source_chunk(au, s, o, au->send, n);
    }
    bau__reverb_process(au->reverb, au->send, o, n, ch, au->cfg.layout);
    done += n;
  }
  bau__unlock(&au->render);
}
