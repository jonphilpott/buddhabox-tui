/**
 * @file tui.h
 * @brief Everything a buddhabox-tui program needs besides the DSP
 *        modules: the audio engine, MIDI input and the virtual panel.
 *
 * A buddhabox-tui program looks like a BuddhaBox sketch. It defines
 * three global functions, and the library supplies main():
 *
 *   setup()          once, before the audio starts: configure modules,
 *                    label the panel, call AudioEngine::begin()
 *   loop()           over and over (about 1000 times a second) on the
 *                    main thread: read knobs, switches, menu and MIDI;
 *                    draw on the display. Never block in here.
 *   audioCallback()  on the audio thread, whenever the sound card needs
 *                    another 256 frames
 *
 * THE PANEL (drawn in the terminal; keys on a US layout): four pages,
 * each with 10 knobs and 20 switches, so 40 knobs and 80 switches in
 * all. The keys play the page on screen; [ and ] change page. On page p
 * (0-3) the keys reach knobs 10p to 10p+9 and switches 20p to 20p+19:
 *
 *   1 2 3 4 5 6 7 8 9 0     switches 20p+0 to 20p+9
 *   q w e r t y u i o p     switches 20p+10 to 20p+19
 *   a s d f g h j k l ;     knobs 10p+0 to 10p+9 up (Shift: fine steps)
 *   z x c v b n m , . /     knobs 10p+0 to 10p+9 down
 *   [ ]                     previous, next page
 *
 * Above the switches, 16 LEDs (setLed(), setLedFlash()) for the program
 * to show what it likes: steps, gates, warnings.
 *   up/down                 the menu: move the highlight
 *   left/right              the menu: change the highlighted value
 *   space                   the menu: run an action item
 *   Ctrl-R                  start recording the output to a new WAV
 *                           file; again to stop (see recorder.cpp)
 *   Ctrl-C                  quit
 *
 * THREADS: loop() and audioCallback() run at the same time on different
 * threads, as loop() and the DMA interrupt do on the board. Share
 * settings the same way: loop() writes a float or a bool, the callback
 * reads it. (Strictly, C++ calls an unsynchronised float a data race;
 * on x86 and ARM an aligned 32-bit store can't tear, so the callback
 * sees the old value or the new one, never half of each.)
 *
 * Example (a sine whose pitch is knob 0):
 *   #include "bbtui/tui.h"
 *   #include "buddhabox/osc/sine.h"
 *   using namespace bb;
 *
 *   SineOscillator osc;
 *   float hz = 220.0f;
 *
 *   void audioCallback(const float *in, float *out, int frames) {
 *     for (int i = 0; i < frames; i++)
 *       out[2 * i] = out[2 * i + 1] = 0.3f * osc.next(hz / SAMPLE_RATE);
 *   }
 *   void setup() {
 *     tui::setKnob(0, "Pitch", 0.5f);
 *     AudioEngine::begin();
 *   }
 *   void loop() { hz = 110.0f * fastExp2(3.0f * tui::knob(0)); }
 */

#pragma once

#include <cstdint>

#include "bbtui/display.h"

/**
 * Called once at start-up, before anything else. Defined by your program.
 *
 * Example:
 *   void setup() { tui::setTitle("DRONE"); AudioEngine::begin(); }
 */
void setup();

/**
 * Called over and over on the main thread, about 1000 times a second.
 * Defined by your program. Keep it quick: the panel and the keys are
 * handled between calls.
 *
 * Example:
 *   void loop() { cutoff = tui::knob(0); }
 */
void loop();

/**
 * Fill one block of audio. Defined by your program; runs on the audio
 * thread. No printing, no waiting, no allocation in here.
 *
 * @param in      Interleaved stereo input [L, R, L, R, ...], -1 to 1.
 *                Silence unless AudioEngine::begin(true) opened an input.
 * @param out     Interleaved stereo output to fill, -1 to 1. Values past
 *                ±1 are clipped by the sound card: limit them yourself.
 * @param frames  Stereo frames in each buffer (AUDIO_BLOCK_SIZE, 256)
 *
 * Example:
 *   void audioCallback(const float *in, float *out, int frames) {
 *     for (int i = 0; i < 2 * frames; i++) out[i] = in[i]; // a wire
 *   }
 */
void audioCallback(const float *in, float *out, int frames);

namespace bb {

namespace AudioEngine {
/**
 * Start the audio: the default output device (and input, if asked) at
 * 44.1 kHz, stereo, in blocks of 256 frames. The operating system
 * converts to the device's own rate if it differs.
 *
 * CHOOSING DEVICES. On Linux it uses ALSA's "default" device; set
 * BB_AUDIO_OUT or BB_AUDIO_IN to another ALSA name, for example
 * BB_AUDIO_IN=plughw:1 for a USB interface. On macOS output goes to the
 * system's output, and BB_AUDIO_IN picks an input by part of its name
 * (BB_AUDIO_IN=zoom). `./program --devices` lists the inputs.
 *
 * CHOOSING INPUT CHANNELS. `left` and `right` pick which of the
 * device's channels become the program's stereo input, counting from 1
 * as on an interface's panel. One channel alone is mono, sent to both
 * sides. The environment variable BB_AUDIO_IN_CHANNELS ("3" or "3,4")
 * overrides them, so one line in a shell profile can serve every
 * program. The status line shows what was opened: "input: ZOOM L6, 3".
 *
 * @param input  true to open the input as well. On macOS the first run
 *               asks for microphone permission for your terminal app.
 *               Off by default, so synths don't ask.
 * @param left   The device channel for the left input, from 1; 0 (the
 *               default) for the device's first two channels
 * @param right  The device channel for the right input; 0 for the same
 *               as `left`
 * @return       true if the output started. The status line says what
 *               failed if not, or if the input failed.
 *
 * Example:
 *   AudioEngine::begin();            // output only
 *   AudioEngine::begin(true);        // and input channels 1 and 2
 *   AudioEngine::begin(true, 3);     // and input 3, mono
 *   AudioEngine::begin(true, 3, 4);  // and inputs 3 and 4, stereo
 */
bool begin(bool input = false, int left = 0, int right = 0);

/**
 * @return  The share of each block's time spent in audioCallback(),
 *          smoothed: 0.25 means a quarter. The panel shows it.
 *
 * Example:
 *   float load = AudioEngine::cpuLoad();
 */
float cpuLoad();

/**
 * @return  The highest single-block load seen so far (1.0 or more means
 *          a block ran late and the sound glitched).
 *
 * Example:
 *   if (AudioEngine::cpuPeak() > 1.0f) tui::status("overrun!");
 */
float cpuPeak();

/** Stop the audio. main() calls this on the way out; you needn't. */
void end();
} // namespace AudioEngine

namespace Midi {
/**
 * One MIDI message: a status byte and up to two data bytes.
 *
 * status's top four bits say what it is (0x90 note on, 0x80 note off,
 * 0xB0 control change ...) and its low four the channel, 0-15.
 * System messages (0xF8 clock, 0xFA start ...) have no channel.
 *
 * Example:
 *   Midi::Message m;
 *   while (Midi::read(m))
 *     if (m.isNoteOn()) playNote(m.data1, m.data2 / 127.0f);
 */
struct Message {
  uint8_t status, data1, data2;

  /** @return  true for a note on with velocity above 0 */
  bool isNoteOn() const { return (status & 0xF0) == 0x90 && data2 > 0; }
  /** @return  true for a note off, or a note on with velocity 0 (which
   *           many keyboards send instead) */
  bool isNoteOff() const {
    return (status & 0xF0) == 0x80 || ((status & 0xF0) == 0x90 && !data2);
  }
  /** @return  true for a control change: data1 the number, data2 the
   *           value */
  bool isCC() const { return (status & 0xF0) == 0xB0; }
  /** @return  The channel, 0-15 (meaningless for system messages) */
  int channel() const { return status & 0x0F; }
};

/**
 * Listen to every MIDI input there is: on macOS every CoreMIDI source,
 * on Linux every ALSA sequencer port that can be read. Devices plugged
 * in later aren't seen until the program restarts.
 *
 * @return  true if the MIDI system opened (even with no devices).
 *
 * Example:
 *   void setup() { Midi::begin(); AudioEngine::begin(); }
 */
bool begin();

/**
 * Take the next waiting message. Call it from loop() until it returns
 * false; up to 256 messages wait, and any more are dropped.
 *
 * @param m  Filled with the message
 * @return   false when none are waiting
 *
 * Example:
 *   Midi::Message m;
 *   while (Midi::read(m)) { ... }
 */
bool read(Message &m);
} // namespace Midi

namespace tui {
constexpr int PAGES = 4;
constexpr int KNOBS_PER_PAGE = 10;
constexpr int SWITCHES_PER_PAGE = 20;
constexpr int KNOBS = PAGES * KNOBS_PER_PAGE;       // 40
constexpr int SWITCHES = PAGES * SWITCHES_PER_PAGE; // 80
constexpr int LEDS = 16;

// LED colours for setLed(): positions around the colour wheel (the
// value times 360 is the hue in degrees). Any value between works too:
// 0.08 is orange, 0.75 violet.
constexpr float LED_OFF = 0.0f;
constexpr float LED_RED = 1.0f;
constexpr float LED_YELLOW = 1.0f / 6.0f;
constexpr float LED_GREEN = 2.0f / 6.0f;
constexpr float LED_CYAN = 3.0f / 6.0f;
constexpr float LED_BLUE = 4.0f / 6.0f;
constexpr float LED_MAGENTA = 5.0f / 6.0f;

/**
 * Name the program in the panel's title bar.
 *
 * @param title  Up to 14 characters show; the pointer is kept, so pass
 *               a string literal or a global.
 *
 * Example:
 *   tui::setTitle("ACID");
 */
void setTitle(const char *title);

/**
 * Label a knob and set where it starts. Unlabelled knobs are drawn dim
 * but still work.
 *
 * @param i      The knob, 0-39: page i / 10, and on that page the
 *               (i % 10)th key pair (a/z, s/x ... ;/ /)
 * @param label  Up to 7 characters show; the pointer is kept
 * @param value  Its starting position, 0 to 1
 *
 * Example:
 *   tui::setKnob(0, "Cutoff", 0.3f);
 */
void setKnob(int i, const char *label, float value = 0.0f);

/**
 * @param i  The knob, 0-39
 * @return   Its position, 0 to 1. A key press moves it by 0.01 (0.001
 *           with Shift); holding the key keeps it moving.
 *
 * Example:
 *   float cutoff = 40.0f * fastExp2(9.0f * tui::knob(0));
 */
float knob(int i);

/**
 * Label a switch and choose how it behaves.
 *
 * @param i          The switch, 0-79: page i / 20, and on that page
 *                   the (i % 20)th key (1-0, then q-p)
 * @param label      Up to 5 characters show; the pointer is kept
 * @param momentary  false: each press flips it on or off (a toggle).
 *                   true: on while the key is held. Most terminals
 *                   can't report a key coming up; in those, a momentary
 *                   switch lights for 150 ms per press. kitty, WezTerm,
 *                   Ghostty, foot and iTerm2 report it, and the panel
 *                   uses it when they do.
 *
 * Example:
 *   tui::setSwitch(0, "Gate");               // toggle
 *   tui::setSwitch(10, "Hit", true);         // momentary
 *   tui::setSwitch(20, "Seq");               // page 2, key 1
 */
void setSwitch(int i, const char *label, bool momentary = false);

/**
 * @param i  The switch, 0-79
 * @return   true while it is on (toggled on, or held)
 *
 * Example:
 *   bool drone = tui::switchOn(0);
 */
bool switchOn(int i);

/**
 * @param i  The switch, 0-79
 * @return   true once for each press since the last call: for actions
 *           (strike, randomize) rather than states.
 *
 * Example:
 *   if (tui::switchPressed(10)) lpg.trigger();
 */
bool switchPressed(int i);

/**
 * Turn a switch's light on or off from code: to show a toggle's state
 * after loading settings, or to use the switch row as LEDs.
 *
 * @param i   The switch, 0-79
 * @param on  The new state
 *
 * Example:
 *   tui::setSwitchOn(step, true);  // light the playing step
 */
void setSwitchOn(int i, bool on);

/**
 * Light an LED. The 16 LEDs sit in a row between the oscilloscope and
 * the switches; the row appears the first time a program sets one.
 *
 * @param i       The LED, 0-15
 * @param colour  0 (LED_OFF) turns it off. Anything above, up to 1,
 *                picks a colour around the colour wheel: 1/6 yellow,
 *                1/3 green, 1/2 cyan, 2/3 blue, 5/6 magenta, 1 red
 *                (the LED_ constants). Values in between blend: 0.08
 *                is orange. Values outside 0 to 1 are clamped.
 *
 * Example:
 *   tui::setLed(0, tui::LED_GREEN);   // running
 *   tui::setLed(1, level);            // a level as a colour
 *   tui::setLed(2, tui::LED_OFF);
 */
void setLed(int i, float colour);

/**
 * Make an LED flash, or stop it flashing. A flashing LED blinks in its
 * colour twice a second, in step with every other flashing LED; an LED
 * that is off stays off.
 *
 * @param i      The LED, 0-15
 * @param flash  true to flash, false to shine steadily
 *
 * Example:
 *   tui::setLed(3, tui::LED_RED);
 *   tui::setLedFlash(3, true);        // a warning
 */
void setLedFlash(int i, bool flash);

/**
 * @return  Up/down presses since the last call: down +1, up -1 each.
 *          Hand it to Menu::update().
 *
 * Example:
 *   menu.update(tui::menuMove(), tui::menuAdjust(), tui::menuPressed());
 */
int menuMove();

/**
 * @return  Left/right presses since the last call: right +1, left -1
 *          each. Hand it to Menu::update().
 */
int menuAdjust();

/**
 * @return  true once for each press of the space bar since the last
 *          call.
 */
bool menuPressed();

/**
 * @return  The virtual 128 x 64 OLED. Draw on it in loop(); the panel
 *          shows it about 30 times a second, once anything is drawn.
 *
 * Example:
 *   Oled &oled = tui::display();
 *   oled.text(0, 0, "HELLO");
 */
Oled &display();

/**
 * Show a line of text in the panel's status bar (in place of the
 * board's Serial.print()).
 *
 * @param text  The message; copied, so a local buffer is fine
 *
 * Example:
 *   tui::status("no input device");
 */
void status(const char *text);

/**
 * @return  Milliseconds since the program started, as Arduino's
 *          millis().
 *
 * Example:
 *   if (tui::millis() - last >= 50) { last = tui::millis(); draw(); }
 */
uint32_t millis();
} // namespace tui

} // namespace bb
