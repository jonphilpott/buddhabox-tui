/**
 * @file internal.h
 * @brief What the library's own .cpp files share, and programs needn't
 *        see: the oscilloscope's sample history, the MIDI activity
 *        count, and the WAV recorder (recorder.cpp).
 */

#pragma once

#include <atomic>
#include <cstdint>

namespace bb {
namespace detail {

/**
 * The last SCOPE_SIZE output samples (left and right averaged), written
 * by the audio thread and drawn by the panel. A circular buffer: sample
 * n lives at scope[n % SCOPE_SIZE], and scopeWritten counts every
 * sample ever written.
 *
 * The panel reads it while the audio thread writes it, without a lock.
 * At worst one frame of the scope shows a few samples from the next
 * block: harmless for a picture, and a lock would risk the audio.
 */
constexpr uint32_t SCOPE_SIZE = 4096;
extern float scope[SCOPE_SIZE];
extern std::atomic<uint32_t> scopeWritten;

/** Every MIDI message received, counted, for the panel's MIDI light. */
extern std::atomic<uint32_t> midiCount;

/** Audio thread: queue a block of output for the recorder, if it is
 *  recording. Interleaved stereo, `frames` frames. */
void recordPush(const float *out, int frames);

/** Print the audio input devices and their channel counts, for
 *  --devices. Runs before the panel starts. */
void listInputs();

/** Main thread: start a new file named after `title`, or finish the
 *  one being recorded. */
void recordToggle(const char *title);

/** Main thread, every loop: write queued samples to the file. */
void recordUpdate();

/** Main thread, on exit: finish the file if one is open. */
void recordEnd();

/**
 * Main thread: what is being recorded.
 * @param seconds  Filled with the length so far
 * @param name     Filled with the file name
 * @return         false when not recording
 */
bool recordInfo(float &seconds, const char *&name);

} // namespace detail
} // namespace bb
