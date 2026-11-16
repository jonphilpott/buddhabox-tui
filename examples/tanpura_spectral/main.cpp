/**
 * @file main.cpp
 * @brief A tanpura resynthesised from a recording: each string replays
 *        its measured harmonics, frame by frame, from pluck to pluck.
 *        Ported from buddhabox-framework's examples/tanpura_spectral.
 *
 * examples/tanpura models the strings' physics. This one doesn't model
 * anything: tools/analyse_tanpura.cpp measured a real tanpura, from a
 * vintage tape of raga Malkauns, with FFTs, and spectra.h holds the
 * result. For each of the three kinds of string (low sa, the two SAs and
 * the first string) it has 16 frames of 32 harmonic levels, spread over
 * the time from one pluck to the next.
 *
 * FROM SPECTRA TO WAVETABLES: the tool also turns each frame's 32
 * harmonic levels into one cycle of the waveform they make (an inverse
 * Fourier transform, done once on the computer), so spectra.h holds 48
 * single-cycle waveforms. Playing 32 harmonics live with
 * HarmonicOscillator measured about 500 cycles a sample per string on
 * the board, too much for four strings and a reverb; reading two
 * waveforms and blending them is a few dozen.
 *
 * HOW IT PLAYS: a pluck starts a string at frame 0; every 32 samples it
 * moves along the frames, blending between neighbours, so the frames
 * last exactly one cycle of the pattern whatever the tempo. On the tape
 * a cycle took about 2.1 seconds. Slower tempos stretch the same
 * evolution out.
 *
 * WHAT THE RECORDING SHOWED:
 * - The fundamentals hardly change from pluck to pluck: a tanpura rings
 *   until it is plucked again.
 * - The pluck refreshes the upper harmonics. On the low sa the jawari
 *   then blooms: the share of its power above the 8th harmonic climbs
 *   from a third to nearly three-quarters over 0.4 s, and fades to under
 *   a tenth by the next pluck. The SAs bloom less, about 0.65 s after
 *   their pluck.
 * - The recording's first string was ma (raga Malkauns has no Pa). Its
 *   spectrum is used for pa, ni and dha too.
 *
 * THE CYCLE, as in examples/tanpura:
 *   5 beats   pa  SA  SA  sa  -
 *   6 beats   pa  -   SA  SA  sa  -
 *
 * spectra.h is copied from the framework. The tool that made it lives
 * there too; regenerate it there and copy it across.
 *
 * THE PANEL:
 *   up/down, left/right  the menu: every setting
 *   knobs 0-7 (a/z to k/,)
 *       Volume, Tempo, Bright, Fine, Reverb, Spread, Space, Tone: each
 *       moves its menu item, and the menu moves it
 *
 * CPU: 54% of the F411 (the reverb is about 27%). On a desktop it is a
 * few percent: compare the panel's CPU meter.
 */

#include "bbtui/menu.h"
#include "bbtui/tui.h"

#include "buddhabox/core/dsp_common.h"
#include "buddhabox/core/midi_utils.h"

#include "buddhabox/fx/reverb.h"

#include "spectra.h"

using namespace bb;

// ── Settings ─────────────────────────────────────────────────────────
// Samples between steps along the frames.
const int kControlEvery = 32;
// The four strings into the reverb. Set on the board with the default
// settings: peaks of about 0.6-0.75 at full Volume.
const float kMixGain = 0.62f;
// The two SA strings are never exactly in tune with each other; this
// many Hz apart they beat slowly. Identical, they'd be one string.
const float kSaBeatHz = 0.25f;

/**
 * One string, played back from its measured waveforms.
 *
 * A wavetable oscillator with two tables at once: frame f and frame
 * f + 1, blended by how far the string has moved between them.
 *
 * Example:
 *   SpectralString s;
 *   s.setFrequency(146.8f);
 *   s.pluck(kWaves[1]);                     // an SA string
 *   s.step(16.0f * 32 / cycleSamples);      // every 32 samples
 *   float out = s.process();
 */
class SpectralString {
public:
  /** @param hz  Pitch in Hz (one float write: safe from loop()) */
  void setFrequency(float hz) { increment_ = hz / SAMPLE_RATE; }

  /**
   * Start from the pluck frame.
   * @param frames  kWaves[string]: 16 frames of one cycle each
   */
  void pluck(const float (*frames)[kWaveSize + 1]) {
    frames_ = frames;
    pos_ = 0.0f;
    step(0.0f);
  }

  /**
   * Move along the frames. Control rate.
   * @param frames  How many frames to advance (16 per cycle)
   */
  void step(float frames) {
    // Hold the last frame if the next pluck is late (Tempo just slowed).
    const float last = kWaveFrames - 1;
    pos_ = minf(pos_ + frames, last);
    frame_ = (int)pos_;
    next_ = frame_ < last ? frame_ + 1 : frame_;
    blend_ = pos_ - frame_;
  }

  /** @return  The next sample (silent until the first pluck) */
  float process() {
    if (!frames_)
      return 0.0f;
    phase_ += increment_;
    if (phase_ >= 1.0f)
      phase_ -= 1.0f;
    // Position in the table, and the fraction between two samples.
    float x = phase_ * kWaveSize;
    int i = (int)x;
    float t = x - i;
    const float *a = frames_[frame_], *b = frames_[next_];
    float sa = a[i] + t * (a[i + 1] - a[i]); // sample 256 repeats 0
    float sb = b[i] + t * (b[i + 1] - b[i]);
    return sa + blend_ * (sb - sa);
  }

private:
  const float (*frames_)[kWaveSize + 1] = nullptr;
  float increment_ = 0.0f, phase_ = 0.0f, pos_ = 0.0f, blend_ = 0.0f;
  int frame_ = 0, next_ = 0;
};

// ── The menu ─────────────────────────────────────────────────────────
enum Item {
  VOLUME,
  ROOT,
  FINE,
  TUNING,
  PATTERN,
  TEMPO,
  BRIGHT,
  SPREAD,
  SPACE,
  TONE,
  REVERB,
  kItems
};

// Tunings: the first string's note, as a ratio to SA (see
// examples/tanpura); the others are always SA, SA and sa.
const char *const kTunings[] = {"pa SA SA sa", "ma SA SA sa", "ni SA SA sa",
                                "dha SA SA sa"};
const float kFirstString[] = {3.0f / 4, 2.0f / 3, 15.0f / 16, 5.0f / 6};

// Patterns: which string each beat plucks; -1 is a rest.
const char *const kPatternNames[] = {"5 beat", "6 beat"};
const int kPatternLength[] = {5, 6};
const int8_t kPatterns[2][6] = {
    {0, 1, 2, 3, -1, -1}, // pa SA SA sa -
    {0, -1, 1, 2, 3, -1}, // pa - SA SA sa -
};
// Which measured spectrum each string plays: first, SA, SA, sa.
const int kSpectrumOf[4] = {2, 1, 1, 0};

// Root: SA, C2 to C4 by name.
const int kLowNote = NOTE_C1 + 12, kHighNote = NOTE_C1 + 36; // C2-C4
const int kNotes = kHighNote - kLowNote + 1;                 // 25
char noteText[kNotes][4];
const char *noteNames[kNotes];

MenuItem items[kItems] = {
    {"Volume", 80, 0, 100, 2},
    {"Root", NOTE_C1 + 26, kLowNote, kHighNote, 1, noteNames}, // D3
    {"Fine", 0, -50, 50, 1, nullptr, " ct"},
    {"Tuning", 0, 0, 3, 1, kTunings},
    {"Pattern", 0, 0, 1, 1, kPatternNames},
    // The tape's tanpura ran at about 140 (a cycle every 2.1 s).
    {"Tempo", 48, 20, 160, 1, nullptr, " bpm"},
    {"Bright", 50, 0, 100, 2}, // 50 = as recorded (see applyMenu())
    {"Spread", 50, 0, 100, 2},
    {"Space", 70, 0, 100, 2}, // reverb decay
    {"Tone", 60, 0, 100, 2},  // reverb damping: 100 = brightest
    {"Reverb", 40, 0, 100, 2},
};
Menu menu(items, kItems);

// ── The knobs ────────────────────────────────────────────────────────
// Knob n drives menu item kKnobItems[n], as the board's eight pots do:
// change the table to move a setting to another knob.
const int kKnobItems[] = {VOLUME, TEMPO,  BRIGHT, FINE,
                          REVERB, SPREAD, SPACE,  TONE};
const int kKnobCount = sizeof(kKnobItems) / sizeof(kKnobItems[0]);
float lastKnob[kKnobCount]; // each knob's last reading, to spot moves

/** @return  Item `i`'s value as a 0-1 knob position. */
float knobPosition(int i) {
  const MenuItem &m = items[i];
  return (float)(m.value - m.min) / (float)(m.max - m.min);
}

// ── Modules ──────────────────────────────────────────────────────────
#define NUM_STRINGS (4)
SpectralString strings[NUM_STRINGS];
Reverb<> reverb;

// ── Patch settings, from the menu ────────────────────────────────────
float samplesPerBeat = SAMPLE_RATE;
int pattern = 0;
float volume = 0.8f;
// Bright: a tilt filter on the mix. A one-pole low-pass splits each
// channel at about 1.5 kHz; the output is the lows plus the highs
// times highGain (0 = dark, 1 = as recorded, 2 = about +6 dB above).
float highGain = 1.0f;
const float kSplit = 0.19f; // one-pole coefficient: about 1.5 kHz
float lowL = 0.0f, lowR = 0.0f;
// Spread: pa hard left, sa hard right, the SAs in the middle.
const float kPan[NUM_STRINGS] = {-1.0f, 0.0f, 0.0f, 1.0f};
float panL[NUM_STRINGS] = {1.0f, 1.0f, 1.0f, 1.0f};
float panR[NUM_STRINGS] = {1.0f, 1.0f, 1.0f, 1.0f};

// ── Audio-thread state ────────────────────────────────────────────
float beatCounter = 0.0f; // samples until the next beat
int beat = -1;            // position in the pattern
int controlCounter = 0;
volatile int lastPlucked = -1; // for the screen

/**
 * Read every menu value into the patch settings. Called from loop()
 * after any change; uses library maths (powf, sqrtf), so control rate
 * only.
 */
void applyMenu() {
  // 1. Pitches.
  float sa = midiToFreq((uint8_t)items[ROOT].value) *
             semitonesToRatio(items[FINE].value / 100.0f);
  float hz[NUM_STRINGS] = {sa * kFirstString[items[TUNING].value], sa,
                           sa + kSaBeatHz, sa * 0.5f};
  for (int i = 0; i < NUM_STRINGS; i++)
    strings[i].setFrequency(hz[i]);

  // 2. Bright: 0 to 100 sets highGain from 0 to 2.
  highGain = items[BRIGHT].value / 50.0f;

  // 3. Rhythm, spread and reverb, as in examples/tanpura.
  pattern = items[PATTERN].value;
  samplesPerBeat = 60.0f * SAMPLE_RATE / items[TEMPO].value;
  for (int i = 0; i < NUM_STRINGS; i++) {
    float p = kPan[i] * items[SPREAD].value / 100.0f;
    panL[i] = sqrtf(1.0f - p);
    panR[i] = sqrtf(1.0f + p);
  }
  reverb.setTime(0.5f + 0.48f * items[SPACE].value / 100.0f);
  reverb.setLp(0.3f + 0.7f * items[TONE].value / 100.0f);
  reverb.setAmount(items[REVERB].value / 100.0f);
  volume = items[VOLUME].value / 100.0f;
}

/**
 * Fill one block of audio (see the sine example for the basics).
 * @param in      Unused: no input
 * @param out     Interleaved stereo output
 * @param frames  Stereo frames to fill
 */
void audioCallback(const float *in, float *out, int frames) {
  for (int i = 0; i < frames; i++) {
    // 1. The beat clock: on each beat, pluck that beat's string.
    beatCounter -= 1.0f;
    if (beatCounter <= 0.0f) {
      beatCounter += samplesPerBeat;
      int p = pattern;
      beat = (beat + 1) % kPatternLength[p];
      int s = kPatterns[p][beat];
      if (s >= 0) {
        strings[s].pluck(kWaves[kSpectrumOf[s]]);
        lastPlucked = s;
      }
    }

    // 2. Every kControlEvery samples, move each string along its
    //    frames: 16 frames per cycle of the pattern.
    if (controlCounter == 0) {
      controlCounter = kControlEvery;
      float cycle = samplesPerBeat * kPatternLength[pattern];
      float frames = (float)kWaveFrames * kControlEvery / cycle;
      for (int s = 0; s < NUM_STRINGS; s++)
        strings[s].step(frames);
    }
    controlCounter--;

    // 3. The strings, panned.
    float left = 0.0f, right = 0.0f;
    for (int s = 0; s < NUM_STRINGS; s++) {
      float out = strings[s].process();
      left += out * panL[s];
      right += out * panR[s];
    }
    left *= kMixGain;
    right *= kMixGain;
    // Bright: lows plus highGain times the highs.
    lowL += kSplit * (left - lowL);
    lowR += kSplit * (right - lowR);
    left = lowL + highGain * (left - lowL);
    right = lowR + highGain * (right - lowR);

    // 4. Reverb, then a soft clip and Volume.
    reverb.process(left, right);
    out[2 * i] = softLimitClip(left) * volume;
    out[2 * i + 1] = softLimitClip(right) * volume;
  }
}

/** Title bar, the four strings (the one just plucked filled), menu. */
void drawScreen() {
  Oled &oled = tui::display();
  oled.rect(0, 0, 128, 8, true);
  int x = oled.text(2, 0, "TANPURA FFT ", false);
  oled.text(x, 0, noteNames[items[ROOT].value - kLowNote], false);
  int playing = lastPlucked;
  oled.rect(0, 8, 128, 8, true, false);
  for (int s = 0; s < NUM_STRINGS; s++)
    oled.rect(s * 32 + 2, 9, 28, 6, s == playing);
  menu.draw(oled, 16, 6, 50);
}

void setup() {
  tui::setTitle("TANPURA FFT");
  for (int k = 0; k < kKnobCount; k++) {
    tui::setKnob(k, items[kKnobItems[k]].name, knobPosition(kKnobItems[k]));
    lastKnob[k] = tui::knob(k);
  }

  static const char *const kNames[12] = {"C",  "C#", "D",  "D#", "E",  "F",
                                         "F#", "G",  "G#", "A",  "A#", "B"};
  for (int n = 0; n < kNotes; n++) {
    int midi = kLowNote + n;
    snprintf(noteText[n], sizeof(noteText[n]), "%s%d", kNames[midi % 12],
             midi / 12 - 1);
    noteNames[n] = noteText[n];
  }
  applyMenu();

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
    MenuItem &m = items[kKnobItems[k]];
    m.value = m.min + (int)(v * (m.max - m.min) + 0.5f);
    moved = true;
  }
  if (moved)
    applyMenu();

  // 2. Menu: hand it the arrow keys and the space bar. A change to a
  //    knob's item moves the knob too.
  if (menu.update(tui::menuMove(), tui::menuAdjust(), tui::menuPressed()) ==
      Menu::CHANGED) {
    int item = menu.selected();
    for (int k = 0; k < kKnobCount; k++) {
      if (kKnobItems[k] == item) {
        tui::setKnob(k, items[item].name, knobPosition(item));
        lastKnob[k] = tui::knob(k);
      }
    }
    applyMenu();
  }

  // 3. Display: 20 frames a second.
  static uint32_t lastFrame = 0;
  if (tui::millis() - lastFrame >= 50) {
    lastFrame = tui::millis();
    drawScreen();
  }
}
