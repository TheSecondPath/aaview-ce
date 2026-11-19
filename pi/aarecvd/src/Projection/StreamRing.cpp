/*
 *  aarecvd - wireless Android Auto receiver for the Veloster head unit (veloster-aa).
 *  Copyright (C) 2026 veloster-aa contributors
 *
 *  This program is free software: you can redistribute it and/or modify it under the terms of the GNU General
 *  Public License as published by the Free Software Foundation, either version 3 of the License, or (at your
 *  option) any later version. It is distributed WITHOUT ANY WARRANTY; see <http://www.gnu.org/licenses/>.
 */
// See StreamRing.hpp for the layout of the stream image.
#include "StreamRing.hpp"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/timex.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <ctime>
#include <stdexcept>
#include <thread>

namespace aarecvd::projection {

namespace {

using Clock = std::chrono::steady_clock;

void putU32(uint8_t *p, uint32_t v) {
  p[0] = v & 0xff;
  p[1] = (v >> 8) & 0xff;
  p[2] = (v >> 16) & 0xff;
  p[3] = (v >> 24) & 0xff;
}
void putU16(uint8_t *p, uint16_t v) {
  p[0] = v & 0xff;
  p[1] = (v >> 8) & 0xff;
}
uint32_t getU32(const uint8_t *p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}
uint16_t getU16(const uint8_t *p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }

void preadAll(int fd, uint8_t *buf, size_t n, off_t off) {
  size_t done = 0;
  while (done < n) {
    ssize_t r = ::pread(fd, buf + done, n - done, off + static_cast<off_t>(done));
    if (r <= 0) {
      std::memset(buf + done, 0, n - done);     // a short image reads as zeros, as a fresh one would
      return;
    }
    done += static_cast<size_t>(r);
  }
}

void pwriteAll(int fd, const uint8_t *buf, size_t n, off_t off) {
  size_t done = 0;
  while (done < n) {
    ssize_t r = ::pwrite(fd, buf + done, n - done, off + static_cast<off_t>(done));
    if (r <= 0) throw std::runtime_error(std::string("stream image write: ") + std::strerror(errno));
    done += static_cast<size_t>(r);
  }
}

void createImage(const std::string &path) {
  struct stat st{};
  if (::stat(path.c_str(), &st) != 0 || st.st_size != static_cast<off_t>(StreamRing::SECTORS) * 512) {
    int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
    if (fd < 0) throw std::runtime_error("cannot create " + path + ": " + std::strerror(errno));
    if (::ftruncate(fd, static_cast<off_t>(StreamRing::SECTORS) * 512) != 0) {
      ::close(fd);
      throw std::runtime_error("cannot size " + path + ": " + std::strerror(errno));
    }
    ::close(fd);
  }
}

// Holds flock(2) on the stream image while the header's flags word is read, changed and written back. aarecvd's header
// writes and 'aarecvd --quit-aaview' (another process) both take it, so neither can write back a flags word the other
// has just changed. Without it, a quit bit set between aarecvd's read of byte 36 and its header write is lost.
class ImageLock {
public:
  explicit ImageLock(int fd) : fd_(fd) {
    while (::flock(fd_, LOCK_EX) != 0 && errno == EINTR) {
    }
  }
  ~ImageLock() { ::flock(fd_, LOCK_UN); }
  ImageLock(const ImageLock &) = delete;
  ImageLock &operator=(const ImageLock &) = delete;

private:
  int fd_;
};

}  // namespace

std::string StreamRing::actionName(int action) {
  static const char *names[] = {"down", "up",   "move",   "keydown", "keyup",
                                "hardkey", "isvevent", "icon", "lights", "pair"};
  if (action >= 0 && action <= 9) return names[action];
  return std::to_string(action);
}

bool StreamRing::clockSynced() {
  struct timex t{};
  t.modes = 0;
  if (::adjtimex(&t) < 0) return false;
  return t.maxerror >= 0 && t.maxerror < static_cast<long>(CLOCK_MAXERROR_US);
}

StreamRing::StreamRing(const std::string &path, uint32_t width, uint32_t height) : width_(width), height_(height) {
  createImage(path);
  fd_ = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
  if (fd_ < 0) throw std::runtime_error("cannot open " + path + ": " + std::strerror(errno));
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::system_clock::now().time_since_epoch()).count();
  session_ = static_cast<uint32_t>(static_cast<uint64_t>(ms) & 0xffffffffu);
  micGen_ = static_cast<uint32_t>(std::time(nullptr)) & 0x7fffff00u;   // never a number an earlier run used
  t0_ = Clock::now();
  writeHeader();
}

StreamRing::~StreamRing() {
  if (fd_ >= 0) ::close(fd_);
}

// -- header / status --
void StreamRing::writeHeader() {
  std::lock_guard<std::recursive_mutex> l(lock_);
  writeHeaderLocked();
}

void StreamRing::writeHeaderLocked() {
  ImageLock imageLock(fd_);            // --quit-aaview changes byte 36 from another process
  if (seq_) {                          // the quit bit belongs to --quit-aaview / --unquit-aaview: mirror what's on disk
    uint8_t b[4];
    preadAll(fd_, b, 4, 36);
    flags_ = (flags_ & ~FLAG_QUIT) | (getU32(b) & FLAG_QUIT);
  }
  seq_ = seq_ + 1;
  const auto now = Clock::now();
  if (!haveSyncedAt_ || now - syncedAt_ > std::chrono::seconds(10)) {   // ask the kernel at most every 10 s
    synced_ = clockSynced();
    syncedAt_ = now;
    haveSyncedAt_ = true;
  }
  uint32_t utc = 0;
  int32_t gmtoff = 0;
  if (setClock_ && synced_) {
    std::time_t t = std::time(nullptr);
    struct tm tmv{};
    localtime_r(&t, &tmv);
    utc = static_cast<uint32_t>(static_cast<uint64_t>(t) & 0xffffffffu);
    gmtoff = static_cast<int32_t>(tmv.tm_gmtoff);
  }
  uint8_t h[512] = {};
  std::memcpy(h, "AVS1", 4);
  const uint32_t v[10] = {seq_, session_, wr_, RING_SECTOR, RING_BYTES, idrPos_, width_, height_, flags_, micGen_};
  for (int i = 0; i < 10; ++i) putU32(h + 4 + 4 * i, v[i]);
  putU32(h + 44, utc);
  putU32(h + 48, static_cast<uint32_t>(gmtoff));
  putU32(h + 52, (flags_ & FLAG_PAIRING) ? pairKey_ : 0);
  putU32(h + 504, seq_);
  pwriteAll(fd_, h, sizeof h, 0);
}

std::vector<StreamRing::Event> StreamRing::poll(bool events) {
  std::lock_guard<std::recursive_mutex> l(lock_);
  return pollLocked(events);
}

std::vector<StreamRing::Event> StreamRing::pollLocked(bool events) {
  uint8_t b[512];
  preadAll(fd_, b, 512, 512);
  if (std::memcmp(b, "AVH1", 4) != 0) return {};
  HuStatus s{};
  uint32_t *f[] = {&s.seq, &s.session, &s.rd, &s.fed, &s.decoded, &s.dropped, &s.flags, &s.tick, &s.nev};
  for (int i = 0; i < 9; ++i) *f[i] = getU32(b + 4 + 4 * i);
  if (s.seq != getU32(b + 504)) return {};
  auto prev = hu_;
  hu_ = s;
  if (prev && prev->seq != s.seq) {    // only a changing sequence means aaview is alive (not a stale sector)
    huSeen_ = Clock::now();
    huSeenSet_ = true;
  }
  if (prev && prev->session != s.session) nev_.reset();   // a new aaview: its old events are not replayed
  if (!events) return {};              // status only (read position); events stay for the next poll()
  std::vector<Event> out;
  if (!prev || prev->session != s.session || !nev_) nev_ = s.nev;
  // The sector holds aaview's last 16 events. If more came since the last poll, the oldest are gone and counted as
  // lost, but the newest 16 are still delivered, so a finger lifted in that burst does not stay down.
  const uint32_t behind = s.nev - *nev_;
  if (behind > 16) {
    eventsLost_ += behind - 16;
    nev_ = s.nev - 16;
  }
  while (*nev_ != s.nev) {
    const uint8_t *e = b + 40 + (*nev_ % 16) * 8;
    out.push_back(Event{e[4], getU16(e), getU16(e + 2)});
    nev_ = *nev_ + 1;
  }
  return out;
}

bool StreamRing::huAlive() {
  std::lock_guard<std::recursive_mutex> l(lock_);
  return huAliveLocked();
}

bool StreamRing::huAliveLocked() {
  return hu_ && huSeenSet_ && Clock::now() - huSeen_ < std::chrono::seconds(5) && !(hu_->flags & 0x80000000u);
}

std::optional<StreamRing::HuStatus> StreamRing::hu() {
  std::lock_guard<std::recursive_mutex> l(lock_);
  return hu_;
}

uint32_t StreamRing::free() {
  std::lock_guard<std::recursive_mutex> l(lock_);
  return freeLocked();
}

uint32_t StreamRing::freeLocked() {
  const uint32_t behind = hu_ ? wr_ - hu_->rd : 0;
  if (!huAliveLocked() || behind > RING_BYTES) return RING_BYTES;   // nobody reading (or an older session): free-run
  return RING_BYTES - behind - 64;
}

void StreamRing::setProjecting(bool on) {
  std::lock_guard<std::recursive_mutex> l(lock_);
  flags_ = on ? (flags_ | FLAG_PROJECTING) : (flags_ & ~FLAG_PROJECTING);
  writeHeaderLocked();
}

void StreamRing::mediaStarted() {
  // Flags bits 8..15 count media starts. Each new value makes aaview put the head unit's source back on USB media.
  std::lock_guard<std::recursive_mutex> l(lock_);
  flags_ = (flags_ & ~0xff00u) | ((flags_ + 0x100u) & 0xff00u);
  writeHeaderLocked();
}

void StreamRing::setLoglevel(int level) {
  std::lock_guard<std::recursive_mutex> l(lock_);
  flags_ = (flags_ & ~0x30000u) | ((static_cast<uint32_t>(level) & 3u) << 16);
}

void StreamRing::setPhone(bool on) {
  std::lock_guard<std::recursive_mutex> l(lock_);
  flags_ = on ? (flags_ | FLAG_PHONE) : (flags_ & ~FLAG_PHONE);
  writeHeaderLocked();
}

void StreamRing::setPairing(bool open, uint32_t passkey) {
  std::lock_guard<std::recursive_mutex> l(lock_);
  const uint32_t flags = open ? (flags_ | FLAG_PAIRING) : (flags_ & ~FLAG_PAIRING);
  const uint32_t key = open ? passkey : 0;
  if (flags == flags_ && key == pairKey_) return;
  flags_ = flags;
  pairKey_ = key;
  writeHeaderLocked();
}

void StreamRing::setClockEnabled(bool on) {
  std::lock_guard<std::recursive_mutex> l(lock_);
  setClock_ = on;
}

// -- microphone --
void StreamRing::setMic(bool on) {
  // While on, aaview records the car's microphone into the mic slots. Each time gets a new mic_gen, and the slots are
  // wiped first, so nothing recorded earlier is ever taken for new sound.
  std::lock_guard<std::recursive_mutex> l(lock_);
  if (on) {
    std::vector<uint8_t> zero(MIC_SLOTS * MIC_SLOT_BYTES, 0);
    pwriteAll(fd_, zero.data(), zero.size(), MIC_SECTOR * 512);
    micGen_ += 1;
    micSeq_ = micLost_ = 0;
    flags_ |= FLAG_MIC;
  } else if (flags_ & FLAG_MIC) {
    flags_ &= ~FLAG_MIC;
  } else {
    return;
  }
  writeHeaderLocked();
}

bool StreamRing::huCanRecord() {
  std::lock_guard<std::recursive_mutex> l(lock_);
  return huAliveLocked() && (hu_->flags & HU_CAN_RECORD);
}

std::vector<std::vector<uint8_t>> StreamRing::pollMic() {
  std::lock_guard<std::recursive_mutex> l(lock_);
  if (!(flags_ & FLAG_MIC)) return {};
  std::vector<uint8_t> b(MIC_SLOTS * MIC_SLOT_BYTES);
  preadAll(fd_, b.data(), b.size(), MIC_SECTOR * 512);
  std::vector<std::pair<uint32_t, std::vector<uint8_t>>> fresh;
  for (uint32_t i = 0; i < b.size(); i += MIC_SLOT_BYTES) {
    const uint8_t *s = b.data() + i;
    if (std::memcmp(s, "AVM1", 4) != 0) continue;
    uint32_t gen = getU32(s + 4), seq = getU32(s + 8), n = getU32(s + 12);
    if (gen != micGen_ || n > MIC_SLOT_BYTES - 20 || n % 2) continue;
    if (seq != getU32(s + MIC_SLOT_BYTES - 4)) continue;   // caught half-written: next time
    if (seq > micSeq_) fresh.emplace_back(seq, std::vector<uint8_t>(s + 16, s + 16 + n));
  }
  std::sort(fresh.begin(), fresh.end());
  std::vector<std::vector<uint8_t>> out;
  for (auto &p : fresh) {
    micLost_ += p.first - micSeq_ - 1;   // slots overwritten before we looked, though we poll every 5 ms
    micSeq_ = p.first;
    out.push_back(std::move(p.second));
  }
  return out;
}

// -- records --
bool StreamRing::put(const uint8_t *payload, size_t size, bool idr, uint16_t rtype, int rflags) {
  // The ring starts at an IDR. After that, a full ring waits up to 0.3 s for aaview, then drops just that frame: a
  // phone may send only one IDR per session, so waiting for the next one could mean no picture for good.
  std::lock_guard<std::mutex> pl(putLock_);
  if (size > MAX_RECORD_BYTES) {       // more than aaview reads of the ring at once (see MAX_RECORD_BYTES)
    std::lock_guard<std::recursive_mutex> l(lock_);
    dropped_ += 1;
    oversize_ += 1;
    return false;
  }
  const uint32_t total = static_cast<uint32_t>((16 + size + 15) & ~static_cast<size_t>(15));
  if (rtype == REC_AU) {
    if (needIdr_ && !idr) {
      std::lock_guard<std::recursive_mutex> l(lock_);
      dropped_ += 1;
      return false;
    }
    const auto deadline = Clock::now() + std::chrono::milliseconds(300);
    while (total > free() && Clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
      poll(false);
    }
    if (total > free()) {
      std::lock_guard<std::recursive_mutex> l(lock_);
      dropped_ += 1;
      return false;
    }
    needIdr_ = false;
  } else if (total > free()) {
    return false;
  }
  uint32_t off, wrNow;
  {
    std::lock_guard<std::recursive_mutex> l(lock_);
    recNo_ = recNo_ + 1;
    const uint32_t pts = static_cast<uint32_t>(
        static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0_).count()) &
        0xffffffffu);
    const uint16_t fl = rflags >= 0 ? static_cast<uint16_t>(rflags) : static_cast<uint16_t>(idr ? 1 : 0);
    rec_.assign(total, 0);
    putU32(rec_.data(), static_cast<uint32_t>(size));
    putU16(rec_.data() + 4, rtype);
    putU16(rec_.data() + 6, fl);
    putU32(rec_.data() + 8, pts);
    putU32(rec_.data() + 12, recNo_);
    wrNow = wr_;
  }
  if (size) std::memcpy(rec_.data() + 16, payload, size);
  off = wrNow % RING_BYTES;
  const uint32_t first = std::min<uint32_t>(total, RING_BYTES - off);
  pwriteAll(fd_, rec_.data(), first, static_cast<off_t>(RING_SECTOR) * 512 + off);
  if (first < total) pwriteAll(fd_, rec_.data() + first, total - first, static_cast<off_t>(RING_SECTOR) * 512);
  std::lock_guard<std::recursive_mutex> l(lock_);
  if (idr) idrPos_ = wr_;
  wr_ = wr_ + total;
  writeHeaderLocked();
  return true;
}

void StreamRing::setQuitFlag(const std::string &path, bool on) {
  createImage(path);
  int fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
  if (fd < 0) throw std::runtime_error("cannot open " + path + ": " + std::strerror(errno));
  try {
    ImageLock imageLock(fd);
    uint8_t b[4];
    preadAll(fd, b, 4, 36);
    const uint32_t flags = getU32(b);
    putU32(b, on ? (flags | FLAG_QUIT) : (flags & ~FLAG_QUIT));
    pwriteAll(fd, b, 4, 36);
  } catch (...) {
    ::close(fd);
    throw;
  }
  ::close(fd);
}

uint32_t StreamRing::flags() {
  std::lock_guard<std::recursive_mutex> l(lock_);
  return flags_;
}
uint32_t StreamRing::micLost() {
  std::lock_guard<std::recursive_mutex> l(lock_);
  return micLost_;
}
uint32_t StreamRing::dropped() {
  std::lock_guard<std::recursive_mutex> l(lock_);
  return dropped_;
}
uint32_t StreamRing::eventsLost() {
  std::lock_guard<std::recursive_mutex> l(lock_);
  return eventsLost_;
}
uint32_t StreamRing::oversize() {
  std::lock_guard<std::recursive_mutex> l(lock_);
  return oversize_;
}

std::vector<std::pair<int, size_t>> nalTypes(const uint8_t *data, size_t size) {
  std::vector<std::pair<int, size_t>> out;
  size_t i = 0;
  while (size >= 3 && i + 3 <= size) {
    const uint8_t *p = static_cast<const uint8_t *>(std::memchr(data + i, 0, size - i));
    if (!p) break;
    i = static_cast<size_t>(p - data);
    if (i + 3 > size) break;
    if (data[i + 1] == 0 && data[i + 2] == 1) {
      if (i + 3 >= size) break;
      out.emplace_back(data[i + 3] & 0x1f, i);
      i += 3;
    } else {
      i += 1;
    }
  }
  return out;
}

}  // namespace aarecvd::projection
