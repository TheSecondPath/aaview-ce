/*
 *  aarecvd - wireless Android Auto receiver for the Veloster head unit (veloster-aa).
 *  Copyright (C) 2026 veloster-aa contributors
 *
 *  This program is free software: you can redistribute it and/or modify it under the terms of the GNU General
 *  Public License as published by the Free Software Foundation, either version 3 of the License, or (at your
 *  option) any later version. It is distributed WITHOUT ANY WARRANTY; see <http://www.gnu.org/licenses/>.
 */
// The wireless start-up exchange on the phone's AA wireless RFCOMM link. We say where to connect (WifiStartRequest:
// the access point's ip, port 5288), then the phone asks for the Wi-Fi details (WifiInfoRequest) and gets them
// (WifiInfoResponse). Frames are [length u16 BE][message id u16 BE][protobuf].
//
// The protobufs are written by hand. aasdk's aap_protobuf has these messages, and OpenAuto's btservice uses them,
// but its WifiSecurityMode enum is the one of the Wi-Fi projection channel, where WPA2_PERSONAL is 5. The Bluetooth
// message wants 8, as in WirelessAndroidAutoDongle's and aa-proxy-rs's WifiInfoResponse.proto, and the phone
// accepts 8.
//
// The Wi-Fi details come only from /etc/aarecv.conf (ssid=, key=, bssid=, ip=, written by aa-ap), so the home
// network the Pi is on is never handed out.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace aarecvd::bt {

struct WifiInfo {
  std::string ssid, key, bssid, ip;
};

// Needs ssid, key and ip (bssid may be empty); otherwise returns nothing.
std::optional<WifiInfo> readWifiInfo(const std::string &apConf);

// Hand-written protobuf: one field, or a whole message we send.
std::vector<uint8_t> pbString(uint32_t field, const std::string &value);
std::vector<uint8_t> pbVarint(uint32_t field, uint64_t value);
std::vector<uint8_t> wifiStartRequest(const std::string &ip, uint16_t port);
std::vector<uint8_t> wifiInfoResponse(const WifiInfo &w);

struct WirelessStart {
  std::string apConf;
  uint16_t port;
  // Runs the exchange on this RFCOMM socket until the phone closes it, then closes it too. There is no timeout:
  // the channel stays open as long as the phone keeps it, which the phone may expect. It blocks, so each link gets
  // its own thread.
  void run(int fd) const;
};

}  // namespace aarecvd::bt
