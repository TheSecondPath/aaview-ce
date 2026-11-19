/*
 *  aarecvd - wireless Android Auto receiver for the Veloster head unit (veloster-aa).
 *  Copyright (C) 2026 veloster-aa contributors
 *
 *  This program is free software: you can redistribute it and/or modify it under the terms of the GNU General
 *  Public License as published by the Free Software Foundation, either version 3 of the License, or (at your
 *  option) any later version. It is distributed WITHOUT ANY WARRANTY; see <http://www.gnu.org/licenses/>.
 */
// The Bluetooth start-up of wireless Android Auto, run by `aarecvd --bt-only`. Like WirelessAndroidAutoDongle's
// aawgd it talks to BlueZ over D-Bus, here with sd-bus from libsystemd. OpenAuto's btservice does the same job with
// Qt Bluetooth but only waits for the phone; this also connects to the phone and keeps trying. It:
//
//   - registers the AA wireless RFCOMM profile (server, channel 22), an HSP headset profile, and a pairing agent
//     that accepts pairing only while pairing is open: 3 minutes, from --pair or a 3 s hold on aaview's icon. The
//     state and the phone's code reach aaview through the file in PairingState.hpp.
//   - every 10 s with no session, connects to paired phones (the headset profile is what makes Android Auto
//     start), and nudges a phone that stays connected without starting a session by disconnecting it fully and
//     connecting again.
//   - closes the headset link again right away, because calls belong to the car's own hands-free.
//   - tells a hung Bluetooth chip from a phone that is out of range, and resets the chip only in the first case.
//   - on the AA wireless link, tells the phone which Wi-Fi to join and to connect to <ip>:5288 (WirelessStart.hpp),
//     and touches /run/veloaa/joining so that aa-ap stops NetworkManager trying for home Wi-Fi meanwhile. Its scans
//     and association attempts take the shared radio away from the access point the phone is joining.
// Everything runs on one sd-event loop, except the start-up exchange, which gets a thread per phone link.
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <systemd/sd-bus.h>
#include <systemd/sd-event.h>

#include <functional>

namespace aarecvd::bt {

struct Options {
  bool pair = false;                              // --pair: discoverable and pairable for 3 minutes
  std::string pairingFile = "/run/veloaa/pairing";   // pairing's state for aarecvd (PairingState.hpp)
  std::string joiningFile = "/run/veloaa/joining";   // touched when a phone opens the AA wireless link: aa-ap stops
                                                     // trying for home Wi-Fi while it joins the access point
  std::string apConf = "/etc/aarecv.conf";        // the access point's ssid=, key=, bssid=, ip= (aa-ap writes it)
  uint16_t port = 5288;                           // where the phone is told to connect
  std::string adapter = "/org/bluez/hci0";
  std::string serialDevice = "serial0-0";         // the chip's UART device, rebound to reset a hung chip
};

class Bluetooth {
public:
  // Registers everything with BlueZ. Throws std::runtime_error when that fails, and systemd restarts the process.
  // sessionActive belongs to the --bt-only loop: whether aarecvd says a session is up.
  Bluetooth(sd_event *event, const Options &options, const bool &sessionActive);
  ~Bluetooth();
  Bluetooth(const Bluetooth &) = delete;
  Bluetooth &operator=(const Bluetooth &) = delete;

  // aaview's icon was tapped with no phone session: try the phone right away.
  void connectNow();

  // Opens pairing (discoverable and pairable) for PAIR_SECONDS, or closes it now. Opening it again restarts the time.
  static constexpr int PAIR_SECONDS = 180;
  void startPairing(const char *why);
  void stopPairing(const char *why);
  bool pairing() const { return pairing_; }

  // Public for the sd-bus vtables' C callbacks.
  struct Profile {
    Bluetooth *bt;
    std::string kind, path;
    struct Held {
      int fd;
      uint64_t gen;
    };
    std::map<std::string, Held> fds;              // device path -> headset socket held for a while
    uint64_t gen = 0;
    sd_bus_slot *slot = nullptr;
    void drop(const std::string &device);
    void shut(int fd, const std::string &device);
  };
  static int profileRelease(sd_bus_message *m, void *u, sd_bus_error *e);
  static int profileNewConnection(sd_bus_message *m, void *u, sd_bus_error *e);
  static int profileRequestDisconnection(sd_bus_message *m, void *u, sd_bus_error *e);
  static int agentRelease(sd_bus_message *m, void *u, sd_bus_error *e);
  static int agentRequestPinCode(sd_bus_message *m, void *u, sd_bus_error *e);
  static int agentRequestPasskey(sd_bus_message *m, void *u, sd_bus_error *e);
  static int agentDisplayPasskey(sd_bus_message *m, void *u, sd_bus_error *e);
  static int agentDisplayPinCode(sd_bus_message *m, void *u, sd_bus_error *e);
  static int agentRequestConfirmation(sd_bus_message *m, void *u, sd_bus_error *e);
  static int agentRequestAuthorization(sd_bus_message *m, void *u, sd_bus_error *e);
  static int agentAuthorizeService(sd_bus_message *m, void *u, sd_bus_error *e);
  static int agentCancel(sd_bus_message *m, void *u, sd_bus_error *e);

private:
  struct Phone {
    std::string path, alias, address;
    bool connected = false;
  };
  struct Device {                                 // what devices() reads of each org.bluez.Device1
    std::string path, alias, address, icon;
    bool paired = false, connected = false;
  };
  bool devices(std::vector<Device> &out, std::string &error);

  // one-shot timer on the loop
  void after(uint64_t ms, std::function<void()> fn);
  // an asynchronous method call on a BlueZ object; done gets the error name ("" = it worked)
  void callAsync(const std::string &path, const char *iface, const char *member, const char *stringArg,
                 uint64_t timeoutMs, std::function<void(const std::string &)> done);
  bool setAdapterProperty(const char *name, char type, uint64_t value);
  std::string adapterString(const char *name);
  bool paired(const std::string &device);
  void trust(const std::string &device);
  bool phones(std::vector<Phone> &out, std::string &error);
  int refuse(const char *what, const std::string &device, sd_bus_error *e);

  void tick();                                    // autoconnect() every 10 s
  void autoconnect();
  void connect(const std::string &path);
  void resetLink(const std::string &path, bool thenConnect);
  void connectFailed(const std::string &path, const std::string &name);

  sd_event *event_;
  sd_bus *bus_ = nullptr;
  Options options_;
  const bool &sessionActive_;
  bool pairing_ = false;
  uint64_t pairGen_ = 0;                          // which opening of pairing a timer belongs to
  double pairUntil_ = 0;                          // monotonicSeconds() when it closes by itself
  void publishPairing(uint32_t passkey);
  void pairedWith(const std::string &device);     // a phone paired: pairing closes 20 s later
  std::optional<double> idleSince_;               // since when there has been no session
  double kicked_ = 0;                             // when a phone last started a session / was last nudged
  int nudges_ = 0;
  std::vector<double> noreplies_;                 // NoReply answers in the last 5 minutes
  int inprogress_ = 0;
  std::optional<double> awaySince_;               // when connects first went unanswered with the chip answering
  std::map<std::string, double> pending_;         // device path -> time until which a call of ours is in flight
  Profile aaw_, hsp_;
  sd_bus_slot *agentSlot_ = nullptr;
};

// Whether the Bluetooth chip answers a command (HCI Read_BD_ADDR on a raw HCI socket).
bool chipAnswers(double waitSeconds = 3.0);

}  // namespace aarecvd::bt
