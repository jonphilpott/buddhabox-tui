/**
 * @file main.cpp
 * @brief Singing bowls: struck resonators, chaotic drift and reverb.
 *        Ported from buddhabox-framework's examples/singing_bowls.
 *
 * A generative ambient patch built from the Mutable Instruments ports:
 *
 *   Dust ──► ModalResonator (bowl) ──┐   (stereo from the bowl's side)
 *   Particle (droplets) ─────────────┼─► mix ─► Reverb ─► Limiter ─► DAC
 *   StringSynthOscillator (drone) ───┘
 *
 *          Lorenz ──► modulates bowl structure, brightness, drone filter
 *
 * - Dust strikes the bowl at random, a few times a minute.
 * - Particle adds sparse, glassy pings around the bowl's pitch.
 * - A string-machine drone sits underneath, through a slowly opening
 *   low-pass.
 * - A Lorenz attractor wanders the bowl's inharmonicity and the drone's
 *   brightness, so the patch never quite repeats.
 * - The bowl's side() output, a second pickup swept by a slow LFO, is
 *   added to one channel and subtracted from the other, so the bowl
 *   moves across the stereo field.
 *
 * THE PANEL (the board version has none; it plays itself here too):
 *   knob 0 (a/z)    Strikes: from one a minute to one a second
 *   knob 1 (s/x)    Reverb: dry to drenched
 *   switch 0 (1)    Hit: strike the bowl now
 *
 * CPU: about 66% on the F411, of which the 12-mode bowl is about 700
 * cycles per sample and the reverb about 480. On a desktop it is a few
 * percent: compare the panel's CPU meter.
 */

#include "bbtui/tui.h"
#include "buddhabox/core/dsp_common.h"
#include "buddhabox/filter/svf.h"
#include "buddhabox/fx/limiter.h"
#include "buddhabox/fx/reverb.h"
#include "buddhabox/mod/lorenz.h"
#include "buddhabox/noise/dust.h"
#include "buddhabox/noise/particle.h"
#include "buddhabox/osc/string_synth.h"
#include "buddhabox/physical/modal_resonator.h"

using namespace bb;

// ── Voices ──────────────────────────────────────────────────────────────
ModalResonator<> bowl; // no bowing: saves RAM
Dust striker;
Particle droplets;
StringSynthOscillator drone;
Svf droneFilter;

// ── Modulation and effects ──────────────────────────────────────────────
Lorenz chaos;
Reverb<> reverb; // 32 KB
Limiter limiter;

// Control-rate work runs every kControlPeriod samples (about 1.4 kHz),
// which keeps per-sample cost down.
const int kControlPeriod = 32;
int controlCounter = 0;

// The bowl's level in the mix (its centre and side outputs alike).
const float kBowlGain = 3.5f;

// Set by loop() when the Hit switch is pressed; the audio thread
// strikes the bowl and clears it.
volatile bool strikeNow = false;

/**
 * Update slow-moving parameters from the Lorenz attractor.
 */
void controlUpdate() {
  // Lorenz outputs wander around ±1; map them to musical ranges.
  bowl.setStructure(0.15f + 0.1f * chaos.x()); // around bell-like
  bowl.setBrightness(0.5f + 0.2f * chaos.y());
  float cutoff = 300.0f + 250.0f * (chaos.z() + 1.0f);
  droneFilter.setCoefficients(cutoff / SAMPLE_RATE, 0.9f, TanApprox::Fast);
}

/**
 * Fill one block of audio (see the sine example for the basics).
 * @param in      Unused: no input
 * @param out     Interleaved stereo output
 * @param frames  Stereo frames to fill
 */
void audioCallback(const float *in, float *out, int frames) {
  for (int i = 0; i < frames; i++) {
    // 1. Slow modulation.
    chaos.process();
    if (++controlCounter >= kControlPeriod) {
      controlCounter = 0;
      controlUpdate();
    }

    // 2. Sources: the struck bowl, the droplets, the filtered drone.
    //    Gains set from a host simulation of two minutes of the patch: a
    //    full strike peaks near 0.5, droplets near 0.17, and the drone
    //    sits about 8 dB below the bowl's average level. The bowl needs
    //    a big gain because a one-sample strike puts little energy in.
    float hit = striker.process();
    if (strikeNow) { // the Hit switch: a full-strength impulse
      strikeNow = false;
      hit = 1.0f;
    }
    float bowlOut = bowl.process(hit) * kBowlGain;
    float drops = droplets.process() * 0.3f;
    float droneOut = droneFilter.process(drone.process()).lp * 0.06f;
    float mix = bowlOut + drops + droneOut;

    // 3. Stereo: the bowl's side signal (a moving second pickup minus the
    // main one) widens it: (centre + side, centre - side). It comes free
    // with the resonator, which computes it anyway.
    float width = bowl.side() * kBowlGain;
    float left = mix + width, right = mix - width;
    reverb.process(left, right);
    limiter.process(left, right);

    out[2 * i] = left;
    out[2 * i + 1] = right;
  }
}

void setup() {
  bowl.setFrequency(146.8f); // D3
  bowl.setModes(12);         // 16 modes would push the patch to about 85%
  bowl.setDamping(0.85f);
  bowl.setPosition(0.3f);
  striker.setDensity(0.08f); // about five strikes a minute

  droplets.setFrequency(1174.7f); // D6, two octaves above the bowl
  droplets.setDensity(0.6f);
  droplets.setSpread(7.0f);
  droplets.setQ(300.0f);

  drone.setFrequency(73.4f); // D2
  drone.setRegistration(0.25f);

  chaos.setRate(0.05f); // very slow drift

  reverb.setTime(0.99f);
  reverb.setAmount(0.45f);
  reverb.setLp(0.6f);

  tui::setTitle("SINGING BOWLS");
  // Knob positions that give the board version's settings: 0.08 Hz
  // strikes and 0.45 reverb.
  tui::setKnob(0, "Strikes", 0.38f);
  tui::setKnob(1, "Reverb", 0.45f);
  tui::setSwitch(0, "Hit", true);
  AudioEngine::begin();
}

/**
 * Read the panel. Only a knob that moved touches its module, so the
 * setters run when something changes, not 1000 times a second.
 */
void loop() {
  static float lastStrikes = -1.0f, lastReverb = -1.0f;
  float strikes = tui::knob(0), wet = tui::knob(1);
  if (strikes != lastStrikes) {
    lastStrikes = strikes;
    // One a minute (1/60 Hz) to one a second, on a log scale: 0.38
    // lands on the board's 0.08 Hz, about five a minute.
    striker.setDensity(fastExp2(-5.9f + 5.9f * strikes));
  }
  if (wet != lastReverb) {
    lastReverb = wet;
    reverb.setAmount(wet);
  }
  if (tui::switchPressed(0))
    strikeNow = true;
}
