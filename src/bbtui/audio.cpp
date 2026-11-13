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
#include <cstdio>
#include <cstdlib>
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

// THE INPUT CHANNELS. The device is opened with `inOpen` channels, and
// the input callback picks two of them, `inLeft` and `inRight`
// (counting from 0), as the program's stereo input. -1 means the
// program didn't choose: use the device's first two, or its only one.
int inLeft = -1, inRight = -1, inOpen = 2;

/**
 * Note which channels the program asked for (counting from 1, as on an
 * interface's front panel). BB_AUDIO_IN_CHANNELS, "3" or "3,4",
 * overrides the program, so one setting in a shell profile serves
 * every program.
 */
void chooseChannels(int left, int right) {
  if (const char *e = getenv("BB_AUDIO_IN_CHANNELS")) {
    left = atoi(e);
    const char *comma = strchr(e, ',');
    right = comma ? atoi(comma + 1) : 0;
  }
  if (left < 1) // not chosen
    return;
  if (right < 1) // one channel: mono, to both sides
    right = left;
  inLeft = left - 1;
  inRight = right - 1;
}

/**
 * The status line's report on the input, e.g. "input: ZOOM L6, 3" or
 * "input: plughw:1, 3+4", so it's clear which channels are live.
 */
void reportInput(const char *device) {
  char msg[80];
  if (inLeft == inRight)
    snprintf(msg, sizeof(msg), "input: %s, %d", device, inLeft + 1);
  else
    snprintf(msg, sizeof(msg), "input: %s, %d+%d", device, inLeft + 1,
             inRight + 1);
  tui::status(msg);
}

/** Push one frame of a device's input (`inOpen` interleaved channels)
 *  to the input queue as stereo. */
inline void pushFrame(const float *frame) {
  inputQueue.push(frame[inLeft]);
  inputQueue.push(frame[inRight]);
}

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
#include <CoreAudio/CoreAudio.h>
#include <string>
#include <vector>

namespace bb {
namespace {

AudioQueueRef outQueue = nullptr, inQueue = nullptr;

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
 * AudioQueue calls this when it has filled an input buffer: every one
 * of the device's channels, interleaved. Pick out the two in use, then
 * hand the buffer back to be filled again.
 */
void inputCallback(void *, AudioQueueRef queue, AudioQueueBufferRef buf,
                   const AudioTimeStamp *, UInt32,
                   const AudioStreamPacketDescription *) {
  const float *s = (const float *)buf->mAudioData;
  int frames = buf->mAudioDataByteSize / (inOpen * sizeof(float));
  for (int i = 0; i < frames; i++)
    pushFrame(s + i * inOpen);
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

// ── Devices ──────────────────────────────────────────────────────────
// CoreAudio describes everything as "objects" with "properties", each
// named by an address: what (the selector), which side (scope: input,
// output or global) and which element (Main = the whole object).

/** An address in the global scope. */
AudioObjectPropertyAddress address(AudioObjectPropertySelector what) {
  return {what, kAudioObjectPropertyScopeGlobal,
          kAudioObjectPropertyElementMain};
}

/** A string property (a name or a UID) as UTF-8. */
std::string stringProperty(AudioObjectID dev, AudioObjectPropertySelector p) {
  AudioObjectPropertyAddress a = address(p);
  CFStringRef s = nullptr;
  UInt32 size = sizeof(s);
  if (AudioObjectGetPropertyData(dev, &a, 0, nullptr, &size, &s) != noErr || !s)
    return "";
  char buf[256];
  CFStringGetCString(s, buf, sizeof(buf), kCFStringEncodingUTF8);
  CFRelease(s); // we own strings that a Get call hands back
  return buf;
}

/**
 * How many input channels a device has: the sum over its input streams
 * (an interface may present its 12 inputs as one stream or several).
 * 0 for an output-only device.
 */
int inputChannels(AudioObjectID dev) {
  AudioObjectPropertyAddress a = {kAudioDevicePropertyStreamConfiguration,
                                  kAudioObjectPropertyScopeInput,
                                  kAudioObjectPropertyElementMain};
  UInt32 size = 0;
  if (AudioObjectGetPropertyDataSize(dev, &a, 0, nullptr, &size) != noErr ||
      size == 0)
    return 0;
  // The answer is an AudioBufferList, whose length varies with the
  // number of streams; ask its size first, then fetch it.
  std::vector<char> mem(size);
  AudioBufferList *list = (AudioBufferList *)mem.data();
  if (AudioObjectGetPropertyData(dev, &a, 0, nullptr, &size, list) != noErr)
    return 0;
  int n = 0;
  for (UInt32 i = 0; i < list->mNumberBuffers; i++)
    n += list->mBuffers[i].mNumberChannels;
  return n;
}

/** Every audio device the system knows, inputs and outputs. */
std::vector<AudioObjectID> allDevices() {
  AudioObjectPropertyAddress a = address(kAudioHardwarePropertyDevices);
  UInt32 size = 0;
  AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &a, 0, nullptr,
                                 &size);
  std::vector<AudioObjectID> ids(size / sizeof(AudioObjectID));
  AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, nullptr, &size,
                             ids.data());
  return ids;
}

/** Lower case, for matching names whatever their capitals. */
std::string lower(std::string s) {
  for (char &c : s)
    c = (char)tolower((unsigned char)c);
  return s;
}

/**
 * The input device: the first whose name contains BB_AUDIO_IN (any
 * case), or the system's default input. 0 if none.
 */
AudioObjectID inputDevice() {
  if (const char *want = getenv("BB_AUDIO_IN")) {
    for (AudioObjectID d : allDevices())
      if (inputChannels(d) > 0 &&
          lower(stringProperty(d, kAudioObjectPropertyName))
                  .find(lower(want)) != std::string::npos)
        return d;
    return 0;
  }
  AudioObjectPropertyAddress a =
      address(kAudioHardwarePropertyDefaultInputDevice);
  AudioObjectID d = 0;
  UInt32 size = sizeof(d);
  AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, nullptr, &size,
                             &d);
  return d;
}

/**
 * Start the input queue.
 *
 * 1. Find the device and count its channels.
 * 2. Open the queue with all of them. The input callback picks out the
 *    two in use; asking AudioQueue for fewer channels than the device
 *    has would leave it to decide which ones to keep (or mix).
 * 3. Point the queue at the device by its UID, a fixed ID string, and
 *    start it with three blocks' worth of buffers.
 */
bool startInput() {
  // 1.
  AudioObjectID dev = inputDevice();
  int have = dev ? inputChannels(dev) : 0;
  if (have == 0) {
    tui::status(getenv("BB_AUDIO_IN") ? "no input matches BB_AUDIO_IN "
                                        "(--devices lists them)"
                                      : "no audio input");
    return false;
  }
  if (inLeft < 0) { // not chosen: the first two, or the only one
    inLeft = 0;
    inRight = have > 1 ? 1 : 0;
  }
  if (inLeft >= have || inRight >= have) {
    char msg[80];
    snprintf(msg, sizeof(msg), "input has %d channels; asked for %d", have,
             (inLeft > inRight ? inLeft : inRight) + 1);
    tui::status(msg);
    return false;
  }
  // 2.
  inOpen = have;
  AudioStreamBasicDescription f = format(have);
  if (AudioQueueNewInput(&f, inputCallback, nullptr, nullptr, nullptr, 0,
                         &inQueue) != noErr) {
    tui::status("can't open the audio input");
    return false;
  }
  // 3.
  std::string uid = stringProperty(dev, kAudioDevicePropertyDeviceUID);
  CFStringRef cfUid =
      CFStringCreateWithCString(nullptr, uid.c_str(), kCFStringEncodingUTF8);
  AudioQueueSetProperty(inQueue, kAudioQueueProperty_CurrentDevice, &cfUid,
                        sizeof(cfUid));
  CFRelease(cfUid);
  for (int i = 0; i < 3; i++) {
    AudioQueueBufferRef buf;
    AudioQueueAllocateBuffer(inQueue, have * kBlock * sizeof(float), &buf);
    AudioQueueEnqueueBuffer(inQueue, buf, 0, nullptr);
  }
  if (AudioQueueStart(inQueue, nullptr) != noErr) {
    tui::status("can't start the audio input");
    return false;
  }
  reportInput(stringProperty(dev, kAudioObjectPropertyName).c_str());
  return true;
}

} // namespace

bool AudioEngine::begin(bool input, int left, int right) {
  // 1. Output: three buffers of one block each. While the card plays
  //    one, the next waits and we fill the third, about 17 ms in all.
  //    A null run loop means "call back on your own thread".
  AudioStreamBasicDescription f = format(2);
  if (AudioQueueNewOutput(&f, outputCallback, nullptr, nullptr, nullptr, 0,
                          &outQueue) != noErr) {
    tui::status("no audio output");
    return false;
  }
  if (input) {
    chooseChannels(left, right);
    startInput(); // reports what it opened, or why it couldn't
  }
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

void detail::listInputs() {
  printf("Audio inputs (BB_AUDIO_IN=<part of the name>):\n");
  for (AudioObjectID d : allDevices())
    if (int n = inputChannels(d))
      printf("  %-40s %d channel%s\n",
             stringProperty(d, kAudioObjectPropertyName).c_str(), n,
             n == 1 ? "" : "s");
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
 * Open an ALSA device for 44.1 kHz float, `channels` interleaved.
 *
 * snd_pcm_set_params() does the whole hardware setup in one call. The
 * 1 (soft_resample) lets ALSA convert the rate if the card can't do
 * 44.1 kHz; 30000 us is the total buffer, about 5 blocks: short enough
 * to play live, long enough to ride out a busy moment on a Pi.
 *
 * @param envName   Environment variable naming the device, if set
 * @param stream    SND_PCM_STREAM_PLAYBACK or SND_PCM_STREAM_CAPTURE
 * @param channels  How many channels: 2 for output; for input, enough
 *                  to reach the highest channel in use
 */
snd_pcm_t *openPcm(const char *envName, snd_pcm_stream_t stream, int channels) {
  const char *name = getenv(envName);
  snd_pcm_t *pcm = nullptr;
  if (snd_pcm_open(&pcm, name ? name : "default", stream, 0) < 0)
    return nullptr;
  if (snd_pcm_set_params(pcm, SND_PCM_FORMAT_FLOAT,
                         SND_PCM_ACCESS_RW_INTERLEAVED, channels, SAMPLE_RATE,
                         1, 30000) < 0) {
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

// The most input channels we open: the read buffer is sized for it.
const int kMaxIn = 32;

/** The input thread: read a block of every open channel, queue the two
 *  in use, repeat. */
void inputLoop() {
  realtime();
  static float buf[kMaxIn * AUDIO_BLOCK_SIZE];
  while (running) {
    snd_pcm_sframes_t n = snd_pcm_readi(inPcm, buf, kBlock);
    if (n < 0) {
      snd_pcm_recover(inPcm, (int)n, 1);
      continue;
    }
    for (int i = 0; i < n; i++)
      pushFrame(buf + i * inOpen);
  }
}

} // namespace

bool AudioEngine::begin(bool input, int left, int right) {
  outPcm = openPcm("BB_AUDIO_OUT", SND_PCM_STREAM_PLAYBACK, 2);
  if (!outPcm) {
    tui::status("no audio output (try BB_AUDIO_OUT=plughw:0)");
    return false;
  }
  if (input) {
    // Open just enough channels to reach the highest one in use: with a
    // plughw: device ALSA passes channels 1 to N straight through.
    chooseChannels(left, right);
    if (inLeft < 0) {
      inLeft = 0;
      inRight = 1;
    }
    inOpen = (inLeft > inRight ? inLeft : inRight) + 1;
    inOpen = inOpen < 2 ? 2 : inOpen;
    const char *name = getenv("BB_AUDIO_IN");
    inPcm = inOpen <= kMaxIn
                ? openPcm("BB_AUDIO_IN", SND_PCM_STREAM_CAPTURE, inOpen)
                : nullptr;
    if (inPcm)
      reportInput(name ? name : "default");
    else if (inOpen > 2)
      tui::status("can't open that many input channels (plughw:?)");
    else
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

void detail::listInputs() {
  printf("Audio inputs (BB_AUDIO_IN=<device>):\n");
  // Each card's hardware device, asked for its most capture channels.
  // A card in use by another program can't be opened and is skipped.
  int card = -1;
  while (snd_card_next(&card) == 0 && card >= 0) {
    char dev[16];
    snprintf(dev, sizeof(dev), "hw:%d", card);
    snd_pcm_t *pcm;
    unsigned int most = 0;
    if (snd_pcm_open(&pcm, dev, SND_PCM_STREAM_CAPTURE, SND_PCM_NONBLOCK) ==
        0) {
      snd_pcm_hw_params_t *hw;
      snd_pcm_hw_params_alloca(&hw);
      snd_pcm_hw_params_any(pcm, hw);
      snd_pcm_hw_params_get_channels_max(hw, &most);
      snd_pcm_close(pcm);
    }
    char *name = nullptr;
    snd_card_get_name(card, &name);
    if (most)
      printf("  plughw:%-3d %-40s %u channel%s\n", card, name ? name : "?",
             most, most == 1 ? "" : "s");
    free(name);
  }
}

} // namespace bb

#endif

// ═════════════════════════════════════════════════════════════════════
namespace bb {
float AudioEngine::cpuLoad() { return load.load(); }
float AudioEngine::cpuPeak() { return peak.load(); }
} // namespace bb
