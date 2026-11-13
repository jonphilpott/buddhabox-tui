/**
 * @file internal.h
 * @brief What the library's own .cpp files share, and programs needn't
 *        see: the oscilloscope's sample history and the MIDI activity
 *        count.
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

} // namespace detail
} // namespace bb
