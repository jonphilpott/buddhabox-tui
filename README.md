# buddhabox-tui

A virtual BuddhaBox in the terminal for macOS and Linux (x86 and
Raspberry Pi). Programs are built from the
BuddhaBox DSP modules and play through the computer's own sound card. Each one shows the same panel:
a 128 × 64 OLED-style display, an oscilloscope, 16 coloured LEDs, and
four pages of controls (80 switches and 40 knobs), all played from the
keyboard.

```
 ACID         up/dn item  lt/rt value  ^C quit       [1234] CPU  1.3% pk 2.3% M
       │⣿⢱⣶⢹⢱⣶⣽⣷⢰⣿⢰⣮⢻⣿⣿⣿⣿⣿⣿⣯⢸⣿⣵⡶⣹⣐⣒⢾⣿⣿⣿⠸⣛⢿⢛⣛⢿⢛⢟⢿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿│
       │ ... the display, 16 rows of braille ...                         │
⣀⣀⣀⣀⣀⣀⣀⣀⠀⠀⠀⠀⠀⠀⣀⣀⡀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⢀⣀⣀⣀⣀⣀⣀⣀⣀⣀⣀⡀⠀⠀⠀⣀⣀⣀⣀⣀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⣀⣀⣀⣀⣀⣀⣀⣀⣀⣀⣀⣀⠀⣀⣀⣀⣀⣀⣀⣀⣀
⠀⠀⠀⠀⠀⠀⠀⠈⠉⠉⠉⠉⠉⠉⠀⠀⠉⠉⠉⠉⠉⠉⠉⠉⠉⠉⠉⠁⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠉⠉⠉⠉⠀⠀⠀⠀⠈⠉⠉⠉⠉⠉⠉⠉⠉⠉⠉⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠈⠉⠁
1○Rndmz 2○      3○      4○      5○      6○      7○      8○      9○      0○
q○      w○      e○      r○      t○      y○      u○      i○      o○      p○
Cutoff  Resonan Env Mod Decay   Accent  Drive   Chaos   Delay M -       -
██▎···· █████▏· ████▎·· ██▊···· ████▎·· ██▏···· ······· ██▏···· ······· ·······
a/z  33 s/x  73 d/c  60 f/v  40 g/b  60 h/n  30 j/m   0 k/,  30 l/.   0 ;//   0
```

## Build and run

In the project folder:

```
make            # every example, into build/
make acid       # just one
./build/acid
```

- **macOS:** needs the Xcode command-line tools (`xcode-select --install`).
- **Linux, Raspberry Pi:** needs a C++ compiler and the ALSA headers:
  `sudo apt install build-essential libasound2-dev`.

The panel needs 80 × 26 characters, or 80 × 10 for programs that don't
draw on the display, plus one row for programs that use the LEDs.

## The keys

- `[` and `]`: previous and next page. The title bar shows `[1234]`,
  with the page on screen highlighted and empty pages dimmed.
- `1`–`0`, `q`–`p`: the page's 20 switches
- `a`–`;`: the page's 10 knobs up
- `z`–`/`: the same knobs down (hold Shift on a knob key for fine steps)

In code, knobs are numbered 0–39 and switches 0–79, page by page:
knob 12 is page 2's `d`/`c`, and switch 25 is page 2's `6`.
- Up/down: move through the menu
- Left/right: change the highlighted value (hold to sweep)
- Space: run an action item, such as Randomize
- Ctrl-C: quit

Most terminals only report a key going down, not coming up. In those,
a momentary switch lights for 150 ms per press, and holding a knob key
moves it at the keyboard's repeat rate. kitty, WezTerm, Ghostty, foot
and recent iTerm2 do report releases (the kitty keyboard protocol). In
those, a momentary switch stays lit while its key is held, and a held
knob key sweeps smoothly. The panel detects this by itself.

## Examples

- `sine`: the smallest program. One tone; pitch from a knob or MIDI.
- `acid`: a 303-style bass line with a menu, eight knobs, and MIDI
  notes to set the key.
- `noisebox`: a West Coast noise box: folded FM, a random sequence,
  a low-pass gate.
- `easel`: a self-playing Buchla Easel-style drone. Light switch 1 to
  play it from the knobs instead.
- `singing_bowls`: struck resonators, chaotic drift and reverb.
- `fx`: the audio input through drive, echo and a spring. The one
  example that uses the input.

`acid`, `noisebox`, `easel` and `singing_bowls` are ports of the board
examples of the same names. The DSP code is unchanged; only the
hardware parts differ.

## Writing a program

A program is one `main.cpp` in `examples/<name>/`. `make` finds it. Copy
`examples/sine` to start. It defines three functions, and the library
supplies `main()`:

```cpp
#include "bbtui/tui.h"
#include "buddhabox/osc/sine.h"
using namespace bb;

SineOscillator osc;
float hz = 220.0f; // written by loop(), read by the audio thread

void audioCallback(const float *in, float *out, int frames) {
  for (int i = 0; i < frames; i++)
    out[2 * i] = out[2 * i + 1] = 0.3f * osc.next(hz / SAMPLE_RATE);
}

void setup() {
  tui::setKnob(0, "Pitch", 0.5f);
  AudioEngine::begin(); // begin(true) opens the input as well
}

void loop() { hz = 110.0f * fastExp2(3.0f * tui::knob(0)); }
```

`src/bbtui/tui.h` documents the whole API:

- Audio: `AudioEngine::begin`, `cpuLoad` and `cpuPeak`.
- MIDI: `Midi::begin` and `Midi::read`.
- The panel: `tui::knob`, `switchOn`, `switchPressed`, `menuMove`,
  `menuAdjust`, `menuPressed`, `display`, `status` and `millis`.
- LEDs: `tui::setLed(i, colour)`, where 0 is off and up to 1 picks a
  colour around the wheel (`LED_RED`, `LED_GREEN`, `LED_BLUE` and so
  on), and `tui::setLedFlash(i, true)` to make it blink.
- The menu: `Menu`, in `src/bbtui/menu.h`. Like the board's, but with
  no edit mode: left/right change a value directly.

Differences from a board sketch:

- `audioCallback` takes float input and output (−1 to 1), not `int16_t`.
  Keep peaks under 1, with `softLimitClip()` or a `Limiter`.
- Knobs, switches and the menu replace `AnalogInput`, `SwitchInput`,
  `EncoderInput` and `Oled`. You never construct them; just call
  `tui::...`.
- No `Serial`: the panel shows CPU load, and `tui::status()` shows
  messages.
- `loop()` runs about 1000 times a second. Don't block in it.
- Memory is plentiful: size buffers in seconds, not kilobytes.
  `DelayLine<441000>` is a 10 s echo in 862 KB.

## Docs and tools

- `docs/index.html`: the guide. `docs/reference.html`: the API
  reference, generated from the docstrings. Both use the framework's
  style.
- `python3 tools/gen_docs.py`: regenerate the reference, and the
  catalogue and code listings in the guide, after changing a docstring
  or an example.
- `python3 tools/new_project.py ../my_synth [--example acid]`: start a
  program in its own folder, with a Makefile that builds against this
  checkout.

## Where the DSP comes from

`src/buddhabox/` is a copy of
[buddhabox-framework](../buddhabox-framework)'s DSP library (taken at
its commit `7c5c012`), without the board-only `io/` and `engine/`. It is
a fork: change it here freely, since the two will diverge. Differences
so far:

- `DelayLine` takes a 32-bit size, so a buffer can hold minutes rather
  than the board's 1.49 s limit.

The lookup tables (`core/lut_sine.h`, `core/luts.h`,
`fx/lut_wavefolder.h`, `mod/lut_slope.h`) were generated by the
framework's `tools/gen_tables.cpp`; regenerate them there if they ever
need to change. For the licence, see Credits and licence below.

## Platform notes

- **Audio:** CoreAudio's AudioQueue on macOS. ALSA on Linux, through
  PipeWire or PulseAudio where they run; JACK isn't needed. Both run at
  44.1 kHz in blocks of 256 frames, and the operating system converts
  to the device's own rate.
- **Choosing a device on Linux:** `BB_AUDIO_OUT=plughw:1 ./build/acid`,
  and `BB_AUDIO_IN` for the input. `aplay -l` and `arecord -l` list
  the cards. On a Pi with a USB interface this is usually needed.
- **Real-time priority on Linux:** the audio threads ask for it and
  carry on without it. To grant it, add `@audio - rtprio 95` to
  `/etc/security/limits.conf` and join the `audio` group.
- **MIDI:** CoreMIDI on macOS and the ALSA sequencer on Linux. Every
  input present at start-up is connected. Restart to pick up devices
  plugged in later, or on Linux connect them with
  `aconnect <device> buddhabox-tui`.
- **Microphone permission (macOS):** a program that opens the input
  asks once, on behalf of your terminal app.
- **If it crashes:** the terminal may be left in raw mode. Type `reset`
  and press Enter.

## Credits and licence

Many of the DSP modules in `src/buddhabox/` are ports of code from
[Mutable Instruments](https://mutable-instruments.net)' Eurorack
firmware and its stmlib library, by Emilie Gillet: oscillators,
resonators, reverbs, filters, envelopes and drums from Plaits, Rings,
Elements, Clouds, Peaks, Streams, Tides and Warps, among others. They
reached this project through the
[Partials](https://github.com/jonphilpott/partials) library and
buddhabox-framework. A ported file's header comment names the module it
came from. Any mistakes made in porting are ours, not
Mutable Instruments'.

That code is Copyright (c) 2012-2023 Emilie Gillet and released under
the MIT licence. The full notice is in `LICENSE.mutable`, and it must
be kept with any copy of this code or substantial part of it.

The rest of buddhabox-tui is Copyright (c) 2026 Jon Philpott, also under
the MIT licence: see `LICENSE`.

The original firmware is at
[github.com/pichenettes/eurorack](https://github.com/pichenettes/eurorack).
