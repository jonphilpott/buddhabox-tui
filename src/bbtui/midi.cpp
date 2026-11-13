/**
 * @file midi.cpp
 * @brief MIDI input: CoreMIDI on macOS, the ALSA sequencer on Linux.
 *
 * Both deliver messages on a thread of their own. Each message goes into
 * a lock-free queue (ring.h), and Midi::read() takes them out on the
 * main thread, in loop(). That keeps the threads apart: your program
 * only ever sees MIDI in loop().
 */

#include "bbtui/internal.h"
#include "bbtui/ring.h"
#include "bbtui/tui.h"

namespace bb {
std::atomic<uint32_t> detail::midiCount{0};

namespace {
Ring<Midi::Message, 256> queue;

/** Queue one message (called on the MIDI thread). */
void put(uint8_t status, uint8_t d1, uint8_t d2) {
  queue.push(Midi::Message{status, d1, d2});
  // Clock ticks (0xF8) arrive 24 times a beat: they'd keep the light on.
  if (status != 0xF8)
    detail::midiCount++;
}
} // namespace

bool Midi::read(Message &m) { return queue.pop(m); }
} // namespace bb

// ═════════════════════════════════════════════════════════════════════
#if defined(__APPLE__)

#include <CoreMIDI/CoreMIDI.h>

/**
 * CoreMIDI hands us an "event list" of Universal MIDI Packets (UMP), the
 * MIDI 2.0 wire format, even from MIDI 1.0 devices. Each packet is 1 to
 * 4 32-bit words; the top 4 bits of the first word give its type, and
 * the type gives its length. We want two types, both one word long:
 * - 1: system messages (clock, start, stop): status in bits 16-23
 * - 2: MIDI 1.0 channel messages (notes, CCs): status in bits 16-23,
 *      then the two data bytes in bits 8-15 and 0-7
 * Asking for kMIDIProtocol_1_0 keeps notes and CCs in type 2, rather
 * than MIDI 2.0's 64-bit type 4.
 */
bool bb::Midi::begin() {
  MIDIClientRef client;
  MIDIPortRef port;
  if (MIDIClientCreate(CFSTR("buddhabox-tui"), nullptr, nullptr, &client))
    return false;
  // The ^(...) { ... } is a "block": Apple's C/C++ extension for a
  // function written inline, like a lambda. CoreMIDI calls it on its own
  // thread for each event list.
  OSStatus err = MIDIInputPortCreateWithProtocol(
      client, CFSTR("in"), kMIDIProtocol_1_0, &port,
      ^(const MIDIEventList *list, void *) {
        // Words per packet, by type 0-15 (from the UMP specification).
        static const int kWords[16] = {1, 1, 1, 2, 2, 4, 1, 1,
                                       2, 2, 2, 3, 3, 4, 4, 4};
        const MIDIEventPacket *p = &list->packet[0];
        for (UInt32 n = 0; n < list->numPackets; n++) {
          for (UInt32 w = 0; w < p->wordCount;) {
            UInt32 word = p->words[w];
            int type = word >> 28;
            if (type == 1 || type == 2)
              put((word >> 16) & 0xFF, (word >> 8) & 0x7F, word & 0x7F);
            w += kWords[type];
          }
          p = MIDIEventPacketNext(p);
        }
      });
  if (err)
    return false;
  // Listen to every source there is now.
  // ponytail: devices plugged in later are missed; watch the client's
  // notifications (kMIDIMsgObjectAdded) and connect them if needed.
  for (ItemCount i = 0; i < MIDIGetNumberOfSources(); i++)
    MIDIPortConnectSource(port, MIDIGetSource(i), nullptr);
  return true;
}

// ═════════════════════════════════════════════════════════════════════
#else // Linux

#include <alsa/asoundlib.h>
#include <thread>

/**
 * The ALSA sequencer is the kernel's MIDI router: every USB keyboard,
 * every software synth's MIDI port, is a "client" with "ports". We make
 * a client with one writable port, subscribe it to every readable port
 * there is (as `aconnect` would), then wait for events on a thread.
 * The sequencer delivers events already parsed, so we turn the ones we
 * know back into MIDI bytes.
 */
bool bb::Midi::begin() {
  snd_seq_t *seq;
  if (snd_seq_open(&seq, "default", SND_SEQ_OPEN_INPUT, 0) < 0)
    return false;
  snd_seq_set_client_name(seq, "buddhabox-tui");
  int me = snd_seq_create_simple_port(
      seq, "in", SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE,
      SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
  if (me < 0)
    return false;

  // 1. Subscribe to every readable port of every other client. Client 0
  //    is the system's timer and announcements: skip it.
  // ponytail: devices plugged in later are missed; run
  // `aconnect <device> buddhabox-tui` or restart.
  snd_seq_client_info_t *client;
  snd_seq_port_info_t *port;
  snd_seq_client_info_alloca(&client);
  snd_seq_port_info_alloca(&port);
  const unsigned kReadable = SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ;
  snd_seq_client_info_set_client(client, -1);
  while (snd_seq_query_next_client(seq, client) >= 0) {
    int c = snd_seq_client_info_get_client(client);
    if (c == SND_SEQ_CLIENT_SYSTEM || c == snd_seq_client_id(seq))
      continue;
    snd_seq_port_info_set_client(port, c);
    snd_seq_port_info_set_port(port, -1);
    while (snd_seq_query_next_port(seq, port) >= 0) {
      unsigned caps = snd_seq_port_info_get_capability(port);
      if ((caps & kReadable) == kReadable)
        snd_seq_connect_from(seq, me, c, snd_seq_port_info_get_port(port));
    }
  }

  // 2. The listening thread. snd_seq_event_input() waits for the next
  //    event. The thread is detached: it ends when the program does.
  std::thread([seq] {
    snd_seq_event_t *ev;
    for (;;) {
      if (snd_seq_event_input(seq, &ev) < 0)
        continue;
      const snd_seq_ev_note_t &n = ev->data.note;
      const snd_seq_ev_ctrl_t &c = ev->data.control;
      switch (ev->type) {
      case SND_SEQ_EVENT_NOTEON:
        put(0x90 | n.channel, n.note, n.velocity);
        break;
      case SND_SEQ_EVENT_NOTEOFF:
        put(0x80 | n.channel, n.note, n.velocity);
        break;
      case SND_SEQ_EVENT_CONTROLLER:
        put(0xB0 | c.channel, c.param, c.value);
        break;
      case SND_SEQ_EVENT_PGMCHANGE:
        put(0xC0 | c.channel, c.value, 0);
        break;
      case SND_SEQ_EVENT_PITCHBEND: {
        int v = c.value + 8192; // -8192..8191 to MIDI's 0..16383
        put(0xE0 | c.channel, v & 0x7F, (v >> 7) & 0x7F);
        break;
      }
      case SND_SEQ_EVENT_CLOCK:
        put(0xF8, 0, 0);
        break;
      case SND_SEQ_EVENT_START:
        put(0xFA, 0, 0);
        break;
      case SND_SEQ_EVENT_CONTINUE:
        put(0xFB, 0, 0);
        break;
      case SND_SEQ_EVENT_STOP:
        put(0xFC, 0, 0);
        break;
      default:
        break;
      }
    }
  }).detach();
  return true;
}

#endif
