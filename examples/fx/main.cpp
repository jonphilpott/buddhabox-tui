/**
 * @file main.cpp
 * @brief An effects box for the audio input: a tape-style echo into a
 *        spring reverb. The one example here that listens.
 *
 *   input ─► drive ─► echo (darkening repeats) ─► spring ─► out
 *
 * Plug a guitar, synth or microphone into your computer's input (or a
 * USB interface's), or just talk into the laptop's microphone. Use
 * headphones with a microphone, or the echo feeds back through the air.
 *
 * On macOS the first run asks for microphone permission for your
 * terminal app. If you said no, turn it on again in System Settings,
 * Privacy & Security, Microphone. On Linux, pick the device with
 * BB_AUDIO_IN if the default is wrong (BB_AUDIO_IN=plughw:1 ./build/fx).
 *
 * THE PANEL:
 *   knob 0 (a/z)  Drive: clean to fuzzy (Overdrive)
 *   knob 1 (s/x)  Time: echo time, 20 ms to 10 s
 *   knob 2 (d/c)  Feedback: one repeat to nearly endless
 *   knob 3 (f/v)  Echo: how loud the repeats are
 *   knob 4 (g/b)  Space: the spring's dry/wet
 *   switch 0 (1)  Thru: input straight to output, effects off
 *
 * Latency: the input and output each queue a few blocks (5.8 ms each),
 * so the sound comes out a few tens of milliseconds late (not
 * measured). Fine for echoes; maybe too slow to play through dry.
 */

#include "bbtui/tui.h"
#include "buddhabox/core/dsp_common.h"
#include "buddhabox/fx/delay.h"
#include "buddhabox/fx/overdrive.h"
#include "buddhabox/fx/spring_reverb.h"

using namespace bb;

// ── Modules ──────────────────────────────────────────────────────────
Overdrive drive;
// 10 s of 16-bit storage: 862 KB, nothing on a desktop.
DelayLine<441000> echo;
SpringReverb<> spring;

// ── Settings from loop(), read by the audio thread ──────────────────
float echoSamples = 13000.0f, echoFeedback = 0.4f, echoMix = 0.4f;
bool bypass = false;
float echoTone = 0.0f; // the feedback low-pass's state (audio only)

/** Fill one block: the input through the chain (audio thread). */
void audioCallback(const float *in, float *out, int frames) {
  for (int i = 0; i < frames; i++) {
    float left = in[2 * i], right = in[2 * i + 1];
    if (bypass) {
      out[2 * i] = left;
      out[2 * i + 1] = right;
      continue;
    }
    // 1. Drive, on the mono sum: the echo and spring are mono-in.
    float x = drive.process(0.5f * (left + right));

    // 2. Echo. Each repeat goes back in through a gentle low-pass, so
    //    it comes back darker than the last, as on tape.
    float wet = echo.read(echoSamples);
    echoTone += 0.35f * (wet - echoTone);
    echo.write(x + echoFeedback * echoTone);
    x += echoMix * wet;

    // 3. The spring, mono in, stereo out; then a soft limit, since
    //    feedback near the top can build past full scale.
    float l = x, r = x;
    spring.process(l, r);
    out[2 * i] = softLimitClip(l);
    out[2 * i + 1] = softLimitClip(r);
  }
}

void setup() {
  tui::setTitle("FX");
  tui::setKnob(0, "Drive", 0.0f);
  tui::setKnob(1, "Time", 0.6f);
  tui::setKnob(2, "Feedbck", 0.4f);
  tui::setKnob(3, "Echo", 0.4f);
  tui::setKnob(4, "Space", 0.3f);
  tui::setSwitch(0, "Thru");
  spring.setTime(0.6f);
  spring.setTone(0.5f);
  AudioEngine::begin(true); // true: open the input too
}

void loop() {
  drive.setDrive(tui::knob(0));
  // Time on a log scale, 20 ms to 10 s (a ratio of 500, 2^8.97): each
  // tenth of the knob multiplies it by the same amount, as the ear
  // hears rhythm.
  float seconds = 0.02f * fastExp2(8.97f * tui::knob(1));
  echoSamples = minf(seconds * SAMPLE_RATE, 440998.0f);
  echoFeedback = 0.95f * tui::knob(2); // under 1: never runs away
  echoMix = tui::knob(3);
  spring.setAmount(tui::knob(4));
  bypass = tui::switchOn(0);
}
