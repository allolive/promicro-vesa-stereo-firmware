// Host simulation of the firmware's clock mapping and edge schedule
// (firmware/promicro-vesa-stereo/sync_core.h), the same code the board runs.
//
// A box sends one message per vsync. The board's crystal runs off the box
// clock by a given skew; each message reaches the board after a fixed path
// delay plus a random USB/scheduling delay with occasional long stalls; some
// vsyncs are never sent. The board loop polls every 20 us. After the lock has
// settled, every armed edge must be at the true vsync + delay (in board ticks)
// within a tolerance, show the right eye, and no edge may be missed or
// doubled.
//
// Options (all optional):
//   --rate MHZ       vsync rate in millihertz (96000 = 96 Hz)          [96000]
//   --rate2 MHZ      switch to this rate at --switch-at seconds        [none]
//   --switch-at S    when the rate changes (VRR between films)         [25]
//   --skew PPM       board crystal against the box clock               [0]
//   --delay US       delay from vsync to the eye switch                [0]
//   --seconds S      length of the run                                 [40]
//   --seed N         random seed                                       [1]
//   --gap S          no 3D for S seconds from 20 s (a break between films)
//   --stall S        no messages for S seconds from 25 s while the video goes
//                    on: the board must coast on the last anchor and period
// Prints one line of statistics; exit status 0 when within tolerance.

#include "sync_core.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>

using namespace stereosync;

namespace {

struct Options {
  double rate_mhz = 96000, rate2_mhz = 0, switch_at = 25;
  double skew_ppm = 0, seconds = 40, gap = 0, stall = 0;
  int32_t delay_us = 0;
  unsigned seed = 1;
};

bool parse(int argc, char **argv, Options *o)
{
  for (int i = 1; i + 1 < argc; i += 2) {
    const char *k = argv[i], *v = argv[i + 1];
    if (!strcmp(k, "--rate")) o->rate_mhz = atof(v);
    else if (!strcmp(k, "--rate2")) o->rate2_mhz = atof(v);
    else if (!strcmp(k, "--switch-at")) o->switch_at = atof(v);
    else if (!strcmp(k, "--skew")) o->skew_ppm = atof(v);
    else if (!strcmp(k, "--delay")) o->delay_us = atoi(v);
    else if (!strcmp(k, "--seconds")) o->seconds = atof(v);
    else if (!strcmp(k, "--seed")) o->seed = (unsigned)atoi(v);
    else if (!strcmp(k, "--gap")) o->gap = atof(v);
    else if (!strcmp(k, "--stall")) o->stall = atof(v);
    else return false;
  }
  return argc % 2 == 1;
}

struct Vsync { double t; uint32_t k; };
struct Msg { double arrive; uint32_t vsync, us; char eye; };

}  // namespace

int main(int argc, char **argv)
{
  Options o;
  if (!parse(argc, argv, &o)) {
    fprintf(stderr, "usage: %s [--rate MHZ] [--rate2 MHZ] [--switch-at S] [--skew PPM] "
                    "[--delay US] [--seconds S] [--seed N] [--gap S] [--stall S]\n", argv[0]);
    return 2;
  }
  std::mt19937 rng(o.seed);
  std::exponential_distribution<double> jitter(1.0 / 250.0);   // mean 250 us
  std::uniform_real_distribution<double> uni(0, 1);

  const double gap_from = 20.0, gap_to = gap_from + o.gap;
  const double stall_from = 25.0, stall_to = stall_from + o.stall;
  const double box_us0 = 4.29e9 - 3e6;       // box clock wraps 3 s in
  const double board_ticks0 = 4.29e9 - 1e6;  // board clock wraps too
  auto board_ticks = [&](double t) {         // board tick count at true time t
    double v = board_ticks0 + t * TICKS_PER_S * (1.0 + o.skew_ppm * 1e-6);
    return (uint32_t)(uint64_t)std::fmod(v, 4294967296.0);
  };
  auto box_us = [&](double t) {
    return (uint32_t)(uint64_t)std::fmod(box_us0 + t * 1e6, 4294967296.0);
  };
  auto rate_at = [&](double t) {
    return (o.rate2_mhz > 0 && t >= o.switch_at ? o.rate2_mhz : o.rate_mhz) / 1000.0;
  };
  auto in_gap = [&](double t) { return o.gap > 0 && t >= gap_from && t < gap_to; };

  // the vsyncs the box shows while 3D plays
  std::vector<Vsync> vs;
  uint32_t k = 0;
  for (double t = 0; t < o.seconds; t += 1.0 / rate_at(t), k++)
    if (!in_gap(t))
      vs.push_back({t, k});

  const double path = 180e-6;
  std::vector<Msg> msgs;
  for (const Vsync &v : vs) {
    if (o.stall > 0 && v.t >= stall_from && v.t < stall_to)
      continue;                              // the box keeps showing, says nothing
    if (uni(rng) < 0.01)
      continue;                              // ISR dropped / reader fell behind
    double extra = jitter(rng) * 1e-6;
    if (uni(rng) < 0.005)
      extra += 5e-3;                         // a long stall
    msgs.push_back({v.t + path + extra, v.k, box_us(v.t), (v.k & 1) ? 'R' : 'L'});
  }
  std::sort(msgs.begin(), msgs.end(), [](const Msg &a, const Msg &b) { return a.arrive < b.arrive; });

  // edges are judged once the fit has 8 windows, and again after a break or
  // a rate change has settled
  const double settle = 10.0;
  auto judged = [&](double te) {
    if (te < settle)
      return false;
    if (o.gap > 0 && te >= gap_from - 1 && te < gap_to + settle)
      return false;
    if (o.rate2_mhz > 0 && te >= o.switch_at && te < o.switch_at + 0.5)
      return false;
    return true;
  };
  auto nearest = [&](double t) {             // the vsync an edge belongs to
    auto it = std::lower_bound(vs.begin(), vs.end(), t,
                               [](const Vsync &v, double x) { return v.t < x; });
    if (it == vs.end())
      return vs.back();
    if (it != vs.begin() && t - (it - 1)->t < it->t - t)
      --it;
    return *it;
  };

  Schedule sched;
  ClockMap clk;
  size_t mi = 0;
  bool pending = false;
  uint32_t pend_t = 0;
  bool pend_left = false;
  const double poll = 20e-6;
  double worst = 0, sum = 0;
  long edges = 0, wrong_eye = 0;
  std::vector<double> edge_times;

  for (double t = 0; t < o.seconds; t += poll) {
    uint32_t now = board_ticks(t);
    if (pending && (int32_t)(now - pend_t) >= 0) {
      pending = false;
      double te = t - (int32_t)(now - pend_t) / (TICKS_PER_S * (1.0 + o.skew_ppm * 1e-6));
      Vsync v = nearest(te - o.delay_us * 1e-6 - path);
      double err = (te - (v.t + path + o.delay_us * 1e-6)) * 1e6;
      bool want_left = (v.k & 1) == 0;
      if (judged(te)) {
        edges++;
        sum += err;
        worst = std::max(worst, std::fabs(err));
        if (want_left != pend_left)
          wrong_eye++;
        edge_times.push_back(te);
      }
    }
    // the firmware's loss path: a silent box stops the glasses and the clock
    if (clk.have_last_msg && now - clk.last_msg_t > 2 * TICKS_PER_S) {
      int32_t extra = sched.right_extra;
      sched = Schedule();
      sched.right_extra = extra;
      clk.reset();
      pending = false;
    }
    while (mi < msgs.size() && msgs[mi].arrive <= t) {
      uint32_t vt, pq8;
      if (clk.on_sync(msgs[mi].vsync, msgs[mi].us, now, 2 * TICKS_PER_S, &vt, &pq8))
        sched.anchor(vt + (uint32_t)(o.delay_us * (int32_t)TICKS_PER_US), msgs[mi].eye == 'L', pq8);
      mi++;
    }
    if (!pending) {
      uint32_t when;
      bool left;
      if (sched.next(now, false, &when, &left)) {
        pending = true;
        pend_t = when;
        pend_left = left;
      }
    }
  }

  long expect = 0;
  for (const Vsync &v : vs)
    if (judged(v.t + path + o.delay_us * 1e-6))
      expect++;
  long gaps = 0, doubles = 0;
  for (size_t i = 1; i < edge_times.size(); i++) {
    double d = edge_times[i] - edge_times[i - 1];
    double p = 1.0 / rate_at(edge_times[i]);
    if (o.gap > 0 && edge_times[i - 1] < gap_from && edge_times[i] >= gap_to)
      continue;                              // the break itself
    if (o.rate2_mhz > 0 && edge_times[i - 1] < o.switch_at + 0.5 && edge_times[i] >= o.switch_at)
      continue;                              // across the rate change
    if (d > 1.5 * p)
      gaps++;
    if (d < 0.5 * p)
      doubles++;
  }
  // The constant path delay is absorbed by the tuned delay, so judge the
  // spread around the mean, not the mean itself.
  double mean = edges ? sum / edges : 0;
  printf("rate %.3f%s skew %+.0fppm delay %d gap %.0fs stall %.1fs: edges %ld/%ld "
         "mean %+.1fus worst %.1fus wrong_eye %ld gaps %ld doubles %ld skew_est %+.1fppm "
         "period %.3fus rate_changes %u resets %u\n",
         o.rate_mhz / 1000, o.rate2_mhz > 0 ? "->2nd" : "", o.skew_ppm, o.delay_us, o.gap,
         o.stall, edges, expect, mean, worst, wrong_eye, gaps, doubles, clk.skew * 1e6,
         clk.period_us(), clk.rate_changes, clk.resets);
  bool ok = edges > expect * 0.99 && edges <= expect + 2 && wrong_eye == 0 && gaps == 0 &&
            doubles == 0 && worst - std::fabs(mean) < 60 && std::fabs(mean) < 60;
  return ok ? 0 : 1;
}
