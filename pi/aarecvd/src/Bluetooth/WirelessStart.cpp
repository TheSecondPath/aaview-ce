/*
 *  aarecvd - wireless Android Auto receiver for the Veloster head unit (veloster-aa).
 *  Copyright (C) 2026 veloster-aa contributors
 *
 *  This program is free software: you can redistribute it and/or modify it under the terms of the GNU General
 *  Public License as published by the Free Software Foundation, either version 3 of the License, or (at your
 *  option) any later version. It is distributed WITHOUT ANY WARRANTY; see <http://www.gnu.org/licenses/>.
 */
#include "WirelessStart.hpp"
#include "../Log.hpp"

#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <fstream>
#include <map>
#include <stdexcept>
#include <thread>

namespace aarecvd::bt {

namespace {

std::string trim(const std::string &s) {
  auto b = s.find_first_not_of(" \t\r\n"), e = s.find_last_not_of(" \t\r\n");
  return b == std::string::npos ? std::string() : s.substr(b, e - b + 1);
}

void varint(std::vector<uint8_t> &out, uint64_t n) {
  do {
    uint8_t b = n & 0x7f;
    n >>= 7;
    out.push_back(static_cast<uint8_t>(b | (n ? 0x80 : 0)));
  } while (n);
}

void sendAll(int fd, const std::vector<uint8_t> &data) {
  size_t off = 0;
  while (off < data.size()) {
    ssize_t n = ::send(fd, data.data() + off, data.size() - off, MSG_NOSIGNAL);
    if (n < 0) {
      if (errno == EINTR) continue;
      throw std::runtime_error(std::strerror(errno));
    }
    off += static_cast<size_t>(n);
  }
}

void recvAll(int fd, uint8_t *buf, size_t n) {
  size_t off = 0;
  while (off < n) {
    ssize_t r = ::recv(fd, buf + off, n - off, 0);
    if (r < 0) {
      if (errno == EINTR) continue;
      throw std::runtime_error(std::strerror(errno));
    }
    if (r == 0) throw std::runtime_error("connection closed");
    off += static_cast<size_t>(r);
  }
}

void sendMessage(int fd, uint16_t id, const std::vector<uint8_t> &body) {
  std::vector<uint8_t> f{static_cast<uint8_t>(body.size() >> 8), static_cast<uint8_t>(body.size()),
                         static_cast<uint8_t>(id >> 8), static_cast<uint8_t>(id)};
  f.insert(f.end(), body.begin(), body.end());
  sendAll(fd, f);
}

// A message's fields for the log, in the form {1: [0], 2: [b'..']}.
std::string describe(const std::vector<uint8_t> &b) {
  std::map<uint64_t, std::vector<std::string>> out;
  size_t i = 0;
  auto rv = [&](uint64_t &v) {
    v = 0;
    for (int shift = 0; i < b.size() && shift < 64; shift += 7) {
      uint8_t c = b[i++];
      v |= static_cast<uint64_t>(c & 0x7f) << shift;
      if (!(c & 0x80)) return true;
    }
    return false;
  };
  while (i < b.size()) {
    uint64_t key = 0, v = 0;
    if (!rv(key)) break;
    uint64_t wt = key & 7;
    if (wt == 0) {
      if (!rv(v)) break;
      out[key >> 3].push_back(std::to_string(v));
    } else if (wt == 2) {
      if (!rv(v) || v > b.size() - i) break;
      std::string s = "b'";
      for (size_t k = 0; k < v; ++k) {
        uint8_t c = b[i + k];
        s += (c >= 0x20 && c < 0x7f && c != '\'' && c != '\\') ? std::string(1, static_cast<char>(c))
                                                                 : log::fmt("\\x%02x", c);
      }
      out[key >> 3].push_back(s + "'");
      i += v;
    } else if (wt == 5 && i + 4 <= b.size()) {
      uint32_t x;
      std::memcpy(&x, &b[i], 4);
      out[key >> 3].push_back(std::to_string(x));
      i += 4;
    } else if (wt == 1 && i + 8 <= b.size()) {
      uint64_t x;
      std::memcpy(&x, &b[i], 8);
      out[key >> 3].push_back(std::to_string(x));
      i += 8;
    } else {
      break;
    }
  }
  std::string s = "{";
  for (auto &kv : out) {
    if (s.size() > 1) s += ", ";
    s += std::to_string(kv.first) + ": [";
    for (size_t k = 0; k < kv.second.size(); ++k) s += (k ? ", " : "") + kv.second[k];
    s += "]";
  }
  return s + "}";
}

}  // namespace

std::optional<WifiInfo> readWifiInfo(const std::string &apConf) {
  std::ifstream f(apConf);
  std::map<std::string, std::string> conf;
  std::string ln;
  while (f && std::getline(f, ln)) {
    auto first = ln.find_first_not_of(" \t");
    if (first == std::string::npos || ln[first] == '#') continue;
    auto eq = ln.find('=');
    if (eq == std::string::npos) continue;
    conf[trim(ln.substr(0, eq))] = trim(ln.substr(eq + 1));
  }
  if (conf["ssid"].empty() || conf["key"].empty() || conf["ip"].empty()) return std::nullopt;
  return WifiInfo{conf["ssid"], conf["key"], conf["bssid"], conf["ip"]};
}

std::vector<uint8_t> pbVarint(uint32_t field, uint64_t value) {
  std::vector<uint8_t> out;
  varint(out, static_cast<uint64_t>(field) << 3);
  varint(out, value);
  return out;
}

std::vector<uint8_t> pbString(uint32_t field, const std::string &value) {
  std::vector<uint8_t> out;
  varint(out, (static_cast<uint64_t>(field) << 3) | 2);
  varint(out, value.size());
  out.insert(out.end(), value.begin(), value.end());
  return out;
}

std::vector<uint8_t> wifiStartRequest(const std::string &ip, uint16_t port) {
  auto out = pbString(1, ip);                   // ip_address
  auto p = pbVarint(2, port);                   // port
  out.insert(out.end(), p.begin(), p.end());
  return out;
}

std::vector<uint8_t> wifiInfoResponse(const WifiInfo &w) {
  std::vector<uint8_t> out;
  for (auto part : {pbString(1, w.ssid), pbString(2, w.key), pbString(3, w.bssid),
                    pbVarint(4, 8),             // security mode: WPA2 personal (8 in the Bluetooth message's enum)
                    pbVarint(5, 1)})            // access point type: dynamic
    out.insert(out.end(), part.begin(), part.end());
  return out;
}

void WirelessStart::run(int fd) const {
  // BlueZ hands the RFCOMM socket over non-blocking, but this thread wants to wait on it. Left non-blocking, the
  // first read fails with EAGAIN and the phone gets nothing.
  int flags = ::fcntl(fd, F_GETFL);
  if (flags >= 0 && (flags & O_NONBLOCK)) ::fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
  try {
    // aa-ap writes /etc/aarecv.conf a few seconds after it starts, so wait a little for it.
    std::optional<WifiInfo> w;
    auto end = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (!(w = readWifiInfo(apConf))) {
      if (std::chrono::steady_clock::now() > end)
        throw std::runtime_error("no ssid/key/ip in " + apConf + " (is aa-ap up?)");
      std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    AA_INFO("bt: -> WifiStartRequest ip %s port %u", w->ip.c_str(), static_cast<unsigned>(port));
    sendMessage(fd, 1, wifiStartRequest(w->ip, port));
    for (;;) {
      uint8_t hdr[4];
      recvAll(fd, hdr, 4);
      size_t n = (hdr[0] << 8) | hdr[1];
      uint16_t mid = static_cast<uint16_t>((hdr[2] << 8) | hdr[3]);
      std::vector<uint8_t> body(n);
      if (n) recvAll(fd, body.data(), n);
      if (mid == 2) {
        AA_DBG("bt: <- WifiInfoRequest; -> WifiInfoResponse ssid \"%s\" bssid %s", w->ssid.c_str(), w->bssid.c_str());
        sendMessage(fd, 3, wifiInfoResponse(*w));
      } else {
        const char *name = mid == 6 ? "WifiConnectStatus" : mid == 7 ? "WifiStartResponse"
                         : mid == 4 ? "WifiVersionRequest" : "message";
        AA_DBG("bt: <- %s (%u) %s", name, static_cast<unsigned>(mid), body.empty() ? "" : describe(body).c_str());
      }
    }
  } catch (const std::exception &ex) {
    AA_WARN("bt: bootstrap ended: %s", ex.what());
  }
  ::close(fd);
}

}  // namespace aarecvd::bt
