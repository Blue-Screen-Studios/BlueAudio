/* The echo heard: what's sent (every source's share) rung through a
 * feedback delay network (8 lines, a Householder mix, each line damped:
 * the highs die first), ringing as long as the rays said (RT60), as loud as
 * what came back to the listener; its lines read out differently for each
 * channel (Hadamard rows: no two alike), leaning toward where most came
 * back from. Distinct echoes (a far wall) as taps of their own, each from
 * its way: faded in as one's found, out as it's lost, its delay kept while
 * it's the same echo (one within 12 ms of it): never a jump, a click. */

#include "internal.h"

#define BAU__LINES 8

struct bau__reverb {
  int rate;
  float* line[BAU__LINES];
  int len[BAU__LINES], at[BAU__LINES];
  float g[BAU__LINES], lp[BAU__LINES];
  float in_scale, wet, to_wet;
  float lean[3], focus;  /* (listener space) */
  float leaned[BAU__MAX_CHANNELS];  /* (each channel's share as it is: eased) */
  float* ring;           /* the send, for the taps */
  int ring_mask, ring_at;
  struct {
    float delay, gain, to_gain, dir[3];  /* (delay: samples; gain eased to to_gain; 0 and gone: free) */
    float pan[BAU__MAX_CHANNELS];
  } tap[BAU__TAPS];
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
  /* The echoes: each found matched to a tap already sounding (within 12
   * ms: its delay kept), else given a free one (faded in); taps no longer
   * found faded out. */
  int matched[BAU__TAPS] = {0};
  for (int t = 0; t < e->taps; ++t) {
    const float delay = e->tap_delay[t] * (float)r->rate;
    int at = -1;
    for (int k = 0; k < BAU__TAPS && at < 0; ++k)
      if (!matched[k] && (r->tap[k].gain > 0.0f || r->tap[k].to_gain > 0.0f) && fabsf(r->tap[k].delay - delay) < 0.012f * (float)r->rate) at = k;
    for (int k = 0; k < BAU__TAPS && at < 0; ++k)
      if (!matched[k] && r->tap[k].gain <= 0.0f && r->tap[k].to_gain <= 0.0f) at = k, r->tap[k].delay = delay;
    if (at < 0) continue;
    matched[at] = 1;
    r->tap[at].to_gain = e->tap_gain[t];
    bau__store(bau__to_listener(e->tap_direction[t], listener), r->tap[at].dir);
  }
  for (int k = 0; k < BAU__TAPS; ++k)
    if (!matched[k]) r->tap[k].to_gain = 0.0f;
}

void bau__reverb_process(bau__reverb* r, const float* send, float* out, int frames, int channels, bau_layout layout) {
  /* Each channel's share of the ring (leaning toward where it came back
   * from), eased toward this call's, ramped across it; each tap's. */
  float lean[BAU__MAX_CHANNELS], lean0[BAU__MAX_CHANNELS], dlean[BAU__MAX_CHANNELS], az[BAU__MAX_CHANNELS];
  const int speakers = bau__speakers(layout, az);
  int heard = 0;
  for (int c = 0; c < speakers; ++c) heard += az[c] > -999.0f;
  bau__pan(layout, bau__vp(r->lean), 1.0f - r->focus * 0.6f, lean);
  const float k_lean = 1.0f - expf(-(float)frames / (0.1f * (float)r->rate));
  for (int c = 0; c < channels; ++c) {
    const float to = r->leaned[c] + (lean[c] * sqrtf((float)heard) - r->leaned[c]) * k_lean;
    lean0[c] = r->leaned[c], dlean[c] = (to - r->leaned[c]) / (float)frames, r->leaned[c] = to;
  }
  for (int t = 0; t < BAU__TAPS; ++t)
    if (r->tap[t].gain > 0.0f || r->tap[t].to_gain > 0.0f) bau__pan(layout, bau__vp(r->tap[t].dir), 0.2f, r->tap[t].pan);
  const float ease = 1.0f - expf(-1.0f / (0.05f * (float)r->rate)), tap_ease = 1.0f - expf(-1.0f / (0.08f * (float)r->rate));
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
        const float share = lean0[c] + dlean[c] * (float)i;
        if (share == 0.0f) continue;
        float v = 0.0f;
        for (int k = 0; k < BAU__LINES; ++k) {
          const int m = c & k;
          v += ((m ^ (m >> 1) ^ (m >> 2)) & 1) ? -y[k] : y[k];
        }
        o[c] += v * norm * r->wet * share;
      }
    }
    r->ring[r->ring_at] = send[i];
    for (int t = 0; t < BAU__TAPS; ++t) {
      if (r->tap[t].gain <= 0.0f && r->tap[t].to_gain <= 0.0f) continue;
      r->tap[t].gain += (r->tap[t].to_gain - r->tap[t].gain) * tap_ease;
      if (r->tap[t].to_gain <= 0.0f && r->tap[t].gain < 1e-4f) r->tap[t].gain = 0.0f;  /* (faded out: free) */
      const float v = r->ring[(r->ring_at - (int)r->tap[t].delay) & r->ring_mask] * r->tap[t].gain;
      for (int c = 0; c < channels; ++c) o[c] += v * r->tap[t].pan[c];
    }
    r->ring_at = (r->ring_at + 1) & r->ring_mask;
  }
}
