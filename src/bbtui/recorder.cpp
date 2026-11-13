/**
 * @file recorder.cpp
 * @brief Recording the output to WAV files: Ctrl-R starts a new file,
 *        Ctrl-R again finishes it.
 *
 * WHY TWO THREADS: writing to disk can stall for milliseconds (a busy
 * disk, a slow SD card on a Pi), and the audio thread must never wait.
 * So the audio thread only copies each block into a lock-free queue
 * (ring.h), and the main thread drains the queue to the file between
 * calls to loop(), about 1000 times a second.
 *
 * THE FILE: 16-bit PCM WAV, 44.1 kHz stereo, the format every player and
 * editor opens, and the board's DAC resolution. A WAV file is a 44-byte
 * header and then the samples. The header holds the data's length,
 * which isn't known until recording stops, so it is written first with
 * zeros and filled in at the end.
 */

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>

#include "bbtui/internal.h"
#include "bbtui/ring.h"
#include "bbtui/tui.h"
#include "buddhabox/core/dsp_common.h"

namespace bb {
namespace {

// About 1.5 s of stereo samples: the main thread drains it every
// millisecond or so, so this only fills if the main thread stalls.
Ring<float, 131072> queue;
std::atomic<bool> recording{false};
std::atomic<uint32_t> dropped{0}; // samples lost to a full queue

FILE *file = nullptr;
std::string fileName;
uint32_t framesWritten = 0;

/** Write a 16- or 32-bit number, least significant byte first, as WAV
 *  wants whatever the processor's own order. */
void put(uint8_t *p, uint32_t v, int bytes) {
  for (int i = 0; i < bytes; i++)
    p[i] = (v >> (8 * i)) & 0xFF;
}

/**
 * The 44-byte header for `frames` stereo 16-bit frames.
 *
 * Layout: "RIFF", the size of everything after these 8 bytes, "WAVE";
 * then the "fmt " chunk (format 1 = PCM, channels, sample rate, bytes per
 * second, bytes per frame, bits per sample); then the "data" chunk's
 * name and size, after which the samples follow.
 */
void header(uint8_t *h, uint32_t frames) {
  const uint32_t channels = 2, bits = 16;
  const uint32_t frameBytes = channels * bits / 8; // 4
  const uint32_t dataBytes = frames * frameBytes;
  memcpy(h, "RIFF", 4);
  put(h + 4, 36 + dataBytes, 4);
  memcpy(h + 8, "WAVEfmt ", 8);
  put(h + 16, 16, 4); // the fmt chunk's size
  put(h + 20, 1, 2);  // PCM
  put(h + 22, channels, 2);
  put(h + 24, SAMPLE_RATE, 4);
  put(h + 28, SAMPLE_RATE * frameBytes, 4);
  put(h + 32, frameBytes, 2);
  put(h + 34, bits, 2);
  memcpy(h + 36, "data", 4);
  put(h + 40, dataBytes, 4);
}

/**
 * A new file name from the program's title and the time:
 * "acid-20261113-101500.wav", with "-2", "-3" ... added if that exists
 * (two recordings in the same second).
 */
std::string newName(const char *title) {
  // Keep letters and digits from the title, in lower case.
  std::string base;
  for (const char *c = title; *c; c++)
    if (isalnum((unsigned char)*c))
      base += (char)tolower((unsigned char)*c);
  if (base.empty())
    base = "recording";
  char stamp[32];
  time_t t = time(nullptr);
  strftime(stamp, sizeof(stamp), "-%Y%m%d-%H%M%S", localtime(&t));
  base += stamp;
  std::string name = base + ".wav";
  for (int n = 2; FILE *f = fopen(name.c_str(), "r"); n++) {
    fclose(f);
    name = base + "-" + std::to_string(n) + ".wav";
  }
  return name;
}

/** Write whatever waits in the queue to the file, as 16-bit samples. */
void drain() {
  int16_t buf[1024]; // 512 frames per write
  int n = 0;
  float v;
  while (queue.pop(v)) {
    // Clamp as the sound card would, then scale to 16 bits.
    v = v < -1.0f ? -1.0f : (v > 1.0f ? 1.0f : v);
    buf[n++] = (int16_t)(v * 32767.0f);
    if (n == 1024) {
      fwrite(buf, sizeof(int16_t), n, file);
      framesWritten += n / 2;
      n = 0;
    }
  }
  // ponytail: writes the host's byte order; every target here (x86,
  // ARM) is little-endian, as WAV is.
  fwrite(buf, sizeof(int16_t), n, file);
  framesWritten += n / 2;
}

/** Stop: let the audio thread see it, write the rest, fix the header. */
void stop() {
  recording = false;
  drain();
  uint8_t h[44];
  header(h, framesWritten);
  fseek(file, 0, SEEK_SET);
  fwrite(h, 1, sizeof(h), file);
  fclose(file);
  file = nullptr;

  char msg[96];
  float seconds = (float)framesWritten / SAMPLE_RATE;
  uint32_t lost = dropped.exchange(0);
  // The length first: if the title bar cuts the message short, it cuts
  // the end of the file name, not the length.
  if (lost)
    snprintf(msg, sizeof(msg), "saved %d:%04.1f (%u lost) %s",
             (int)seconds / 60, fmodf(seconds, 60.0f), lost,
             fileName.c_str());
  else
    snprintf(msg, sizeof(msg), "saved %d:%04.1f %s", (int)seconds / 60,
             fmodf(seconds, 60.0f), fileName.c_str());
  tui::status(msg);
}

} // namespace

void detail::recordPush(const float *out, int frames) {
  if (!recording.load(std::memory_order_relaxed))
    return;
  for (int i = 0; i < 2 * frames; i++)
    if (!queue.push(out[i]))
      dropped++;
}

void detail::recordToggle(const char *title) {
  if (file) {
    stop();
    return;
  }
  // Empty the queue first: the audio thread may have pushed the end of
  // a block just after the last recording stopped.
  float v;
  while (queue.pop(v)) {
  }
  fileName = newName(title);
  file = fopen(fileName.c_str(), "wb");
  if (!file) {
    tui::status(("can't write " + fileName).c_str());
    return;
  }
  uint8_t h[44];
  header(h, 0); // a placeholder until stop() knows the length
  fwrite(h, 1, sizeof(h), file);
  framesWritten = 0;
  dropped = 0;
  recording = true;
}

void detail::recordUpdate() {
  if (file)
    drain();
}

void detail::recordEnd() {
  if (file)
    stop();
}

bool detail::recordInfo(float &seconds, const char *&name) {
  if (!file)
    return false;
  seconds = (float)framesWritten / SAMPLE_RATE;
  name = fileName.c_str();
  return true;
}

} // namespace bb
