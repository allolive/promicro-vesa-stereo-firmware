// promicro-vesa-stereo: a VESA stereo sync signal for 3D emitters (the 3-pin
// mini-DIN, or a 3.5 mm mono sync input such as Hi-Shock's), locked to the
// vsyncs a host reports over USB serial.
//
// Board: ATmega32U4 at 5 V / 16 MHz (Arduino Micro, Pro Micro 5V, Leonardo).
// Wiring: pin 9 (OC1A) -> sync (3.5 mm tip, high = left eye) through
//         220 ohm; GND -> ground (sleeve); VCC -> +5 V, which powers the
//         emitter. docs/GUIDE.md shows the mini-DIN's pins by position.
//         Optional: a P-channel MOSFET on the emitter's +5 V, gate on pin 8
//         (low = on) with 10 kohm to VCC. Pin 8 is driven low from start-up,
//         so a board without it behaves the same.
//
// The box sends one line per vsync over USB serial: "S <vsync> <us> <eye>",
// with the vsync's CLOCK_MONOTONIC time in microseconds (mod 2^32) and the eye
// the video layer shows for it. USB delivery jitters by up to a millisecond,
// so the edges are not driven from the messages. The vsync times and the
// frame period are the box's own (exact); only the box clock's offset and
// drift against our crystal are fitted, from the earliest-arriving messages
// (their delay is the floor of the path). Timer 1's compare unit then switches
// the pin in hardware at the vsync plus a tunable delay. A box that goes
// quiet is coasted through on the last vsync and the exact period, for up to
// 2 s; then the glasses stop.
//
// Commands (one per line):
//   S <vsync> <us> <L|R|->  sync message from the box
//   M <0|1|2>               mode: 0 off, 1 free-run, 2 follow the box
//   F <millihertz>          free-run edge rate (96000 = 96 Hz = 48 Hz per eye)
//   D <microseconds>        delay from vsync to the eye switch, may be negative
//   I <0|1>                 invert the eye (swap left and right)
//   E <microseconds>        move only the switches into the right eye, so the
//                           high (left) part of the wave is longer or shorter:
//                           a probe of whether the emitter reads both edges
//   P <0|1>                 the emitter's power through the optional MOSFET
//                           (pin 8): 1 on, 0 off. On at every start-up, never
//                           saved; nothing changes on a board without one
//   W                       save mode, rate, delay and invert to EEPROM
//   ?                       status line (starts with the firmware version)

#include <EEPROM.h>
#include "sync_core.h"

static const uint8_t SYNC_PIN = 9;               // PB5 / OC1A
static const uint8_t POWER_PIN = 8;              // PB4: optional MOSFET gate, low = on
static const uint32_t TICKS_PER_S = stereosync::TICKS_PER_S;   // 16 MHz / 8
static const uint32_t LOST_TICKS = 2 * TICKS_PER_S;       // 2 s: stop the glasses
static const uint16_t EE_MAGIC = 0x3d5a;
#define FIRMWARE_VERSION "0.2.0"

enum Mode : uint8_t { MODE_OFF = 0, MODE_FREE = 1, MODE_SYNC = 2 };

struct Settings {
  uint16_t magic;
  uint8_t mode;
  uint8_t invert;
  int32_t delay_us;
  uint32_t free_mhz;
};

static Settings cfg;

// ---- timer 1 as a 32-bit clock --------------------------------------------

static volatile uint16_t ovf_hi;
static volatile bool edge_pending;

ISR(TIMER1_OVF_vect) { ovf_hi++; }

ISR(TIMER1_COMPA_vect)
{
  TIMSK1 &= ~_BV(OCIE1A);
  edge_pending = false;
}

static uint32_t now_ticks()
{
  uint8_t s = SREG;
  cli();
  uint16_t lo = TCNT1;
  uint16_t hi = ovf_hi;
  if ((TIFR1 & _BV(TOV1)) && lo < 0x8000)
    hi++;
  SREG = s;
  return ((uint32_t)hi << 16) | lo;
}

static void output_idle()
{
  uint8_t s = SREG;
  cli();
  TIMSK1 &= ~_BV(OCIE1A);
  TCCR1A = _BV(COM1A1);          // clear on compare...
  TCCR1C = _BV(FOC1A);           // ...forced now, so OC1A's latch is low too
  TCCR1A = 0;                    // disconnect OC1A, pin back to PORTB
  edge_pending = false;
  SREG = s;
  digitalWrite(SYNC_PIN, LOW);
}

// Switch the pin to 'high' at 'when'. Refuses an edge no longer far enough
// ahead (the loop was late): the 16-bit compare would fire a wrap later.
static bool arm_edge(uint32_t when, bool high)
{
  uint8_t s = SREG;
  cli();
  if ((int32_t)(when - now_ticks()) <= stereosync::ARM_MARGIN / 2) {
    SREG = s;
    return false;
  }
  TCCR1A = high ? (_BV(COM1A1) | _BV(COM1A0)) : _BV(COM1A1);
  OCR1A = (uint16_t)when;
  TIFR1 = _BV(OCF1A);
  TIMSK1 |= _BV(OCIE1A);
  edge_pending = true;
  SREG = s;
  return true;
}

// ---- schedule and clock mapping (sync_core.h) ----------------------------

static stereosync::Schedule sched;
static stereosync::ClockMap clk;

static void schedule()
{
  if (edge_pending)
    return;
  uint32_t when;
  bool left;
  if (sched.next(now_ticks(), cfg.mode == MODE_FREE, &when, &left))
    arm_edge(when, left ^ (cfg.invert != 0));
}

static void on_sync(uint32_t vsync, uint32_t us, char eye, uint32_t arrival)
{
  uint32_t vsync_t, period_q8;
  if (!clk.on_sync(vsync, us, arrival, LOST_TICKS, &vsync_t, &period_q8))
    return;
  if (cfg.mode != MODE_SYNC || eye == '-')
    return;
  sched.anchor(vsync_t + (uint32_t)(cfg.delay_us * (int32_t)stereosync::TICKS_PER_US),
               eye == 'L', period_q8);
}

// ---- settings and commands ------------------------------------------------

static void load_settings()
{
  EEPROM.get(0, cfg);
  if (cfg.magic != EE_MAGIC) {
    cfg.magic = EE_MAGIC;
    cfg.mode = MODE_SYNC;
    cfg.invert = 0;
    cfg.delay_us = 0;
    cfg.free_mhz = 96000;
  }
}

// Back to no picture and no clock: a new session maps the box clock afresh.
static void restart()
{
  output_idle();
  int32_t right_extra = sched.right_extra;
  sched = stereosync::Schedule();
  sched.right_extra = right_extra;
  clk.reset();
}

static void apply_mode()
{
  restart();
  if (cfg.mode == MODE_FREE) {
    sched.set_free_rate(cfg.free_mhz);
    sched.anchor(now_ticks() + 1000, true, sched.period_q8);
  }
}

static void status()
{
  Serial.print(F("promicro-vesa-stereo " FIRMWARE_VERSION " mode "));
  Serial.print(cfg.mode);
  Serial.print(F(" delay_us "));
  Serial.print(cfg.delay_us);
  Serial.print(F(" invert "));
  Serial.print(cfg.invert);
  Serial.print(F(" right_extra_us "));
  Serial.print(sched.right_extra / (int32_t)stereosync::TICKS_PER_US);
  Serial.print(F(" free_mhz "));
  Serial.print(cfg.free_mhz);
  Serial.print(F(" period_us "));
  Serial.print(clk.period_us(), 3);
  Serial.print(F(" rate_changes "));
  Serial.print(clk.rate_changes);
  Serial.print(F(" skew_ppm "));
  Serial.print(clk.skew * 1e6f, 1);
  Serial.print(F(" fit_windows "));
  Serial.print(clk.win_n);
  Serial.print(F(" msgs "));
  Serial.print(clk.msgs);
  Serial.print(F(" resets "));
  Serial.print(clk.resets);
  Serial.print(F(" last_resid_us "));
  Serial.print(clk.last_resid / 2);
  Serial.print(F(" running "));
  Serial.print(sched.have_anchor ? 1 : 0);
  Serial.print(F(" power "));
  Serial.println(digitalRead(POWER_PIN) == LOW ? 1 : 0);
}

static char line[48];
static uint8_t line_len;
static uint32_t line_t;

static void handle_line()
{
  char *p = line;
  char cmd = *p++;
  switch (cmd) {
  case 'S': {
    char *e;
    uint32_t vsync = strtoul(p, &e, 10);
    uint32_t us = strtoul(e, &e, 10);
    while (*e == ' ')
      e++;
    if (*e == 'L' || *e == 'R' || *e == '-')
      on_sync(vsync, us, *e, line_t);
    break;
  }
  case 'M':
    cfg.mode = (uint8_t)constrain(atoi(p), 0, 2);
    apply_mode();
    break;
  case 'F':
    cfg.free_mhz = strtoul(p, NULL, 10);
    if (cfg.mode == MODE_FREE)
      sched.set_free_rate(cfg.free_mhz);
    break;
  case 'D':
    cfg.delay_us = strtol(p, NULL, 10);
    break;
  case 'I':
    cfg.invert = atoi(p) ? 1 : 0;
    break;
  case 'E':                      // the schedule clamps it to the current rate
    sched.right_extra = (int32_t)strtol(p, NULL, 10) * (int32_t)stereosync::TICKS_PER_US;
    break;
  case 'P':
    digitalWrite(POWER_PIN, atoi(p) ? LOW : HIGH);
    break;
  case 'W':
    EEPROM.put(0, cfg);
    Serial.println(F("saved"));
    break;
  case '?':
    status();
    break;
  default:
    break;
  }
}

void setup()
{
  // the emitter on as early as possible: the 10 kohm holds it off until here
  digitalWrite(POWER_PIN, LOW);
  pinMode(POWER_PIN, OUTPUT);
  pinMode(SYNC_PIN, OUTPUT);
  digitalWrite(SYNC_PIN, LOW);
  TCCR1A = 0;
  TCCR1B = _BV(CS11);            // normal mode, clk/8
  TCNT1 = 0;
  TIFR1 = 0xff;
  TIMSK1 = _BV(TOIE1);
  Serial.begin(115200);
  load_settings();
  apply_mode();
}

void loop()
{
  while (Serial.available()) {
    int c = Serial.read();
    if (line_len == 0)
      line_t = now_ticks();      // arrival of the line's first byte
    if (c == '\n' || c == '\r') {
      if (line_len) {
        line[line_len] = 0;
        handle_line();
      }
      line_len = 0;
    } else if (line_len < sizeof(line) - 1) {
      line[line_len++] = (char)c;
    }
  }

  // A late box coasts on the last anchor; a silent one stops the glasses.
  if (cfg.mode == MODE_SYNC && clk.have_last_msg &&
      now_ticks() - clk.last_msg_t > LOST_TICKS)
    restart();

  if (cfg.mode == MODE_OFF)
    return;
  schedule();
}
