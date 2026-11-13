/**
 * @file main.cpp
 * @brief A West Coast noise box: a folded, FM'd complex oscillator,
 *        a pulser-driven random sequence and a low-pass gate, played
 *        from 8 knobs and the menu. Ported from buddhabox-framework's
 *        examples/noisebox.
 *
 * Where examples/easel plays itself, this one is an instrument: the
 * Buchla Music Easel's layout, with the knobs on the panel and the
 * routing in the menu. It goes from a pinging bongo through a slowly
 * churning drone to a screaming, crushed mess.
 *
 *   Pulser ─┬─► sequence (1-8 steps, Random rewrites them) ─► pitch
 *           └─► LPG ping                                       │
 *                                                              ▼
 *   Mod osc (tri, saw, square or noise) ──FM, timbre──► Complex osc
 *        │                                  (sine + noise, folded, 2x)
 *        └── Mod Mix ──────────────────────────────┐       │
 *                                                  ▼       ▼
 *   Lorenz chaos ─► timbre, FM               Low-pass gate (ping/drone)
 *                                                   │
 *                                 crush ─► echo ─► spring ─► out
 *
 * THE KNOBS, 0-7 (keys a/z to k/,):
 * - Noise: white noise into the folder: grit at low settings, a roar
 *   at high ones.
 * - Timbre: how hard the wavefolder folds: round sine to buzzy.
 * - FM: how far the mod osc swings the complex osc's pitch.
 * - Mod Rate: the mod osc. In Ratio mode, 0.25x to 8x the pitch (a
 *   tone); in Free mode, 0.05 Hz (a slow wobble) to 2 kHz.
 * - Rate: the pulser, 4 s to 30 ms between pulses. At the fast end
 *   the gate pings at audio rate and buzzes.
 * - Decay: how long each ping rings. In Drone mode, the gate's level.
 * - Echo Mix: how loud the echo is (see the menu's Echo items).
 * - Space: the spring.
 * Random sits in the menu: each pulse, the chance of rewriting the step
 * about to play with a random note. 0 locks the loop, full is all
 * random (a "Turing machine").
 *
 * THE MENU (up/down to move, left/right to change) also holds:
 * - Volume: the main level, applied after everything else.
 * - Pitch: chromatic, C1 to C6 (32.7 Hz to 1 kHz), shown as a note
 *   name; the sequence plays on top of it, in the Colour's scale.
 * - Colour: that scale, one of 16 five-note scales from dark to bright.
 * - Mod Wave: Triangle, Saw, Square or Noise (random steps at the mod
 *   rate: Free and fast is static, slow is a random wobble).
 * - Mod Track: Ratio or Free (see Mod Rate).
 * - Mod>Timbre: the mod osc moves the folding too (the Easel's
 *   "timbre modulation").
 * - Mod Mix: the mod osc's own signal, added before the gate.
 * - Fold: Sine (smooth, bell-like) or Analog (rougher, vocal).
 * - Chaos, Chaos Rate: a Lorenz attractor wobbling Timbre and FM,
 *   from a slow drift to an audio-rate snarl.
 * - Crush: sample-rate reduction, 44 kHz down to about 700 Hz.
 * - Gate: Ping (each pulse strikes the gate) or Drone (always open).
 * - Steps: the sequence's length.
 * - Echo Time, Echo Mix, Feedback: a delay with darkening repeats,
 *   10 ms to 2 s (the board's stops at 370 ms).
 * - Randomize: a new sequence.
 * A knob and its menu item are the same setting: moving either moves
 * the other.
 *
 * THE PANEL: knobs 0-7 as above; switch 0 (1) Randomize; switch 1 (2)
 * Gate, lit for Ping; MIDI notes set Pitch.
 *
 * OVERSAMPLING: folding and FM make harmonics far above 22 kHz, which
 * fold back down as off-key whine. The complex oscillator runs at
 * 88.2 kHz with an 18 kHz low-pass before every second sample is kept,
 * as in examples/easel, where it bought 14-16 dB on high notes.
 *
 * WHY THE SPRING RUNS AT HALF RATE: at the full rate it measured 31%
 * of the CPU in this patch. Its all-passes reach 2 samples back
 * instead of 4, the same time, so the chirp falls in the same place.
 * But it does not sound the same: it is crunchier. At 22 kHz nothing
 * above 11 kHz fits, and the bright fold and noise above that, only
 * partly removed by the averaging in the callback, folds back down to
 * the wrong pitches inside the spring. Here that suits the patch. For
 * a clean spring, low-pass the input properly first (two Svfs at
 * 9 kHz, as the complex oscillator does) or run it at the full rate.
 *
 * The spring stays at half rate here too, though a desktop could
 * afford the full rate: it is part of this patch's sound.
 */

#include "bbtui/menu.h"
#include "bbtui/tui.h"
#include "buddhabox/core/dsp_common.h"
#include "buddhabox/core/midi_utils.h"
#include "buddhabox/core/random.h"
#include "buddhabox/dynamics/vactrol_lpg.h"
#include "buddhabox/filter/svf.h"
#include "buddhabox/fx/decimator.h"
#include "buddhabox/fx/delay.h"
#include "buddhabox/fx/spring_reverb.h"
#include "buddhabox/fx/table_wavefolder.h"
#include "buddhabox/mod/colour.h"
#include "buddhabox/mod/lorenz.h"
#include "buddhabox/mod/pulser.h"
#include "buddhabox/noise/clocked_noise.h"
#include "buddhabox/osc/basic.h"
#include "buddhabox/osc/sine.h"

using namespace bb;

// ── Settings ─────────────────────────────────────────────────────────
const int kMaxSteps = 8; // the longest sequence

// ── The menu ─────────────────────────────────────────────────────────
// Volume, Pitch and Colour first, since they're changed most from the
// menu; then the eight pot items, in panel order; then the rest.
enum Item {
  VOLUME,
  PITCH,
  COLOUR,
  NOISE,
  TIMBRE,
  FM,
  MOD_RATE,
  RATE,
  DECAY,
  RANDOM,
  SPACE,
  MOD_WAVE,
  MOD_TRACK,
  MOD_TIMBRE,
  MOD_MIX,
  FOLD,
  CHAOS,
  CHAOS_RATE,
  CRUSH,
  GATE,
  STEPS,
  ECHO_TIME,
  ECHO_MIX,
  FEEDBACK,
  RANDOMIZE, // an action, not a value
  kItems
};

// Pitch is a MIDI note, C1 to C6, shown by name. setup() writes the
// names ("C1", "C#1" ... "C6") into noteText and points noteNames at
// them, so the menu can show labels[value - min].
const int kLowNote = NOTE_C1, kHighNote = NOTE_C6;
const int kNotes = kHighNote - kLowNote + 1; // 61: five octaves
char noteText[kNotes][4];
const char *noteNames[kNotes];

const char *const kModWaves[] = {"Triangle", "Saw", "Square", "Noise"};
const char *const kTracks[] = {"Free", "Ratio"};
const char *const kFolds[] = {"Sine", "Analog"};
const char *const kGates[] = {"Drone", "Ping"};

MenuItem items[kItems] = {
    {"Volume", 70, 0, 100, 2},
    {"Pitch", NOTE_C3, kLowNote, kHighNote, 1, noteNames},
    {"Colour", 6, 0, 15, 1},
    {"Noise", 0, 0, 10, 2},
    {"Timbre", 30, 0, 100, 2},
    {"FM", 20, 0, 100, 2},
    {"Mod Rate", 50, 0, 100, 1},
    {"Rate", 50, 0, 100, 2},
    {"Decay", 40, 0, 100, 2},
    {"Random", 20, 0, 100, 2},
    {"Space", 30, 0, 100, 2},
    {"Mod Wave", 0, 0, 3, 1, kModWaves},
    {"Mod Track", 1, 0, 1, 1, kTracks},
    {"Mod>Timbre", 0, 0, 100, 2},
    {"Mod Mix", 0, 0, 100, 2},
    {"Fold", 0, 0, 1, 1, kFolds},
    {"Chaos", 0, 0, 100, 2},
    {"Chaos Rate", 40, 0, 100, 2},
    {"Crush", 0, 0, 100, 2},
    {"Gate", 1, 0, 1, 1, kGates},
    {"Steps", 5, 1, kMaxSteps, 1},
    {"Echo Time", 25, 1, 200, 1, nullptr, "0ms"}, // tens of ms
    {"Echo Mix", 0, 0, 100, 2},
    {"Feedback", 40, 0, 95, 2},
    {"Randomize"},
};
Menu menu(items, kItems);

// Where each item really is, 0 to 1, when a pot set it last: finer than
// the menu's whole-number steps, so a slow turn sweeps smoothly. -1 when
// the menu set it last.
float fine[kItems];

/** @return  Item `i`'s position, 0 to 1: the pot's, or the menu's. */
float position(int i) {
  if (fine[i] >= 0.0f)
    return fine[i];
  const MenuItem &m = items[i];
  return (float)(m.value - m.min) / (float)(m.max - m.min);
}

// ── The knobs ────────────────────────────────────────────────────────
// Knob n sets menu item kKnobItems[n].
const Item kKnobItems[] = {NOISE, TIMBRE, FM,       MOD_RATE,
                           RATE,  DECAY,  ECHO_MIX, SPACE};
const int kKnobCount = sizeof(kKnobItems) / sizeof(kKnobItems[0]);
float lastKnob[kKnobCount]; // each knob's last reading, to spot moves

// ── Modules ──────────────────────────────────────────────────────────
Pulser pulser;
BasicOscillator modOsc;
ClockedNoise modNoise;     // the mod osc's Noise wave
SineOscillator complexOsc; // runs at 2x: see setup()
TableWavefolder folder;
Svf antiAlias1, antiAlias2; // 18 kHz low-pass at 2x, before decimating
Random white;               // the Noise item
Lorenz chaos;
VactrolLpg lpg;
Decimator crush;
// 2 s of 16-bit storage: 172 KB. The board's held 0.37 s in 32 KB.
DelayLine<88200> echo;
// The spring runs at half rate (see the file header): 1024 samples
// hold its longer trip there, and a stretch of 2 keeps its chirp in
// place. Crunchier than the full-rate spring: see the header. 8 KB.
SpringReverb<1024, 2> spring;
float springPrevL = 0.0f, springPrevR = 0.0f; // its last two outputs
float springNowL = 0.0f, springNowR = 0.0f;
float springLastIn = 0.0f; // the previous sample's input
bool oddSample = false;
float space = 0.3f; // Space: the spring's dry/wet
Colour colour;
LFSR rng(208); // the Easel's sound source is the 208

// ── Settings for the audio interrupt, from applyMenu() ──────────────
// loop() writes these and the audio interrupt reads them. A float is
// written in one instruction, so the interrupt never sees half of one.
float volume = 0.5f;      // Volume, as a gain
float baseHz = 100.0f;    // Pitch
float timbre = 0.3f;      // Timbre
float fmDepth = 0.2f;     // FM
float modRatio = 1.0f;    // Mod Rate, Ratio mode
float modFreeHz = 1.0f;   // Mod Rate, Free mode
bool modTrack = true;     // Mod Track: true = Ratio
int modWave = 0;          // Mod Wave: 3 = Noise
float modTimbre = 0.0f;   // Mod>Timbre
float modMix = 0.0f;      // Mod Mix
float noiseAmount = 0.0f; // Noise
bool analogFold = false;  // Fold
float chaosAmount = 0.0f; // Chaos
bool crushOn = false;     // Crush above 0
bool ping = true;         // Gate
float droneLevel = 0.4f;  // Decay, in Drone mode
float randomChance = 0.2f;
int steps = 5;
float echoSamples = 11025.0f, echoMix = 0.0f, echoFeedback = 0.4f;

// ── The sequence ─────────────────────────────────────────────────────
// Scale indices into the Colour: 0-4 the first octave, 5-9 the second.
// The audio interrupt rewrites them (Random); loop() rolls them all
// (Randomize). Single bytes: each write is atomic.
uint8_t sequence[kMaxSteps];
volatile int step = 0;        // the step playing, for the display
volatile bool pulsed = false; // set on each pulse, for the display
float noteRatio = 1.0f;       // the step's interval above Pitch
float echoTone = 0.0f;        // the echo feedback low-pass's state

/** Roll a new sequence over two octaves of the scale. */
void randomize() {
  for (int i = 0; i < kMaxSteps; i++)
    sequence[i] = (uint8_t)rng.nextRange(0, 9);
}

/**
 * Turn the menu's numbers into the voice's settings. Called from loop()
 * after any change; uses library maths, so control rate only.
 */
void applyMenu() {
  // 1. The panel. Exponential where the ear hears ratios (pitch, rates),
  //    linear where it hears amounts.
  // Volume, squared: the ear hears level roughly on a log scale, and
  // a square is close enough that the bottom half of the range isn't
  // all "nearly full". 70 is about -6 dB.
  float v = position(VOLUME);
  volume = 0.92f * v * v;
  // Pitch (menu only): a whole note, so it always lands on a semitone.
  baseHz = midiToFreq((uint8_t)items[PITCH].value); // powf: control rate
  noiseAmount = position(NOISE);
  timbre = position(TIMBRE);
  fmDepth = 0.95f * position(FM); // under 1: the pitch never goes negative
  modRatio = 0.25f * fastExp2(5.0f * position(MOD_RATE));     // 0.25x - 8x
  modFreeHz = 0.05f * fastExp2(15.3f * position(MOD_RATE));   // to 2 kHz
  pulser.setPeriod(4.0f * fastExp2(-7.06f * position(RATE))); // 4 s - 30 ms
  lpg.setDecay(position(DECAY));
  droneLevel = position(DECAY);
  randomChance = position(RANDOM);
  space = position(SPACE);

  // 2. The menu-only items.
  modWave = items[MOD_WAVE].value;
  const BasicOscillator::Shape shapes[] = {
      BasicOscillator::TRIANGLE, BasicOscillator::SAW, BasicOscillator::SQUARE};
  if (modWave < 3)
    modOsc.setShape(shapes[modWave]);
  modTrack = items[MOD_TRACK].value;
  modTimbre = 0.5f * position(MOD_TIMBRE);
  modMix = 0.6f * position(MOD_MIX);
  analogFold = items[FOLD].value;
  chaosAmount = position(CHAOS);
  chaos.setRate(0.3f + 0.7f * position(CHAOS_RATE)); // drift to snarl
  // Crush: off at 0, then 44 kHz down to about 700 Hz (six octaves).
  float c = position(CRUSH);
  crushOn = c > 0.0f;
  crush.setRate(SAMPLE_RATE * fastExp2(-6.0f * c));
  ping = items[GATE].value;
  steps = items[STEPS].value;
  colour.setColour(COLOUR_BY_BRIGHTNESS[items[COLOUR].value]);
  echoSamples = minf(items[ECHO_TIME].value * 0.01f * SAMPLE_RATE, 88198.0f);
  echoMix = position(ECHO_MIX);
  echoFeedback = items[FEEDBACK].value / 100.0f;
}

/**
 * One pulse (audio interrupt): maybe rewrite the next step, play it,
 * and strike the gate.
 */
void onPulse() {
  int s = (step + 1) % steps;
  // Random: the Turing machine's lock. At 0 the loop repeats forever;
  // turning it up lets new notes in.
  if (rng.nextFloat() < randomChance)
    sequence[s] = (uint8_t)rng.nextRange(0, 9);
  step = s;
  // The Colour's root is 1 Hz (setup()), so getNote() is the ratio.
  noteRatio = colour.getNote(0, sequence[s]);
  if (ping)
    lpg.trigger();
  pulsed = true;
}

/**
 * The complex oscillator at twice the sample rate: two folded samples
 * (sine plus noise), low-passed at 18 kHz, keeping only the second. The
 * low-pass is what makes dropping every other sample safe.
 *
 * @param frequency  Cycles per sample at the 2x rate (Hz / 88200)
 * @param noise      Added to the sine before folding
 */
float complexOscillator(float frequency, float noise) {
  float out = 0.0f;
  for (int k = 0; k < 2; k++) {
    float folded = folder.process(complexOsc.next(frequency) + noise);
    if (analogFold)
      folded = folder.analog(); // the same sample, the rougher curve
    out = antiAlias2.process(antiAlias1.process(folded).lp).lp;
  }
  return out;
}

/** Fill one block of audio (audio thread: no printing, no waiting). */
void audioCallback(const float *in, float *out, int frames) {
  const float oversampledTime = 1.0f / (2.0f * SAMPLE_RATE);

  for (int i = 0; i < frames; i++) {
    // 1. Timing.
    if (pulser.process())
      onPulse();

    // 2. Chaos: two of the Lorenz outputs, scaled by the Chaos item.
    chaos.process();
    float chaosTimbre = 0.5f * chaosAmount * chaos.x();
    float chaosFm = 0.5f * chaosAmount * chaos.y();

    // 3. Modulation oscillator: following the pitch (Ratio) or on its
    //    own (Free). The wave's frequency is capped at 0.25 of the
    //    sample rate, as BasicOscillator asks.
    float hz = baseHz * noteRatio;
    float modHz = modTrack ? hz * modRatio : modFreeHz;
    float mod;
    if (modWave == 3) {
      modNoise.setFrequency(modHz);
      mod = modNoise.process();
    } else {
      mod = modOsc.process(minf(modHz / SAMPLE_RATE, 0.25f), 0.5f);
    }

    // 4. Complex oscillator. FM swings the pitch by up to 95% either
    //    way, so it never runs backwards; chaos and the mod osc move
    //    the folding.
    float depth = clampf(fmDepth + chaosFm, 0.0f, 0.95f);
    folder.setAmount(timbre + modTimbre * mod + chaosTimbre);
    float noise = noiseAmount * white.bipolar();
    float x =
        complexOscillator(hz * (1.0f + depth * mod) * oversampledTime, noise);
    x += modMix * mod;

    // 5. Low-pass gate: struck by the pulser, or held open by Decay.
    x = ping ? lpg.process(x) : lpg.process(x, droneLevel);

    // 6. Crush, then the echo. Its repeats go back in through a
    //    low-pass, so each is darker than the last.
    if (crushOn) // skipped at 0: it costs about 5% even passing through
      x = crush.process(x);
    float wet = echo.read(echoSamples);
    echoTone += 0.35f * (wet - echoTone);
    echo.write(0.7f * x + echoFeedback * echoTone);
    x += echoMix * wet;

    // 7. The spring, at half rate. Each odd sample feeds it the
    //    average of the last two inputs (a weak low-pass: some of the
    //    treble still folds back, hence the crunch) and takes a new pair out.
    //    The output between is a straight line through the last two: the
    //    midpoint on even samples, the newest on odd ones.
    x *= 0.7f;
    if (oddSample) {
      springPrevL = springNowL;
      springPrevR = springNowR;
      springNowL = springNowR = 0.5f * (x + springLastIn);
      spring.process(springNowL, springNowR); // fully wet: setup()
    }
    springLastIn = x;
    float wetL = oddSample ? springNowL : 0.5f * (springPrevL + springNowL);
    float wetR = oddSample ? springNowR : 0.5f * (springPrevR + springNowR);
    oddSample = !oddSample;

    // 8. Dry/wet, then a soft limit to round off what goes past full
    //    scale. The spring spreads the mono voice into stereo.
    float left = x + space * (wetL - x);
    float right = x + space * (wetR - x);
    //    Volume comes after the limit, so turning it down only makes
    //    it quieter, never cleaner or dirtier.
    out[2 * i] = softLimitClip(left) * volume;
    out[2 * i + 1] = softLimitClip(right) * volume;
  }
}

/**
 * Draw one frame. The yellow top band (rows 0-15 on two-colour panels)
 * holds the title and the sequence; the blue rest holds the menu.
 */
void drawScreen() {
  Oled &oled = tui::display();
  // 1. Title bar, dark on lit, with a box that flashes on each pulse.
  oled.rect(0, 0, 128, 8, true);
  oled.text(2, 0, "NOISEBOX", false);
  oled.text(64, 0, kGates[items[GATE].value], false);
  oled.rect(118, 1, 6, 6, true, !pulsed);
  pulsed = false;

  // 2. The sequence: a bar per step, as tall as its note, with a line
  //    under the step playing.
  oled.rect(0, 8, 128, 8, true, false);
  int playing = step;
  for (int i = 0; i < steps; i++) {
    int h = 1 + sequence[i] / 2; // 1 to 5 pixels
    oled.rect(i * 16 + 2, 14 - h, 12, h, true);
    if (i == playing)
      oled.rect(i * 16 + 2, 15, 12, 1, true);
  }

  // 3. The menu: six rows under the yellow band.
  menu.draw(oled, 16, 6);
}

void setup() {
  tui::setTitle("NOISEBOX");
  tui::setSwitch(0, "Rndmz", true);
  tui::setSwitch(1, "Ping");

  // 1. The complex oscillator and its anti-alias filters run at
  //    88.2 kHz. Two 2-pole low-passes with these Qs make a 4-pole
  //    Butterworth: flat to the cutoff, then 24 dB per octave down.
  complexOsc.init(2.0f * SAMPLE_RATE);
  antiAlias1.init(2.0f * SAMPLE_RATE);
  antiAlias2.init(2.0f * SAMPLE_RATE);
  antiAlias1.setFrequency(18000.0f, 0.5412f);
  antiAlias2.setFrequency(18000.0f, 1.3066f);

  // 2. The rest of the voice.
  lpg.setColour(0.6f);          // between VCA and filter: the bongo
  spring.init(SAMPLE_RATE / 2); // half rate: see the file header
  spring.setAmount(1.0f);       // wet only: the mix is in the callback
  spring.setTime(0.7f);
  spring.setTone(0.45f);
  colour.setRoot(1.0f); // so getNote() returns a ratio over Pitch
  static const char *const kNames[12] = {"C",  "C#", "D",  "D#", "E",  "F",
                                         "F#", "G",  "G#", "A",  "A#", "B"};
  for (int n = 0; n < kNotes; n++) {
    int midi = kLowNote + n;
    // MIDI 24 is C1: octave = note / 12 - 1.
    snprintf(noteText[n], sizeof(noteText[n]), "%s%d", kNames[midi % 12],
             midi / 12 - 1);
    noteNames[n] = noteText[n];
  }
  for (int i = 0; i < kItems; i++)
    fine[i] = -1.0f;
  for (int k = 0; k < kKnobCount; k++) {
    tui::setKnob(k, items[kKnobItems[k]].name, position(kKnobItems[k]));
    lastKnob[k] = tui::knob(k);
  }
  tui::setSwitchOn(1, items[GATE].value);
  randomize();
  applyMenu();

  Midi::begin();
  AudioEngine::begin();
}

void loop() {
  // 1. Knobs. A knob that moved sets its menu item, so the screen shows
  //    where it is.
  bool moved = false;
  for (int k = 0; k < kKnobCount; k++) {
    float v = tui::knob(k);
    if (v == lastKnob[k])
      continue;
    lastKnob[k] = v;
    int item = kKnobItems[k];
    MenuItem &m = items[item];
    m.value = m.min + (int)(v * (m.max - m.min) + 0.5f);
    fine[item] = v;
    moved = true;
  }

  // 2. Switches: Randomize, and Gate (lit = Ping).
  if (tui::switchPressed(0))
    randomize();
  if (tui::switchOn(1) != (bool)items[GATE].value) {
    items[GATE].value = tui::switchOn(1);
    moved = true;
  }

  // 3. MIDI: a note in range sets Pitch.
  Midi::Message msg;
  while (Midi::read(msg)) {
    if (msg.isNoteOn() && msg.data1 >= kLowNote && msg.data1 <= kHighNote) {
      items[PITCH].value = msg.data1;
      moved = true;
    }
  }
  if (moved)
    applyMenu();

  // 4. Menu: hand it the arrow keys and the space bar. A change to a
  //    knob's or switch's item moves that too.
  Menu::Event event =
      menu.update(tui::menuMove(), tui::menuAdjust(), tui::menuPressed());
  if (event == Menu::CHANGED) {
    int item = menu.selected();
    fine[item] = -1.0f; // the menu has this item now
    for (int k = 0; k < kKnobCount; k++) {
      if (kKnobItems[k] == item) {
        tui::setKnob(k, items[item].name, position(item));
        lastKnob[k] = tui::knob(k);
      }
    }
    tui::setSwitchOn(1, items[GATE].value);
    applyMenu();
  } else if (event == Menu::ACTION && menu.selected() == RANDOMIZE) {
    randomize();
  }

  // 5. Display: redraw at 20 frames a second.
  static uint32_t lastFrame = 0;
  if (tui::millis() - lastFrame >= 50) {
    lastFrame = tui::millis();
    drawScreen();
  }
}
