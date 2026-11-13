/**
 * @file panel.cpp
 * @brief The virtual front panel in the terminal, the keyboard, and
 *        main().
 *
 * WHAT THE TERMINAL GIVES US: a grid of characters, and a stream of
 * bytes when keys are pressed. Everything here is built on two things:
 * - Raw mode (termios): normally the terminal holds typed text until
 *   Enter and handles Ctrl-C itself. Raw mode hands us every key at once,
 *   Ctrl-C included, and stops the terminal echoing keys to the screen.
 * - ANSI escape codes: byte sequences starting with ESC (0x1b) that
 *   move the cursor, set colours and so on, instead of printing.
 *   "\x1b[5;1H" moves to row 5, column 1; "\x1b[33m" turns text yellow.
 * No ncurses: the panel uses a dozen codes, all listed where they're used.
 *
 * KEY RELEASES: an ordinary terminal sends bytes when a key goes down,
 * and again and again while it's held (auto-repeat), but nothing when it
 * comes up. The kitty keyboard protocol fixes that: once switched on,
 * every key arrives as "ESC [ code ; modifiers : event u", where event
 * is 1 (press), 2 (repeat) or 3 (release). At start-up we ask whether
 * the terminal speaks it, and use it if so.
 *
 * DRAWING: about 30 times a second the panel builds each screen line as
 * a string, and rewrites only the lines that changed since the last
 * frame. That keeps it light enough to run over SSH to a Raspberry Pi.
 *
 * LAYOUT (80 columns, 26 rows with a display, 10 without, plus one
 * once the program lights an LED):
 *   row 0       title bar: program, help or status, page, CPU, MIDI
 *   rows 1-16   the 128 x 64 display in braille (once something is drawn)
 *   rows 17-20  oscilloscope of the output
 *   row 21      the 16 LEDs (once one is set)
 *   rows 22-23  the page's 20 switches
 *   rows 24-26  the page's 10 knobs: label, bar, keys and value
 */

#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <chrono>
#include <thread>

#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include "bbtui/internal.h"
#include "bbtui/tui.h"

namespace bb {
namespace {

// ── Panel state (main thread only) ──────────────────────────────────
struct Knob {
  const char *label = nullptr;
  float value = 0.0f;
  int held = 0;       // kitty only: +1 or -1 while a key holds it
  bool fine = false;  // that key was pressed with Shift
  uint32_t since = 0; // when it was pressed, in ms
};

struct Switch {
  const char *label = nullptr;
  bool momentary = false;
  bool on = false;
  int presses = 0;        // not yet collected by switchPressed()
  uint32_t lastEvent = 0; // ms; times a momentary switch's 150 ms flash
};

Knob knobs[tui::KNOBS];
Switch switches[tui::SWITCHES];
int moves = 0, adjusts = 0, spacePresses = 0;
int page = 0; // the page on screen, 0 to tui::PAGES - 1
const char *title = "BUDDHABOX";
char statusText[80] = "";
uint32_t statusTime = 0;
Oled screen;

struct Led {
  float colour = 0.0f; // 0 off; above 0, a hue (see tui::setLed)
  bool flash = false;
};
Led leds[tui::LEDS];
bool ledsUsed = false; // has the program set any? Then show the row.

bool kitty = false; // the terminal reports key releases
volatile sig_atomic_t quit = 0, resized = 0;
termios savedTermios;

// The keyboard layout, US. Each key's position in kKeys is its job:
// 0-19 switches, 20-29 knobs up, 30-39 knobs down. kShifted holds the
// same keys with Shift, which ordinary terminals send as different
// characters ('A' for 'a', '!' for '1').
const char *const kKeys = "1234567890qwertyuiopasdfghjkl;zxcvbnm,./";
const char *const kShifted = "!@#$%^&*()QWERTYUIOPASDFGHJKL:ZXCVBNM<>?";

// Special keys, given codes outside ASCII.
enum { KEY_UP = 1000, KEY_DOWN, KEY_RIGHT, KEY_LEFT };
enum Event { PRESS = 1, REPEAT = 2, RELEASE = 3 };

uint32_t now() { return tui::millis(); }

// ── Terminal set-up ─────────────────────────────────────────────────

void out(const std::string &s) {
  // write() can write less than asked when the terminal is busy (over a
  // slow SSH link), so loop until it has all gone.
  const char *p = s.data();
  size_t left = s.size();
  while (left > 0) {
    ssize_t n = write(STDOUT_FILENO, p, left);
    if (n <= 0)
      return;
    p += n;
    left -= n;
  }
}

/**
 * Ask whether the terminal speaks the kitty keyboard protocol.
 *
 * We send two questions: "ESC [ ? u" (which kitty-protocol terminals
 * answer with "ESC [ ? flags u") and "ESC [ c" (device attributes, which
 * every terminal answers, ending in 'c'). If the 'u' answer comes before
 * the 'c' one, the protocol is there; if only the 'c' comes, it isn't.
 * Waiting for 'c' means we never have to guess a timeout for a terminal
 * that stays silent.
 */
bool detectKitty() {
  out("\x1b[?u\x1b[c");
  std::string reply;
  pollfd p = {STDIN_FILENO, POLLIN, 0};
  while (poll(&p, 1, 500) > 0) { // give up after half a second of quiet
    char c;
    if (read(STDIN_FILENO, &c, 1) != 1)
      break;
    reply += c;
    if (c == 'c')
      break;
  }
  return reply.find("\x1b[?") != std::string::npos &&
         reply.find('u') != std::string::npos;
}

void onSignal(int sig) {
  if (sig == SIGWINCH)
    resized = 1;
  else
    quit = 1;
}

void terminalStart() {
  // 1. Raw mode. cfmakeraw() clears the flags for line editing, echo,
  //    signals (so Ctrl-C reaches us as a byte) and output processing
  //    (so "\n" no longer means "\r\n": we position the cursor
  //    ourselves). VMIN = 0, VTIME = 0 makes read() return at once,
  //    with nothing if no key is waiting.
  tcgetattr(STDIN_FILENO, &savedTermios);
  termios raw = savedTermios;
  cfmakeraw(&raw);
  raw.c_cc[VMIN] = 0;
  raw.c_cc[VTIME] = 0;
  tcsetattr(STDIN_FILENO, TCSANOW, &raw);

  // 2. The alternate screen (ESC [ ? 1049 h): a separate page that
  //    vanishes on exit, leaving the shell's scrollback as it was. Then
  //    hide the cursor (ESC [ ? 25 l) and clear (ESC [ 2 J).
  out("\x1b[?1049h\x1b[?25l\x1b[2J");

  // 3. Key releases, if the terminal can. "ESC [ > 11 u" pushes flags
  //    1 + 2 + 8: unambiguous escape codes, event types (press, repeat,
  //    release), and every key as a code, letters included. Without
  //    flag 8, letters would still arrive as plain text with no release.
  kitty = detectKitty();
  if (kitty)
    out("\x1b[>11u");

  signal(SIGWINCH, onSignal); // the window changed size
  signal(SIGTERM, onSignal);
  signal(SIGHUP, onSignal); // the terminal closed (or SSH dropped)
}

void terminalEnd() {
  // Undo everything, in reverse: pop the kitty flags (ESC [ < u), reset
  // colours, show the cursor, leave the alternate screen, restore the
  // saved termios.
  if (kitty)
    out("\x1b[<u");
  out("\x1b[0m\x1b[?25h\x1b[?1049l");
  tcsetattr(STDIN_FILENO, TCSANOW, &savedTermios);
}

// ── Keys ────────────────────────────────────────────────────────────

/**
 * Act on one key.
 *
 * @param key    A character from kKeys, ' ', or a KEY_ arrow code
 * @param shift  Shift was held
 * @param event  PRESS, REPEAT or RELEASE (ordinary terminals only send
 *               PRESS, with auto-repeat arriving as more presses)
 */
void handleKey(int key, bool shift, int event) {
  uint32_t t = now();

  // 1. Arrows and space drive the menu: up/down move, left/right
  //    change. Holding an arrow repeats it, to scroll or sweep a value.
  if (key >= KEY_UP) {
    if (event == RELEASE)
      return;
    if (key == KEY_UP || key == KEY_DOWN)
      moves += key == KEY_DOWN ? 1 : -1;
    else
      adjusts += key == KEY_RIGHT ? 1 : -1;
    return;
  }
  if (key == ' ') {
    if (event == PRESS)
      spacePresses++;
    return;
  }

  // 2. [ and ] change page, wrapping round. Anything held is let go
  //    first: its key-up would otherwise land on the new page, and the
  //    old knob would sweep, or the switch stay lit, for ever.
  if (key == '[' || key == ']') {
    if (event != PRESS)
      return;
    for (Knob &k : knobs)
      k.held = 0;
    for (Switch &s : switches)
      if (s.momentary)
        s.on = false;
    page = (page + (key == ']' ? 1 : tui::PAGES - 1)) % tui::PAGES;
    return;
  }
  const char *pos = key > 0 && key < 128 ? strchr(kKeys, key) : nullptr;
  if (!pos)
    return;
  int index = (int)(pos - kKeys);

  // 3. Switches: the key's position on the page on screen.
  if (index < tui::SWITCHES_PER_PAGE) {
    Switch &s = switches[page * tui::SWITCHES_PER_PAGE + index];
    // In an ordinary terminal, a held key's auto-repeat looks just like
    // more presses. Repeats come every 30-80 ms and nobody taps twice
    // that fast, so a "press" under 120 ms after the last is a repeat.
    if (!kitty && event == PRESS && t - s.lastEvent < 120)
      event = REPEAT;
    s.lastEvent = t;
    if (event == PRESS) {
      s.presses++;
      s.on = s.momentary ? true : !s.on;
    } else if (event == RELEASE && s.momentary) {
      s.on = false;
    }
    return;
  }

  // 4. Knobs. A press nudges by a step; in kitty mode a held key then
  //    sweeps smoothly (tick() does it), so its repeats are ignored. In
  //    ordinary terminals the repeats are the sweep.
  index -= tui::SWITCHES_PER_PAGE;
  int dir = index < tui::KNOBS_PER_PAGE ? 1 : -1;
  Knob &k = knobs[page * tui::KNOBS_PER_PAGE + index % tui::KNOBS_PER_PAGE];
  if (event == RELEASE) {
    k.held = 0;
    return;
  }
  if (event == REPEAT && kitty)
    return;
  k.value += dir * (shift ? 0.001f : 0.01f);
  k.value = k.value < 0.0f ? 0.0f : (k.value > 1.0f ? 1.0f : k.value);
  if (event == PRESS && kitty) {
    k.held = dir;
    k.fine = shift;
    k.since = t;
  }
}

/**
 * Parse one "ESC [ ... final" sequence and act on it.
 *
 * @param params  What sits between "ESC [" and the final byte
 * @param final   The final byte: 'u' for a kitty key, 'A'-'D' arrows
 */
void handleCsi(const std::string &params, char final) {
  // Kitty form: "code;modifiers:event". Ordinary arrows have no
  // parameters, or "1;modifiers". sscanf leaves the defaults in place
  // for any field that's missing.
  int code = 1, mods = 1, event = PRESS;
  sscanf(params.c_str(), "%d;%d:%d", &code, &mods, &event);
  bool shift = (mods - 1) & 1; // modifiers are sent as 1 + a bitmask
  bool ctrl = (mods - 1) & 4;

  switch (final) {
  case 'A':
    return handleKey(KEY_UP, shift, event);
  case 'B':
    return handleKey(KEY_DOWN, shift, event);
  case 'C':
    return handleKey(KEY_RIGHT, shift, event);
  case 'D':
    return handleKey(KEY_LEFT, shift, event);
  case 'u':
    if (ctrl && code == 'c')
      quit = 1;
    else if (ctrl && code == 'r' && event == PRESS)
      detail::recordToggle(title);
    else if (!ctrl)
      handleKey(code, shift, event);
    return;
  default:
    return; // anything else (focus reports, F-keys): ignore
  }
}

/** Read every waiting byte and act on the keys in it. */
void readKeys() {
  static std::string pending; // a sequence split across reads
  char buf[256];
  ssize_t n;
  while ((n = read(STDIN_FILENO, buf, sizeof(buf))) > 0)
    pending.append(buf, n);

  size_t i = 0;
  while (i < pending.size()) {
    unsigned char c = pending[i];
    if (c == 0x03) { // Ctrl-C in an ordinary terminal
      quit = 1;
      i++;
    } else if (c == 0x12) { // Ctrl-R: start or stop recording
      detail::recordToggle(title);
      i++;
    } else if (c == 0x1b) {
      // An escape sequence: "ESC [ params final" or "ESC O letter" (the
      // arrows in some terminals). If it hasn't all arrived, wait.
      if (i + 1 >= pending.size())
        break;
      char kind = pending[i + 1];
      if (kind == 'O') {
        if (i + 2 >= pending.size())
          break;
        handleCsi("", pending[i + 2]);
        i += 3;
      } else if (kind == '[') {
        // The final byte is the first in the range '@' to '~'.
        size_t j = i + 2;
        while (j < pending.size() && (pending[j] < '@' || pending[j] > '~'))
          j++;
        if (j >= pending.size())
          break;
        handleCsi(pending.substr(i + 2, j - i - 2), pending[j]);
        i = j + 1;
      } else {
        i++; // a lone Escape key press: ignore it
      }
    } else {
      // A plain character: a key, or a key with Shift.
      const char *shifted = strchr(kShifted, c);
      if (c && shifted)
        handleKey(kKeys[shifted - kShifted], true, PRESS);
      else
        handleKey(c, false, PRESS);
      i++;
    }
  }
  pending.erase(0, i);
}

/**
 * Time-based work, every loop: sweep held knobs (kitty), and switch off
 * momentary switches whose 150 ms flash is over (ordinary terminals).
 */
void tick(float dt) {
  uint32_t t = now();
  for (Knob &k : knobs) {
    // Hold a key for 0.3 s and the knob sweeps: the whole range in
    // 2.5 s, or 20 s with Shift.
    if (k.held && t - k.since > 300) {
      k.value += k.held * (k.fine ? 0.05f : 0.4f) * dt;
      k.value = k.value < 0.0f ? 0.0f : (k.value > 1.0f ? 1.0f : k.value);
    }
  }
  if (!kitty)
    for (Switch &s : switches)
      if (s.momentary && s.on && t - s.lastEvent > 150)
        s.on = false;
}

// ── Drawing ─────────────────────────────────────────────────────────

// Colours: ESC [ n m. 0 resets; 1 bright; 2 dim; 7 swaps foreground and
// background; 30-37 set the foreground (31 red, 32 green, 33 yellow,
// 36 cyan).
const char *const RESET = "\x1b[0m";
const char *const DIM = "\x1b[2m";
const char *const INVERSE = "\x1b[7m";
const char *const GREEN = "\x1b[32m";
const char *const YELLOW = "\x1b[33m";
const char *const CYAN = "\x1b[36m";
const char *const LIT = "\x1b[1;33m";

/**
 * Append the braille character for a 2 x 4 block of dots.
 *
 * Unicode's braille block starts at U+2800 and uses the low 8 bits of
 * the code as the dots: bits 0-2 the left column, top to bottom; bits
 * 3-5 the right column; bit 6 bottom-left; bit 7 bottom-right (dots 7
 * and 8 were added later, hence the odd order).
 *
 * In UTF-8, U+2800 + b is three bytes: E2, A0 + (b >> 6), 80 + (b & 3F).
 */
void braille(std::string &s, uint8_t b) {
  s += (char)0xE2;
  s += (char)(0xA0 + (b >> 6));
  s += (char)(0x80 + (b & 0x3F));
}

/** The dot bits for the pixel at (dx, dy) within a braille cell. */
const uint8_t kDot[4][2] = {
    {0x01, 0x08}, {0x02, 0x10}, {0x04, 0x20}, {0x40, 0x80}};

/** Append n spaces. */
void pad(std::string &s, int n) {
  while (n-- > 0)
    s += ' ';
}

/** Append text cut or padded to exactly `width` columns (ASCII only). */
void field(std::string &s, const char *text, int width) {
  int len = text ? (int)strlen(text) : 0;
  if (len > width)
    len = width;
  s.append(text ? text : "", len);
  pad(s, width - len);
}

/**
 * The page numbers, "[1234]": the page on screen bright and not
 * inverted, pages with nothing labelled on them dim. The brackets hint
 * at the [ and ] keys. ESC [ 27 m and 7 m turn inverse off and on;
 * 22 m ends bright or dim.
 */
void pageField(std::string &s) {
  s += '[';
  for (int p = 0; p < tui::PAGES; p++) {
    bool used = false;
    for (int i = 0; i < tui::KNOBS_PER_PAGE; i++)
      used |= knobs[p * tui::KNOBS_PER_PAGE + i].label != nullptr;
    for (int i = 0; i < tui::SWITCHES_PER_PAGE; i++)
      used |= switches[p * tui::SWITCHES_PER_PAGE + i].label != nullptr;
    if (p == page)
      s += "\x1b[27;1m";
    else if (!used)
      s += "\x1b[2m";
    s += (char)('1' + p);
    s += "\x1b[22;7m";
  }
  s += ']';
}

/** The title bar: name, help or status, page, CPU and the MIDI light. */
std::string titleBar() {
  // 1. The left: name, then a status message for 5 s after it is set,
  //    or the key help.
  std::string s = INVERSE;
  s += ' ';
  field(s, title, 12);
  s += ' ';
  const char *help = "up/dn item lt/rt value ^R rec ^C quit";
  // While recording, a flashing red dot, the length so far and the
  // file name take the place of the help and status. ESC [ 31 m is
  // red; 39 m goes back to the default colour.
  float seconds;
  const char *file;
  bool fresh = statusText[0] && now() - statusTime < 5000;
  if (detail::recordInfo(seconds, file)) {
    s += (now() / 500) % 2 ? "\x1b[31m\u25CF\x1b[39m" : " ";
    char rec[64];
    snprintf(rec, sizeof(rec), " REC %d:%04.1f %s", (int)seconds / 60,
             fmodf(seconds, 60.0f), file);
    field(s, rec, 37);
  } else {
    field(s, fresh ? statusText : help, 38);
  }
  s += ' '; // a gap, however long the text
  pageField(s);
  s += ' ';

  // 2. The right: CPU now and at its worst, and MIDI, lit for 200 ms
  //    after each message.
  static uint32_t lastCount = 0, lastMidi = 0;
  uint32_t count = detail::midiCount.load();
  if (count != lastCount) {
    lastCount = count;
    lastMidi = now();
  }
  char cpu[32];
  snprintf(cpu, sizeof(cpu), "CPU %4.1f%% pk%4.1f%%",
           AudioEngine::cpuLoad() * 100.0f, AudioEngine::cpuPeak() * 100.0f);
  field(s, cpu, 17);
  s += now() - lastMidi < 200 && lastMidi ? "●" : " ";
  s += "M ";
  s += RESET;
  return s;
}

/**
 * The display, 16 rows of 64 braille cells, indented to the middle.
 * The top 16 pixels are yellow and the rest cyan, like the common
 * two-colour 0.96" OLEDs.
 */
void drawScreen(std::vector<std::string> &lines) {
  for (int cy = 0; cy < 16; cy++) {
    std::string s;
    pad(s, 7);
    s += DIM;
    s += "│";
    s += RESET;
    s += cy < 4 ? YELLOW : CYAN;
    for (int cx = 0; cx < 64; cx++) {
      uint8_t b = 0;
      for (int dy = 0; dy < 4; dy++)
        for (int dx = 0; dx < 2; dx++)
          if (screen.get(2 * cx + dx, 4 * cy + dy))
            b |= kDot[dy][dx];
      braille(s, b);
    }
    s += RESET;
    s += DIM;
    s += "│";
    s += RESET;
    lines.push_back(s);
  }
}

/**
 * The oscilloscope: the last 640 output samples (14.5 ms) across 160 x 16
 * braille dots.
 *
 * 1. Trigger: start the picture where the wave crosses zero going up,
 *    as a hardware scope does. Each frame then starts at the same point
 *    of the cycle, so a steady tone stands still instead of rolling.
 * 2. Each of the 160 dot columns covers 4 samples; draw a vertical line
 *    from their lowest to their highest. A high tone becomes a band,
 *    rather than a scatter of dots that misses the peaks.
 */
void drawScope(std::vector<std::string> &lines) {
  const int kWidth = 160, kHeight = 16, kPer = 4;
  const uint32_t kSpan = kWidth * kPer;
  using detail::scope;
  using detail::SCOPE_SIZE;
  uint32_t n = detail::scopeWritten.load(std::memory_order_acquire);

  // 1. Find the newest rising zero crossing that leaves a whole picture
  //    after it, searching back over the history.
  uint32_t start = n - kSpan;
  for (uint32_t i = n - kSpan; i > n - SCOPE_SIZE + 1; i--) {
    if (scope[(i - 1) % SCOPE_SIZE] < 0.0f && scope[i % SCOPE_SIZE] >= 0.0f) {
      start = i;
      break;
    }
  }

  // 2. Columns of dots, into a 160 x 16 grid.
  bool dots[kHeight][kWidth] = {};
  for (int x = 0; x < kWidth; x++) {
    float lo = 1.0f, hi = -1.0f;
    for (int k = 0; k < kPer; k++) {
      float v = scope[(start + x * kPer + k) % SCOPE_SIZE];
      lo = v < lo ? v : lo;
      hi = v > hi ? v : hi;
    }
    // +1 at the top row (0), -1 at the bottom (kHeight - 1).
    auto row = [](float v) {
      int r = (int)((1.0f - v) * 0.5f * (kHeight - 1) + 0.5f);
      return r < 0 ? 0 : (r >= kHeight ? kHeight - 1 : r);
    };
    for (int y = row(hi); y <= row(lo); y++)
      dots[y][x] = true;
  }

  // 3. Into braille: 4 text rows of 80 cells.
  for (int cy = 0; cy < kHeight / 4; cy++) {
    std::string s = GREEN;
    for (int cx = 0; cx < kWidth / 2; cx++) {
      uint8_t b = 0;
      for (int dy = 0; dy < 4; dy++)
        for (int dx = 0; dx < 2; dx++)
          if (dots[4 * cy + dy][2 * cx + dx])
            b |= kDot[dy][dx];
      braille(s, b);
    }
    s += RESET;
    lines.push_back(s);
  }
}

/**
 * The ANSI code for an LED colour.
 *
 * The colour is a hue: 0-1 around the colour wheel, red, yellow, green,
 * cyan, blue, magenta and back to red. Hue to red-green-blue: the wheel
 * has six sixths, and in each one channel is full, one is empty and one
 * ramps up or down. Then pick the nearest of the 256-colour palette's
 * 6 x 6 x 6 cube (ESC [ 38 ; 5 ; n m), which nearly every terminal
 * shows; 24-bit colour codes would be finer but aren't everywhere.
 */
std::string ledColour(float colour) {
  float h = (colour >= 1.0f ? 0.0f : colour) * 6.0f; // 0 to 6
  int sixth = (int)h;
  float f = h - sixth; // how far through this sixth
  float r = 0, g = 0, b = 0;
  switch (sixth) {
  case 0:
    r = 1;
    g = f;
    break; // red to yellow
  case 1:
    r = 1 - f;
    g = 1;
    break; // yellow to green
  case 2:
    g = 1;
    b = f;
    break; // green to cyan
  case 3:
    g = 1 - f;
    b = 1;
    break; // cyan to blue
  case 4:
    r = f;
    b = 1;
    break; // blue to magenta
  default:
    r = 1;
    b = 1 - f;
    break; // magenta to red
  }
  // Each channel to 0-5, the cube's steps; the cube starts at 16.
  int n = 16 + 36 * (int)(r * 5 + 0.5f) + 6 * (int)(g * 5 + 0.5f) +
          (int)(b * 5 + 0.5f);
  return "\x1b[1;38;5;" + std::to_string(n) + "m";
}

/**
 * The LED row: 16 cells of 5 columns, " 1 ● ", the number dim, the LED
 * lit in its colour or a dim ring when off. Flashing LEDs are lit for
 * the first 250 ms of each half-second.
 */
void drawLeds(std::vector<std::string> &lines) {
  if (!ledsUsed)
    return;
  bool flashOn = (now() / 250) % 2 == 0;
  std::string s;
  for (int i = 0; i < tui::LEDS; i++) {
    const Led &led = leds[i];
    char number[4];
    snprintf(number, sizeof(number), "%2d", i + 1);
    s += DIM;
    s += number;
    s += RESET;
    s += ' ';
    bool lit = led.colour > 0.0f && (!led.flash || flashOn);
    s += lit ? ledColour(led.colour) : std::string(DIM);
    s += lit ? "●" : "○"; // ● or ○
    s += RESET;
    s += ' ';
  }
  lines.push_back(s);
}

/**
 * The page's two rows of ten switches: "1●Label ", the key dim, the
 * light lit.
 */
void drawSwitches(std::vector<std::string> &lines) {
  for (int row = 0; row < 2; row++) {
    std::string s;
    for (int i = row * 10; i < row * 10 + 10; i++) {
      const Switch &sw = switches[page * tui::SWITCHES_PER_PAGE + i];
      s += DIM;
      s += kKeys[i];
      s += RESET;
      s += sw.on ? LIT : DIM;
      s += sw.on ? "●" : "○"; // ● or ○
      s += RESET;
      if (!sw.label)
        s += DIM;
      field(s, sw.label, 5);
      s += RESET;
      s += ' ';
    }
    lines.push_back(s);
  }
}

/**
 * The page's ten knobs, three rows each: label; a bar of 7 cells using the
 * eighth blocks (▏▎▍▌▋▊▉█), so it moves in 56 steps; then the keys and the
 * value, 0-100.
 */
void drawKnobs(std::vector<std::string> &lines) {
  static const char *const kEighths[8] = {"▏", "▎", "▍", "▌",
                                          "▋", "▊", "▉", "█"};
  std::string labels, bars, keys;
  for (int i = 0; i < tui::KNOBS_PER_PAGE; i++) {
    const Knob &k = knobs[page * tui::KNOBS_PER_PAGE + i];
    if (!k.label)
      labels += DIM;
    field(labels, k.label ? k.label : "-", 7);
    labels += RESET;
    pad(labels, 1);

    int eighths = (int)(k.value * 56.0f + 0.5f);
    bars += k.label ? CYAN : DIM;
    for (int c = 0; c < 7; c++) {
      int fill = eighths - 8 * c; // eighths in this cell
      if (fill >= 8)
        bars += kEighths[7];
      else if (fill > 0)
        bars += kEighths[fill - 1];
      else
        bars += "·"; // · an empty cell
    }
    bars += RESET;
    pad(bars, 1);

    char value[8];
    snprintf(value, sizeof(value), "%3d", (int)(k.value * 100.0f + 0.5f));
    keys += DIM;
    keys += kKeys[20 + i];
    keys += '/';
    keys += kKeys[30 + i];
    keys += RESET;
    pad(keys, 1);
    keys += value;
    pad(keys, 1);
  }
  lines.push_back(labels);
  lines.push_back(bars);
  lines.push_back(keys);
}

/** Build the frame and send the lines that changed. */
void draw() {
  static std::vector<std::string> previous;
  std::vector<std::string> lines;
  lines.push_back(titleBar());
  if (screen.used())
    drawScreen(lines);
  drawScope(lines);
  drawLeds(lines);
  drawSwitches(lines);
  drawKnobs(lines);

  // After a resize, or when the layout grows (the display appearing),
  // clear the screen and redraw every line.
  std::string s;
  if (resized || lines.size() != previous.size()) {
    resized = 0;
    s += "\x1b[2J";
    previous.clear();
  }
  for (size_t r = 0; r < lines.size(); r++) {
    if (r < previous.size() && previous[r] == lines[r])
      continue;
    // Move to row r + 1, column 1 (rows count from 1), write the line,
    // and erase to the end of it (ESC [ K) in case the old one was
    // longer.
    s += "\x1b[" + std::to_string(r + 1) + ";1H" + lines[r] + "\x1b[K";
  }
  out(s);
  previous = lines;
}

} // namespace

// ── The public API (tui.h) ──────────────────────────────────────────

void tui::setTitle(const char *t) { title = t; }

void tui::setKnob(int i, const char *label, float value) {
  if (i < 0 || i >= KNOBS)
    return;
  knobs[i].label = label;
  knobs[i].value = value < 0.0f ? 0.0f : (value > 1.0f ? 1.0f : value);
}

float tui::knob(int i) { return i >= 0 && i < KNOBS ? knobs[i].value : 0.0f; }

void tui::setSwitch(int i, const char *label, bool momentary) {
  if (i < 0 || i >= SWITCHES)
    return;
  switches[i].label = label;
  switches[i].momentary = momentary;
}

bool tui::switchOn(int i) { return i >= 0 && i < SWITCHES && switches[i].on; }

bool tui::switchPressed(int i) {
  if (i < 0 || i >= SWITCHES || switches[i].presses == 0)
    return false;
  switches[i].presses--;
  return true;
}

void tui::setSwitchOn(int i, bool on) {
  if (i >= 0 && i < SWITCHES)
    switches[i].on = on;
}

int tui::menuMove() {
  int m = moves;
  moves = 0;
  return m;
}

int tui::menuAdjust() {
  int a = adjusts;
  adjusts = 0;
  return a;
}

bool tui::menuPressed() {
  if (spacePresses == 0)
    return false;
  spacePresses--;
  return true;
}

void tui::setLed(int i, float colour) {
  if (i < 0 || i >= LEDS)
    return;
  leds[i].colour = colour < 0.0f ? 0.0f : (colour > 1.0f ? 1.0f : colour);
  ledsUsed = true;
}

void tui::setLedFlash(int i, bool flash) {
  if (i < 0 || i >= LEDS)
    return;
  leds[i].flash = flash;
  ledsUsed = true;
}

Oled &tui::display() { return screen; }

void tui::status(const char *text) {
  snprintf(statusText, sizeof(statusText), "%s", text);
  statusTime = millis();
}

uint32_t tui::millis() {
  static const auto start = std::chrono::steady_clock::now();
  return (uint32_t)std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now() - start)
      .count();
}

} // namespace bb

/**
 * The program: set up the terminal, call setup() once, then loop() about
 * 1000 times a second, reading keys and redrawing between calls, until
 * Ctrl-C. With --devices, list the audio inputs and stop instead.
 */
int main(int argc, char **argv) {
  using namespace bb;
  if (argc > 1 && strcmp(argv[1], "--devices") == 0) {
    detail::listInputs();
    return 0;
  }
  if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
    fprintf(stderr, "buddhabox-tui needs a terminal\n");
    return 1;
  }
  tui::millis(); // start the clock
  terminalStart();
  setup();

  uint32_t lastFrame = 0, lastTick = tui::millis();
  while (!quit) {
    // 1. Keys, then held keys, then the program's loop().
    readKeys();
    uint32_t t = tui::millis();
    tick((t - lastTick) / 1000.0f);
    lastTick = t;
    ::loop();
    detail::recordUpdate(); // write recorded audio to disk

    // 2. A new frame every 33 ms: 30 a second.
    if (t - lastFrame >= 33) {
      lastFrame = t;
      draw();
    }
    // 3. Sleep a millisecond, so the loop doesn't spin a core at 100%.
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  AudioEngine::end();
  detail::recordEnd(); // after the audio stops: nothing more can arrive
  terminalEnd();
  return 0;
}
