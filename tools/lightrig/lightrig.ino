// Light rig: times light edges on up to four light sensors, for measuring
// shutter glasses and a display's drawing order.
//
// Board: ATmega32U4 at 5 V / 16 MHz (Pro Micro 5V, Arduino Micro, Leonardo).
// Sensors: TEMT6000 modules (VCC, GND, analog OUT) on A0..A3, e.g.
//   A0 screen top, A1 screen bottom, A2 behind the left lens, A3 behind the right.
//
// The ADC runs free over the four inputs (about 13 us a conversion, so each
// input is seen every ~55 us). Each input keeps a running minimum and maximum
// and reports crossings of their midpoint, with hysteresis, timestamped on
// timer 1 (0.5 us ticks, 32-bit, hexadecimal so printing stays cheap):
//   E <ch> <1 rising|0 falling> <ticks>
// Edges are queued and printed between sampling passes, never inside one.
// Commands (one per line):
//   ?            status: per input min/max and edge count
//   H <percent>  hysteresis as % of the input's range (default 20)
//   R <ch> <n>   raw capture: n samples (<= 900) of one input, then
//                "R <ch> <us_per_sample> v v v ..." in one line
//   M <0|1>      edge messages off/on (default on)

static const uint8_t NCH = 4;
static const uint8_t mux[NCH] = {7, 6, 5, 4};   // A0..A3 on the 32U4 are ADC7..ADC4

static volatile uint16_t ovf_hi;
ISR(TIMER1_OVF_vect) { ovf_hi++; }

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

struct Chan {
  uint8_t lo = 255, hi = 0;
  bool state = false;
  uint32_t edges = 0;
  uint16_t decay = 0;
};
static Chan ch[NCH];
static uint8_t hyst_pct = 20;
static bool report = true;

struct Edge {
  uint32_t t;
  uint8_t ch_state;               // channel << 1 | rising
};
static Edge queue[32];
static volatile uint8_t q_head, q_tail;

static uint8_t read_adc(uint8_t c)
{
  ADMUX = _BV(REFS0) | _BV(ADLAR) | (mux[c] & 0x07);   // AVcc ref, left-adjust: 8 bits in ADCH
  ADCSRB = 0;                                           // MUX5 = 0 for ADC4..7
  ADCSRA |= _BV(ADSC);
  while (ADCSRA & _BV(ADSC))
    ;
  return ADCH;
}

static void sample(uint8_t c)
{
  uint32_t t = now_ticks();
  uint8_t v = read_adc(c);
  Chan &k = ch[c];
  // slow decay of the envelope so the levels follow a changed scene
  if (++k.decay >= 2000) {
    k.decay = 0;
    if (k.hi > k.lo + 2) {
      k.hi--;
      k.lo++;
    }
  }
  if (v > k.hi)
    k.hi = v;
  if (v < k.lo)
    k.lo = v;
  int range = (int)k.hi - (int)k.lo;
  if (range < 8)
    return;                       // nothing switching on this input
  int mid = (k.hi + k.lo) / 2;
  int h = range * hyst_pct / 200;
  bool s = k.state;
  if (!s && v > mid + h)
    s = true;
  else if (s && v < mid - h)
    s = false;
  if (s != k.state) {
    k.state = s;
    k.edges++;
    uint8_t next = (q_head + 1) % 32;
    if (report && next != q_tail) {
      queue[q_head].t = t;
      queue[q_head].ch_state = (c << 1) | (s ? 1 : 0);
      q_head = next;
    }
  }
}

static void raw_capture(uint8_t c, uint16_t n)
{
  static uint8_t buf[900];
  if (n > sizeof(buf))
    n = sizeof(buf);
  uint32_t t0 = now_ticks();
  for (uint16_t i = 0; i < n; i++)
    buf[i] = read_adc(c);
  uint32_t t1 = now_ticks();
  Serial.print(F("R "));
  Serial.print(c);
  Serial.print(' ');
  Serial.print(((t1 - t0) / 2.0f) / n, 2);
  for (uint16_t i = 0; i < n; i++) {
    Serial.print(' ');
    Serial.print(buf[i]);
  }
  Serial.println();
}

static char line[24];
static uint8_t len;

static void command()
{
  char *p = line + 1;
  switch (line[0]) {
  case '?':
    for (uint8_t c = 0; c < NCH; c++) {
      Serial.print(F("ch "));
      Serial.print(c);
      Serial.print(F(" lo "));
      Serial.print(ch[c].lo);
      Serial.print(F(" hi "));
      Serial.print(ch[c].hi);
      Serial.print(F(" edges "));
      Serial.println(ch[c].edges);
    }
    break;
  case 'H':
    hyst_pct = (uint8_t)constrain(atoi(p), 1, 90);
    break;
  case 'M':
    report = atoi(p) != 0;
    break;
  case 'R': {
    char *e;
    long c = strtol(p, &e, 10);
    long n = strtol(e, NULL, 10);
    if (c >= 0 && c < NCH && n > 0)
      raw_capture((uint8_t)c, (uint16_t)n);
    break;
  }
  default:
    break;
  }
}

void setup()
{
  TCCR1A = 0;
  TCCR1B = _BV(CS11);             // clk/8: 0.5 us ticks
  TIMSK1 = _BV(TOIE1);
  ADCSRA = _BV(ADEN) | _BV(ADPS2);  // clk/16: ~13 us a conversion, 8-bit use
  DIDR0 = 0xf0;                   // digital inputs off on ADC4..7
  Serial.begin(115200);
}

static void flush_edges()
{
  while (q_tail != q_head) {
    const Edge &e = queue[q_tail];
    Serial.print(F("E "));
    Serial.print(e.ch_state >> 1);
    Serial.print(' ');
    Serial.print(e.ch_state & 1);
    Serial.print(' ');
    Serial.println(e.t, HEX);
    q_tail = (q_tail + 1) % 32;
  }
}

void loop()
{
  for (uint8_t c = 0; c < NCH; c++)
    sample(c);
  flush_edges();
  while (Serial.available()) {
    int c = Serial.read();
    if (c == '\n' || c == '\r') {
      if (len) {
        line[len] = 0;
        command();
      }
      len = 0;
    } else if (len < sizeof(line) - 1) {
      line[len++] = (char)c;
    }
  }
}
