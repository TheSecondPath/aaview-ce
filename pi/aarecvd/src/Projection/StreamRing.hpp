/*
 *  aarecvd - wireless Android Auto receiver for the Veloster head unit (veloster-aa).
 *  Copyright (C) 2026 veloster-aa contributors
 *
 *  This program is free software: you can redistribute it and/or modify it under the terms of the GNU General
 *  Public License as published by the Free Software Foundation, either version 3 of the License, or (at your
 *  option) any later version. It is distributed WITHOUT ANY WARRANTY; see <http://www.gnu.org/licenses/>.
 */
// The Pi's half of the pipe to aaview on the head unit. /run/veloaa/stream.img (4 MB, 8192 sectors, in RAM) is LUN 2
// of the stick gadget. The layout is little-endian and matches the comment at the top of apps/aaview/aaview.cpp:
//
//   sector 0   Pi header  "AVS1", u32 seq, u32 session, u32 wr, u32 ring_sector, u32 ring_bytes, u32 idr_pos,
//                         u32 width, u32 height, u32 flags (bit 0 quit, 1 projecting, 2 phone, 3 microphone wanted,
//                         bit 4 pairing open, bits 8..15 media starts, bits 16..17 log level), u32 mic_gen,
//                         u32 utc (byte 44), i32 local offset (byte 48), u32 pairing code (byte 52, 0 = none),
//                         seq copy at 504
//   sector 1   aaview     "AVH1", u32 seq, u32 session, u32 rd, u32 fed, u32 decoded, u32 dropped, u32 flags,
//                         u32 tick, u32 nevents, 16 x (u16 x, u16 y, u8 action, u8 pointer, u16 ms), seq copy at 504
//   2 to 13    microphone, written by aaview while flag bit 3 is set: 3 slots of 4 sectors (slot = seq % 3), each
//                         "AVM1", u32 mic_gen, u32 seq (from 1 for each mic_gen), u32 nbytes, PCM, seq copy at 2044
//   16 to 4111 the ring (2 MiB), records, 16-byte aligned: u32 len, u16 type (1 AU, 2 audio, 9 bench), u16 flags (AU: bit 0 IDR;
//              audio: the stream, 0 media, 1 speech, 2 system), u32 pts_ms, u32 rec_no, payload
//   4112 on    unused
//
// The ring size has to be a power of two so pos % RING_BYTES stays right when the 32-bit counters wrap.
#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace aarecvd::projection {

class StreamRing {
public:
  static constexpr uint32_t SECTORS = 8192;
  static constexpr uint32_t RING_SECTOR = 16;
  static constexpr uint32_t RING_BYTES = 4096 * 512;
  static constexpr uint16_t REC_AU = 1, REC_AUDIO = 2;
  static constexpr uint32_t FLAG_QUIT = 1, FLAG_PROJECTING = 2, FLAG_PHONE = 4, FLAG_MIC = 8, FLAG_PAIRING = 16;
  static constexpr uint32_t MIC_SECTOR = 2, MIC_SLOTS = 3, MIC_SLOT_BYTES = 2048;
  static constexpr uint32_t HU_CAN_RECORD = 4;            // aaview's status flags: it has microphone support
  static constexpr uint32_t CLOCK_MAXERROR_US = 16000000;  // systemd's test for NTPSynchronized
  // The largest record payload: aaview reads the ring 512 KB at a time and keeps 2 KB of that for alignment. Bigger
  // records are dropped here, because aaview cannot read them in one piece.
  static constexpr uint32_t MAX_RECORD_BYTES = 512 * 1024 - 2048;

  // aaview's event actions
  enum Action { DOWN = 0, UP = 1, MOVE = 2, KEYDOWN = 3, KEYUP = 4, HARDKEY = 5, ISVEVENT = 6, ICON = 7, LIGHTS = 8,
                PAIR = 9 };   // PAIR: x 1 open pairing, 0 close it (icon held 3 s), 2 code confirmed (tap)
  static std::string actionName(int action);

  struct HuStatus {
    uint32_t seq, session, rd, fed, decoded, dropped, flags, tick, nev;
  };
  struct Event {
    int action;
    uint16_t x, y;
  };

  explicit StreamRing(const std::string &path = "/run/veloaa/stream.img", uint32_t width = 800, uint32_t height = 480);
  ~StreamRing();

  void writeHeader();
  // Reads aaview's status sector and returns the new events. With events=false only the status is read, and the
  // events wait for the next poll.
  std::vector<Event> poll(bool events = true);
  bool huAlive();
  std::optional<HuStatus> hu();
  uint32_t free();

  void setProjecting(bool on);
  void mediaStarted();
  void setLoglevel(int level);     // only kept: the next header write carries it
  void setPhone(bool on);
  void setMic(bool on);
  void setClockEnabled(bool on);   // follows veloaa.txt's set_clock
  void setPairing(bool open, uint32_t passkey);   // writes the header when it changes
  bool huCanRecord();
  std::vector<std::vector<uint8_t>> pollMic();
  // Appends one record; false if it was dropped.
  bool put(const uint8_t *payload, size_t size, bool idr = false, uint16_t rtype = REC_AU, int rflags = -1);

  // For 'aarecvd --quit-aaview' and '--unquit-aaview': sets or clears the quit bit in the header on disk, in place,
  // without starting a new session. A running aarecvd copies it into its next header write. aaview quits cleanly on
  // it and releases the decoder's memory; killing aaview leaks that memory until the head unit restarts. Clear it
  // again before starting aaview. Creates the image if it is not there.
  static void setQuitFlag(const std::string &path, bool on);

  uint32_t flags();
  uint32_t micLost();
  uint32_t dropped();
  uint32_t oversize();             // records dropped for being over MAX_RECORD_BYTES (also in dropped())
  uint32_t eventsLost();           // aaview events overwritten before a poll saw them

  // Whether an NTP client keeps the clock, judged by the kernel's maximum error being under 16 s.
  static bool clockSynced();

private:
  void writeHeaderLocked();
  std::vector<Event> pollLocked(bool events);
  bool huAliveLocked();
  uint32_t freeLocked();

  int fd_ = -1;
  uint32_t session_, seq_ = 0, wr_ = 0, idrPos_ = 0, recNo_ = 0, width_, height_, flags_ = 0, pairKey_ = 0;
  bool setClock_ = true, synced_ = false;
  std::chrono::steady_clock::time_point syncedAt_;
  bool haveSyncedAt_ = false;
  bool needIdr_ = true;            // the ring starts at an IDR
  std::optional<HuStatus> hu_;
  std::chrono::steady_clock::time_point huSeen_;
  bool huSeenSet_ = false;
  std::optional<uint32_t> nev_;    // aaview's cumulative event count we've handled
  uint32_t dropped_ = 0, oversize_ = 0, eventsLost_ = 0;
  uint32_t micGen_, micSeq_ = 0, micLost_ = 0;
  std::chrono::steady_clock::time_point t0_;
  std::recursive_mutex lock_;      // header writes and status polls come from several threads
  std::mutex putLock_;             // records are appended one at a time
  std::vector<uint8_t> rec_;
};

// (NAL type, offset of its 00 00 01) for every start code in an H.264 stream.
std::vector<std::pair<int, size_t>> nalTypes(const uint8_t *data, size_t size);

static_assert((StreamRing::RING_BYTES & (StreamRing::RING_BYTES - 1)) == 0, "the ring must be a power of two");
static_assert(StreamRing::RING_SECTOR + StreamRing::RING_BYTES / 512 <= StreamRing::SECTORS,
              "the ring must fit in the image");
static_assert(StreamRing::MAX_RECORD_BYTES < StreamRing::RING_BYTES / 2,
              "aaview skips records over half the ring");

}  // namespace aarecvd::projection
