/**
 * @file main.cpp
 * @brief The smallest buddhabox-tui program: a sine tone you can play
 *        from a knob or a MIDI keyboard.
 *
 * Every buddhabox-tui program has the same parts:
 * 1. DSP objects as globals (from buddhabox-framework).
 * 2. audioCallback(): fills each block of output on the audio thread.
 * 3. setup(): labels the panel, configures the objects, starts audio.
 * 4. loop(): reads the panel and MIDI on the main thread and hands the
 *    results to the audio through plain globals.
 *
 * Panel:
 *   knob 0 (a/z)  Pitch, 55 Hz to 880 Hz
 *   knob 1 (s/x)  Level
 *   switch 0 (1)  Mute
 *   MIDI          a note on sets the pitch
 */

#include "bbtui/tui.h"
#include "buddhabox/core/dsp_common.h"
#include "buddhabox/core/midi_utils.h"
#include "buddhabox/osc/sine.h"

using namespace bb;

// 1. DSP objects, and the settings loop() hands to the audio thread.
SineOscillator osc;
float hz = 220.0f, level = 0.3f;
bool mute = false;

// 2. The audio: one sine, the same on both channels.
void audioCallback(const float *in, float *out, int frames) {
  float gain = mute ? 0.0f : level;
  for (int i = 0; i < frames; i++)
    out[2 * i] = out[2 * i + 1] = gain * osc.next(hz / SAMPLE_RATE);
}

// 3. Set-up.
void setup() {
  tui::setTitle("SINE");
  tui::setKnob(0, "Pitch", 0.5f);
  tui::setKnob(1, "Level", 0.3f);
  tui::setSwitch(0, "Mute");
  Midi::begin();
  AudioEngine::begin();
}

// 4. Controls. The pitch knob covers four octaves; fastExp2() turns its
//    0-1 into a ratio, so each tenth of the knob is the same interval.
void loop() {
  hz = 55.0f * fastExp2(4.0f * tui::knob(0));
  level = tui::knob(1);
  mute = tui::switchOn(0);

  // A MIDI note sets the pitch knob to match, so the panel shows it.
  Midi::Message m;
  while (Midi::read(m))
    if (m.isNoteOn())
      tui::setKnob(0, "Pitch", fastLog2(midiToFreq(m.data1) / 55.0f) / 4.0f);
}
