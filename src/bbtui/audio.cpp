/**
 * @file audio.cpp
 * @brief The audio engine: CoreAudio's AudioQueue on macOS, ALSA on
 *        Linux. Both call render(), which calls your audioCallback().
 *
 * HOW THE TWO PLATFORMS DIFFER, AND WHY THESE APIs:
 * - macOS: AudioQueue (AudioToolbox). It runs its own real-time thread
 *   and converts our 44.1 kHz float stereo to whatever the device wants
 *   (48 kHz, mono microphones ...), so there is no conversion code
 *   here. The lower-level AudioUnit API would shave a few milliseconds
 *   of latency but leaves the sample-rate conversion to us.
 * - Linux: ALSA, the kernel's own audio API. Its "default" device goes
 *   through PipeWire or PulseAudio where they run, or straight to the
 *   card where they don't (a headless Raspberry Pi). ALSA has no
 *   callback thread of its own, so we make one: a loop that renders a
 *   block and writes it, and the write blocks until the card has room.
 *
 * INPUT: the input device runs on its own clock and thread, so its
 * samples cross to the output thread through a lock-free queue (ring.h).
 * Each output block takes one block from the queue, or silence if none
 * has arrived yet.
 */

#include <atomic>
#include <chrono>
#include <cstring>

#include "bbtui/internal.h"
#include "bbtui/ring.h"
#include "bbtui/tui.h"
#include "buddhabox/core/dsp_common.h"

namespace bb {

namespace detail {
float scope[SCOPE_SIZE];
std::atomic<uint32_t> scopeWritten{0};
} // namespace detail

namespace {

const int kBlock = AUDIO_BLOCK_SIZE; // stereo frames per callback

// Input samples (interleaved stereo) waiting for the output thread.
// 8 blocks: enough slack for the two threads' timing to wobble.
Ring<float, 8 * 2 * AUDIO_BLOCK_SIZE> inputQueue;

std::atomic<float> load{0.0f}, peak{0.0f};
std::atomic<bool> running{false};

/**
 * Make one block of output: gather input, time the user's callback,
 * and copy the result to the oscilloscope. Runs on the audio thread.
 *
 * @param out  Interleaved stereo, kBlock frames
 */
void render(float *out) {
  // 1. Input: a block from the queue, or silence. If the input clock
  //    runs faster than the output's, the queue slowly fills; dropping
  //    the oldest samples when it holds more than 4 blocks stops the
  //    delay between input and output growing without end.
  // ponytail: drops samples to bound latency (a small click every
  // minute or so); resample against the drift if that is ever audible.
  static float in[2 * AUDIO_BLOCK_SIZE];
  float skip;
  while (inputQueue.size() > 4 * 2 * kBlock)
    inputQueue.pop(skip);
  if (inputQueue.size() >= 2 * kBlock) {
    for (int i = 0; i < 2 * kBlock; i++)
      inputQueue.pop(in[i]);
  } else {
    memset(in, 0, sizeof(in));
  }

  // 2. The user's callback, timed. The load is the share of the block's
  //    duration (256 / 44100 s, 5.8 ms) spent computing it.
  auto start = std::chrono::steady_clock::now();
  audioCallback(in, out, kBlock);
  float seconds =
      std::chrono::duration<float>(std::chrono::steady_clock::now() - start)
          .count();
  float now = seconds * SAMPLE_RATE / kBlock;
  // A one-pole smoother: each block moves the reading 5% of the way to
  // the new value, so the meter settles over about 20 blocks (0.1 s).
  load.store(load.load() + 0.05f * (now - load.load()));
  if (now > peak.load())
    peak.store(now);

  // 3. The recorder (if on) gets the block; the oscilloscope gets the
  //    mono mix.
  detail::recordPush(out, kBlock);
  uint32_t n = detail::scopeWritten.load(std::memory_order_relaxed);
  for (int i = 0; i < kBlock; i++)
    detail::scope[(n + i) % detail::SCOPE_SIZE] =
        0.5f * (out[2 * i] + out[2 * i + 1]);
  detail::scopeWritten.store(n + kBlock, std::memory_order_release);
}

} // namespace

} // namespace bb

// ═════════════════════════════════════════════════════════════════════
#if defined(__APPLE__)

#include <AudioToolbox/AudioToolbox.h>

namespace bb {
namespace {

AudioQueueRef outQueue = nullptr, inQueue = nullptr;
int inChannels = 2;

/**
 * AudioQueue calls this on its own thread when it has played a buffer
 * and wants it refilled. We render into it and hand it back.
 */
void outputCallback(void *, AudioQueueRef queue, AudioQueueBufferRef buf) {
  render((float *)buf->mAudioData);
  buf->mAudioDataByteSize = 2 * kBlock * sizeof(float);
  AudioQueueEnqueueBuffer(queue, buf, 0, nullptr);
}

/**
 * AudioQueue calls this when it has filled an input buffer. Copy the
 * samples into the queue (doubling a mono input to stereo) and hand the
 * buffer back to be filled again.
 */
void inputCallback(void *, AudioQueueRef queue, AudioQueueBufferRef buf,
                   const AudioTimeStamp *, UInt32,
                   const AudioStreamPacketDescription *) {
  const float *s = (const float *)buf->mAudioData;
  int frames = buf->mAudioDataByteSize / (inChannels * sizeof(float));
  for (int i = 0; i < frames; i++) {
    float l = s[i * inChannels];
    float r = inChannels == 2 ? s[i * 2 + 1] : l;
    inputQueue.push(l);
    inputQueue.push(r);
  }
  AudioQueueEnqueueBuffer(queue, buf, 0, nullptr);
}

/** Our format: 44.1 kHz, 32-bit float, interleaved. */
AudioStreamBasicDescription format(int channels) {
  AudioStreamBasicDescription f = {};
  f.mSampleRate = SAMPLE_RATE;
  f.mFormatID = kAudioFormatLinearPCM;
  f.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
  f.mBitsPerChannel = 32;
  f.mChannelsPerFrame = channels;
  f.mFramesPerPacket = 1;
  f.mBytesPerFrame = f.mBytesPerPacket = channels * sizeof(float);
  return f;
}

/** Start the input queue: stereo if the device allows, else mono. */
bool startInput() {
  for (inChannels = 2; inChannels >= 1; inChannels--) {
    AudioStreamBasicDescription f = format(inChannels);
    if (AudioQueueNewInput(&f, inputCallback, nullptr, nullptr, nullptr, 0,
                           &inQueue) == noErr)
      break;
  }
  if (!inQueue)
    return false;
  for (int i = 0; i < 3; i++) {
    AudioQueueBufferRef buf;
    AudioQueueAllocateBuffer(inQueue, inChannels * kBlock * sizeof(float),
                             &buf);
    AudioQueueEnqueueBuffer(inQueue, buf, 0, nullptr);
  }
  return AudioQueueStart(inQueue, nullptr) == noErr;
}

} // namespace

bool AudioEngine::begin(bool input) {
  // 1. Output: three buffers of one block each. While the card plays
  //    one, the next waits and we fill the third, about 17 ms in all.
  //    A null run loop means "call back on your own thread".
  AudioStreamBasicDescription f = format(2);
  if (AudioQueueNewOutput(&f, outputCallback, nullptr, nullptr, nullptr, 0,
                          &outQueue) != noErr) {
    tui::status("no audio output");
    return false;
  }
  if (input && !startInput())
    tui::status("no audio input");
  // 2. Fill all three buffers before starting, so the card has sound
  //    queued from the first moment.
  for (int i = 0; i < 3; i++) {
    AudioQueueBufferRef buf;
    AudioQueueAllocateBuffer(outQueue, 2 * kBlock * sizeof(float), &buf);
    outputCallback(nullptr, outQueue, buf);
  }
  running = AudioQueueStart(outQueue, nullptr) == noErr;
  return running;
}

void AudioEngine::end() {
  // true = stop now, rather than after the queued buffers play.
  if (inQueue)
    AudioQueueDispose(inQueue, true);
  if (outQueue)
    AudioQueueDispose(outQueue, true);
  inQueue = outQueue = nullptr;
  running = false;
}

} // namespace bb

// ═════════════════════════════════════════════════════════════════════
#else // Linux

#include <alsa/asoundlib.h>
#include <cstdlib>
#include <pthread.h>
#include <thread>

namespace bb {
namespace {

snd_pcm_t *outPcm = nullptr, *inPcm = nullptr;
std::thread outThread, inThread;

/**
 * Open an ALSA device for 44.1 kHz float stereo.
 *
 * snd_pcm_set_params() does the whole hardware setup in one call. The
 * 1 (soft_resample) lets ALSA convert the rate if the card can't do
 * 44.1 kHz; 30000 us is the total buffer, about 5 blocks: short enough
 * to play live, long enough to ride out a busy moment on a Pi.
 *
 * @param envName  Environment variable naming the device, if set
 * @param stream   SND_PCM_STREAM_PLAYBACK or SND_PCM_STREAM_CAPTURE
 */
snd_pcm_t *openPcm(const char *envName, snd_pcm_stream_t stream) {
  const char *name = getenv(envName);
  snd_pcm_t *pcm = nullptr;
  if (snd_pcm_open(&pcm, name ? name : "default", stream, 0) < 0)
    return nullptr;
  if (snd_pcm_set_params(pcm, SND_PCM_FORMAT_FLOAT,
                         SND_PCM_ACCESS_RW_INTERLEAVED, 2, SAMPLE_RATE, 1,
                         30000) < 0) {
    snd_pcm_close(pcm);
    return nullptr;
  }
  return pcm;
}

/**
 * Ask for real-time scheduling, so the desktop or a busy terminal can't
 * delay the audio thread. Needs permission (root, or an rtprio limit in
 * /etc/security/limits.conf); without it the call fails and the thread
 * runs at normal priority, which is usually fine.
 */
void realtime() {
  sched_param p = {};
  p.sched_priority = 70;
  pthread_setschedparam(pthread_self(), SCHED_FIFO, &p);
}

/** The output thread: render a block, write it, repeat. */
void outputLoop() {
  realtime();
  static float buf[2 * AUDIO_BLOCK_SIZE];
  while (running) {
    render(buf);
    // writei blocks until the card has room: that wait is the clock.
    snd_pcm_sframes_t n = snd_pcm_writei(outPcm, buf, kBlock);
    if (n < 0) // an underrun (-EPIPE) or a suspend: recover and go on
      snd_pcm_recover(outPcm, (int)n, 1);
  }
}

/** The input thread: read a block, queue it, repeat. */
void inputLoop() {
  realtime();
  static float buf[2 * AUDIO_BLOCK_SIZE];
  while (running) {
    snd_pcm_sframes_t n = snd_pcm_readi(inPcm, buf, kBlock);
    if (n < 0) {
      snd_pcm_recover(inPcm, (int)n, 1);
      continue;
    }
    for (int i = 0; i < 2 * n; i++)
      inputQueue.push(buf[i]);
  }
}

} // namespace

bool AudioEngine::begin(bool input) {
  outPcm = openPcm("BB_AUDIO_OUT", SND_PCM_STREAM_PLAYBACK);
  if (!outPcm) {
    tui::status("no audio output (try BB_AUDIO_OUT=plughw:0)");
    return false;
  }
  if (input) {
    inPcm = openPcm("BB_AUDIO_IN", SND_PCM_STREAM_CAPTURE);
    if (!inPcm)
      tui::status("no audio input (try BB_AUDIO_IN=plughw:1)");
  }
  running = true;
  outThread = std::thread(outputLoop);
  if (inPcm)
    inThread = std::thread(inputLoop);
  return true;
}

void AudioEngine::end() {
  if (!running)
    return;
  running = false;
  // The output thread wakes within a block; the input thread may sit in
  // readi until the next block arrives, which is just as quick.
  if (outThread.joinable())
    outThread.join();
  if (inThread.joinable())
    inThread.join();
  if (outPcm)
    snd_pcm_close(outPcm);
  if (inPcm)
    snd_pcm_close(inPcm);
  outPcm = inPcm = nullptr;
}

} // namespace bb

#endif

// ═════════════════════════════════════════════════════════════════════
namespace bb {
float AudioEngine::cpuLoad() { return load.load(); }
float AudioEngine::cpuPeak() { return peak.load(); }
} // namespace bb
