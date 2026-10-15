// Clock mapping and edge schedule for promicro-vesa-stereo, free of AVR specifics
// so the same code runs in the host simulation (tests/sim_sync_core.cpp).
//
// Time is timer ticks (2 MHz on the board), uint32 with wrap-safe differences.

#pragma once
#include <stdint.h>

namespace stereosync {

static const uint32_t TICKS_PER_S = 2000000UL;
static const uint32_t TICKS_PER_US = 2;
static const int32_t ARM_MARGIN = 200;    // never arm an edge closer than 100 us
static const int32_t ARM_WINDOW = 30000;  // OCR1A is 16 bits: stay inside 15 ms
static const uint8_t WINDOWS = 8;         // skew fit over the last 8 one-second minima
// The frame period is exact: box time over vsync count since the span began.
// A rate change (VRR between films) starts a new span; so does an old one,
// before the box's 32-bit microsecond clock could wrap within it.
static const uint32_t SPAN_MAX_US = 1800000000UL;   // 30 min
static const uint32_t SPAN_MIN_FRAMES = 64;         // a fresh span is trusted from here
static const uint32_t RATE_JUMP_PPM = 5000;         // 0.5 %: another rate
// what a box silent for the 2 s the glasses coast can still be (240 Hz max)
static const uint32_t MAX_COAST_FRAMES = 512;
static const uint32_t MAX_COAST_US = 2100000UL;

// Edge n is at anchor + n * period (period in 1/256 tick), showing
// anchor_left ^ (n odd).
struct Schedule {
  uint32_t anchor_t = 0;
  uint8_t anchor_frac = 0;
  bool anchor_left = true;
  uint32_t period_q8 = 0;
  bool have_anchor = false;
  uint32_t last_edge = 0;
  bool have_last_edge = false;
  int32_t right_extra = 0;       // ticks added to edges into the right eye only


  void set_free_rate(uint32_t millihertz)
  {
    if (millihertz < 1000)
      millihertz = 1000;
    period_q8 = (uint32_t)(((uint64_t)TICKS_PER_S * 1000ULL * 256ULL) / millihertz);
  }

  // Edge 0 at t, showing the left eye if left; every period the other eye.
  void anchor(uint32_t t, bool left, uint32_t pq8)
  {
    anchor_t = t;
    anchor_frac = 0;
    anchor_left = left;
    period_q8 = pq8;
    have_anchor = true;
  }

  // The next edge to arm at 'now': true with *when/*left set, false when there
  // is nothing to arm yet. free_run re-anchors on the edge it returns.
  bool next(uint32_t now, bool free_run, uint32_t *when, bool *left)
  {
    if (!have_anchor || !period_q8)
      return false;
    // start at the edge 'now' falls in, rather than walking up from the anchor
    uint32_t n0 = 0;
    int32_t since = (int32_t)(now - anchor_t);
    if (since > 0)
      n0 = (uint32_t)(((uint64_t)(uint32_t)since << 8) / period_q8);
    int32_t half = (int32_t)(period_q8 >> 9);
    int32_t extra = right_extra;
    if (extra > half / 2)        // keep the edges in order whatever the rate
      extra = half / 2;
    if (extra < -half / 2)
      extra = -half / 2;
    for (uint32_t n = n0; n < n0 + 4; n++) {
      uint64_t off = (uint64_t)anchor_frac + (uint64_t)n * period_q8;
      uint32_t base = anchor_t + (uint32_t)(off >> 8);
      bool l = anchor_left ^ (n & 1);
      uint32_t t = l ? base : base + (uint32_t)extra;
      if ((int32_t)(t - now) <= ARM_MARGIN)
        continue;
      if (have_last_edge && (int32_t)(t - last_edge) <= half)
        continue;                // the anchor moved: never fire one edge twice
      if ((int32_t)(t - now) > ARM_WINDOW)
        return false;            // too far ahead for the 16-bit compare yet
      *when = t;
      *left = l;
      last_edge = t;
      have_last_edge = true;
      if (free_run) {
        anchor_t = base;
        anchor_frac = off & 0xff;
        anchor_left = l;
      }
      return true;
    }
    return false;
  }
};

// d = arrival - 2 * box_us. Its floor is the fixed part of the USB path; the
// skew between the box clock and our crystal makes the floor drift, so the
// floor is fitted over the last WINDOWS one-second minima. That fit is the
// only estimate: the vsync times and the frame period come from the box.
struct ClockMap {
  bool have_ref = false;
  uint32_t d_ref = 0;
  int32_t win_min = 0;
  uint32_t win_min_t = 0;        // when the window's minimum arrived
  bool win_has = false;
  uint32_t win_start = 0;
  uint32_t win_t[WINDOWS] = {};
  int32_t win_m[WINDOWS] = {};
  uint8_t win_n = 0, win_head = 0;
  float skew = 0;                // floor drift per tick
  int32_t fit_m = 0;
  uint32_t fit_t = 0;
  bool have_fit = false;

  uint32_t last_msg_t = 0, last_vsync = 0, last_us = 0;
  bool have_last_msg = false;
  uint32_t span_vsync = 0, span_us = 0;
  bool have_span = false;
  uint32_t period_q8 = 0;        // exact frame period, box time in 1/256 tick
  uint32_t msgs = 0, resets = 0, rate_changes = 0;
  int32_t last_resid = 0;

  void reset()
  {
    have_ref = have_fit = win_has = false;
    win_n = win_head = 0;
    skew = 0;
    have_last_msg = have_span = false;
    period_q8 = 0;
    resets++;
  }

  // du box microseconds over dv vsyncs, in 1/256 tick
  static uint32_t q8(uint32_t du, uint32_t dv)
  {
    return (uint32_t)(((uint64_t)du * TICKS_PER_US * 256ULL + dv / 2) / dv);
  }

  static bool differs(uint32_t a, uint32_t b, uint32_t ppm)
  {
    uint32_t d = a > b ? a - b : b - a;
    return (uint64_t)d * 1000000ULL > (uint64_t)b * ppm;
  }

  float period_us() const { return period_q8 / (256.0f * TICKS_PER_US); }

  void fit()
  {
    if (win_n < 2) {
      uint8_t i = (uint8_t)((win_head + WINDOWS - 1) % WINDOWS);
      fit_t = win_t[i];
      fit_m = win_m[i];
      skew = 0;
      have_fit = true;
      return;
    }
    uint8_t first = (uint8_t)((win_head + WINDOWS - win_n) % WINDOWS);
    uint32_t t0 = win_t[first];
    float sx = 0, sy = 0, sxx = 0, sxy = 0;
    for (uint8_t k = 0; k < win_n; k++) {
      uint8_t i = (uint8_t)((first + k) % WINDOWS);
      float x = (float)(int32_t)(win_t[i] - t0);
      float y = (float)win_m[i];
      sx += x; sy += y; sxx += x * x; sxy += x * y;
    }
    float n = win_n;
    float den = n * sxx - sx * sx;
    skew = den != 0 ? (n * sxy - sx * sy) / den : 0;
    uint8_t last = (uint8_t)((win_head + WINDOWS - 1) % WINDOWS);
    fit_t = win_t[last];
    fit_m = (int32_t)((sy - skew * sx) / n + skew * (float)(int32_t)(win_t[last] - t0));
    have_fit = true;
  }

  // The exact period from this message: the span's, or this interval's when
  // the rate just changed.
  void track_period(uint32_t vsync, uint32_t us, uint32_t dv, uint32_t du)
  {
    uint32_t step = q8(du, dv);
    if (!have_span) {
      span_vsync = last_vsync;
      span_us = last_us;
      have_span = true;
      period_q8 = step;
    } else if (period_q8 && differs(step, period_q8, RATE_JUMP_PPM)) {
      span_vsync = last_vsync;   // another rate: a new span from this interval
      span_us = last_us;
      period_q8 = step;
      rate_changes++;
      return;
    } else if (us - span_us > SPAN_MAX_US) {
      span_vsync = last_vsync;   // before the box clock could wrap in the span;
      span_us = last_us;         // the period stands until the new span is long
    }
    uint32_t sdv = vsync - span_vsync;
    if (sdv >= SPAN_MIN_FRAMES || period_q8 == 0)
      period_q8 = q8(us - span_us, sdv);
  }

  // Feed one message. On success *vsync_t is the vsync in our ticks and
  // *period_q8 the frame period in our ticks (1/256); false until both are
  // known.
  bool on_sync(uint32_t vsync, uint32_t us, uint32_t arrival, uint32_t lost_ticks,
               uint32_t *vsync_t, uint32_t *out_period_q8)
  {
    msgs++;
    if (have_last_msg) {
      uint32_t dv = vsync - last_vsync;
      uint32_t du = us - last_us;
      // a coast of up to lost_ticks is not a restart: its frames and time
      // still give the period
      if (dv == 0 || dv > MAX_COAST_FRAMES || du > MAX_COAST_US ||
          (int32_t)(arrival - last_msg_t) > (int32_t)lost_ticks)
        reset();                 // box restarted, clock jumped, or a long gap
      else
        track_period(vsync, us, dv, du);
    }
    last_vsync = vsync;
    last_us = us;
    last_msg_t = arrival;
    have_last_msg = true;

    uint32_t d = arrival - TICKS_PER_US * us;
    if (!have_ref) {
      d_ref = d;
      have_ref = true;
      win_start = arrival;
    }
    int32_t rel = (int32_t)(d - d_ref);
    if (!win_has || rel < win_min) {
      win_min = rel;
      win_min_t = arrival;
    }
    win_has = true;
    if ((int32_t)(arrival - win_start) >= (int32_t)TICKS_PER_S) {
      // placed where the minimum was seen: with skew the floor moves within
      // the window, and stamping it at the window's end biases the fit
      win_t[win_head] = win_min_t;
      win_m[win_head] = win_min;
      win_head = (uint8_t)((win_head + 1) % WINDOWS);
      if (win_n < WINDOWS)
        win_n++;
      win_has = false;
      win_start = arrival;
      fit();
    }

    int32_t floor_now = have_fit
      ? fit_m + (int32_t)(skew * (float)(int32_t)(arrival - fit_t))
      : win_min;                 // first second: the best seen so far
    last_resid = rel - floor_now;
    if (period_q8 == 0)
      return false;
    *vsync_t = d_ref + (uint32_t)floor_now + TICKS_PER_US * us;
    // box time to our ticks: the period stretches with the skew
    *out_period_q8 = period_q8 + (uint32_t)(int32_t)((float)period_q8 * skew);
    return true;
  }
};

}  // namespace stereosync
