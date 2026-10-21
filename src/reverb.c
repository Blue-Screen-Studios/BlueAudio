/* The echo heard: what's sent (every source's share) rung through a
 * feedback delay network (8 lines, a Householder mix, each line damped:
 * the highs die first), ringing as long as the rays said (RT60), as loud as
 * what came back to the listener; its lines read out differently for each
 * channel (Hadamard rows: no two alike), leaning toward where most came
 * back from. Distinct echoes (a far wall) as taps of their own, each from
 * its way. */

#include "internal.h"

#define BAU__LINES 8

struct bau__reverb {
  int rate;
  float* line[BAU__LINES];
  int len[BAU__LINES], at[BAU__LINES];
  float g[BAU__LINES], lp[BAU__LINES];
  float in_scale, wet, to_wet;
  float lean[3], focus;  /* (listener space) */
  float* ring;           /* the send, for the taps */
  int ring_mask, ring_at;
  int taps;
  float tap_delay[4], tap_gain[4], tap_dir[4][3];
};

bau__reverb* bau__reverb_create(int rate) {
  static const float ms[BAU__LINES] = {31.3f, 37.9f, 41.7f, 47.3f, 53.1f, 59.9f, 67.7f, 73.1f};
  bau__reverb* r = (bau__reverb*)calloc(1, sizeof *r);
  if (!r) return NULL;
  r->rate = rate;
  for (int i = 0; i < BAU__LINES; ++i) {
    r->len[i] = (int)(ms[i] * 0.001f * (float)rate);
    r->line[i] = (float*)calloc((size_t)r->len[i], sizeof(float));
    if (!r->line[i]) {
      bau__reverb_destroy(r);
      return NULL;
    }
  }
  int size = 1024;
  while (size < 2 * rate + 1024) size *= 2;
  r->ring = (float*)calloc((size_t)size, sizeof(float));
  if (!r->ring) {
    bau__reverb_destroy(r);
    return NULL;
  }
  r->ring_mask = size - 1;
  return r;
}

void bau__reverb_destroy(bau__reverb* r) {
  if (!r) return;
  for (int i = 0; i < BAU__LINES; ++i) free(r->line[i]);
  free(r->ring);
  free(r);
}

static bau__v3 bau__to_listener(const float* dir, const float listener[9]) {
  const bau__v3 ahead = bau__vp(listener + 3), up = bau__vp(listener + 6), right = bau__cross(ahead, up), d = bau__vp(dir);
  return bau__v(bau__dot(d, right), bau__dot(d, up), bau__dot(d, ahead));
}

void bau__reverb_set(bau__reverb* r, const bau_echo* e, const float listener[9]) {
  const float rt = e->decay;
  float g2 = 0.0f;
  for (int i = 0; i < BAU__LINES; ++i) {
    r->g[i] = rt > 0.05f ? powf(10.0f, -3.0f * (float)r->len[i] / ((float)r->rate * rt)) : 0.0f;
    g2 += r->g[i] * r->g[i];
  }
  g2 /= (float)BAU__LINES;
  /* (Its ring as loud as what went in, however long it rings.) */
  r->in_scale = sqrtf(1.0f - (g2 < 0.999f ? g2 : 0.999f));
  r->to_wet = rt > 0.05f ? e->level * 0.7f : 0.0f;
  const bau__v3 lean = bau__to_listener(e->direction, listener);
  bau__store(lean, r->lean);
  r->focus = e->focus;
  r->taps = e->taps;
  for (int t = 0; t < e->taps; ++t) {
    r->tap_delay[t] = e->tap_delay[t] * (float)r->rate, r->tap_gain[t] = e->tap_gain[t];
    bau__store(bau__to_listener(e->tap_direction[t], listener), r->tap_dir[t]);
  }
}

void bau__reverb_process(bau__reverb* r, const float* send, float* out, int frames, int channels, bau_layout layout) {
  /* Each channel's share of the ring (leaning toward where it came back
   * from); each tap's. */
  float lean[BAU__MAX_CHANNELS], taps[4][BAU__MAX_CHANNELS], az[BAU__MAX_CHANNELS];
  const int speakers = bau__speakers(layout, az);
  int heard = 0;
  for (int c = 0; c < speakers; ++c) heard += az[c] > -999.0f;
  bau__pan(layout, bau__vp(r->lean), 1.0f - r->focus * 0.6f, lean);
  for (int c = 0; c < channels; ++c) lean[c] *= sqrtf((float)heard);
  for (int t = 0; t < r->taps; ++t) bau__pan(layout, bau__vp(r->tap_dir[t]), 0.2f, taps[t]);
  const float ease = 1.0f - expf(-1.0f / (0.05f * (float)r->rate));
  const float norm = 1.0f / sqrtf((float)BAU__LINES);
  for (int i = 0; i < frames; ++i) {
    r->wet += (r->to_wet - r->wet) * ease;
    const float x = send[i] * r->in_scale;
    float y[BAU__LINES], sum = 0.0f;
    for (int k = 0; k < BAU__LINES; ++k) y[k] = r->line[k][r->at[k]], sum += y[k];
    sum *= 2.0f / (float)BAU__LINES;
    for (int k = 0; k < BAU__LINES; ++k) {
      /* (Householder: each line fed back from all; damped: the highs first.) */
      r->lp[k] += (y[k] - sum - r->lp[k]) * 0.55f;
      r->line[k][r->at[k]] = ((k & 1) ? -x : x) + r->g[k] * r->lp[k];
      if (++r->at[k] >= r->len[k]) r->at[k] = 0;
    }
    float* o = out + (size_t)i * channels;
    if (r->wet > 1e-5f) {
      for (int c = 0; c < channels; ++c) {
        if (lean[c] == 0.0f) continue;
        float v = 0.0f;
        for (int k = 0; k < BAU__LINES; ++k) {
          const int m = c & k;
          v += ((m ^ (m >> 1) ^ (m >> 2)) & 1) ? -y[k] : y[k];
        }
        o[c] += v * norm * r->wet * lean[c];
      }
    }
    r->ring[r->ring_at] = send[i];
    for (int t = 0; t < r->taps; ++t) {
      const float v = r->ring[(r->ring_at - (int)r->tap_delay[t]) & r->ring_mask] * r->tap_gain[t];
      for (int c = 0; c < channels; ++c) o[c] += v * taps[t][c];
    }
    r->ring_at = (r->ring_at + 1) & r->ring_mask;
  }
}
