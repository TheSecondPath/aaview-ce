/*
 *  aarecvd - wireless Android Auto receiver for the Veloster head unit (veloster-aa).
 *  Copyright (C) 2026 veloster-aa contributors
 *
 *  This program is free software: you can redistribute it and/or modify it under the terms of the GNU General
 *  Public License as published by the Free Software Foundation, either version 3 of the License, or (at your
 *  option) any later version. It is distributed WITHOUT ANY WARRANTY; see <http://www.gnu.org/licenses/>.
 */
// See Bluetooth.hpp for what this does and why.
#include "Bluetooth.hpp"
#include "PairingState.hpp"
#include "WirelessStart.hpp"
#include "../Log.hpp"

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <thread>

namespace aarecvd::bt {

namespace {

const char *AAW_UUID = "4de17a00-52cb-11e6-bdf4-0800200c9a66";
const char *HSP_HS_UUID = "00001108-0000-1000-8000-00805f9b34fb";
const char *HSP_AG_UUID = "00001112-0000-1000-8000-00805f9b34fb";
const uint16_t RFCOMM_CHANNEL = 22;   // 8 (what WirelessAndroidAutoDongle uses) is taken by BlueZ's SIM Access profile
const char *AAW_PATH = "/veloaa/aaw";
const char *HSP_PATH = "/veloaa/hsp";
const char *AGENT_PATH = "/veloaa/agent";

const double NUDGE_AFTER = 60;       // s a phone may sit connected over Bluetooth with no AA session before it's nudged
const double CONNECT_LIMIT = 20;     // s for one connect attempt (WirelessAndroidAutoDongle and aa-proxy-rs both use 20)
const uint64_t HSP_HOLD_MS = 10000;  // the headset link may stay open waiting for the phone to open the AA wireless link
const double RESET_ANYWAY = 900;     // s of unanswered connects after which the chip is reset even though it answers

// Creates the file, or sets its time to now, making its folder if needed. A failure is only logged.
void touchFile(const std::string &path) {
  if (path.empty()) return;
  auto slash = path.rfind('/');
  if (slash != std::string::npos && slash > 0) ::mkdir(path.substr(0, slash).c_str(), 0755);
  int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
  if (fd < 0 || ::futimens(fd, nullptr) < 0) AA_WARN("bt: touching %s: %s", path.c_str(), std::strerror(errno));
  if (fd >= 0) ::close(fd);
}

bool endsWith(const std::string &s, const char *tail) {
  size_t n = std::strlen(tail);
  return s.size() >= n && s.compare(s.size() - n, n, tail) == 0;
}

std::string errName(const sd_bus_error &e, int r) {
  if (e.name) return e.name;
  return log::fmt("errno %d (%s)", -r, std::strerror(-r));
}

const sd_bus_vtable PROFILE_VTABLE[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("Release", "", "", Bluetooth::profileRelease, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("NewConnection", "oha{sv}", "", Bluetooth::profileNewConnection, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("RequestDisconnection", "o", "", Bluetooth::profileRequestDisconnection,
                  SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_VTABLE_END};

const sd_bus_vtable AGENT_VTABLE[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("Release", "", "", Bluetooth::agentRelease, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("RequestPinCode", "o", "s", Bluetooth::agentRequestPinCode, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("RequestPasskey", "o", "u", Bluetooth::agentRequestPasskey, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("DisplayPasskey", "ouq", "", Bluetooth::agentDisplayPasskey, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("DisplayPinCode", "os", "", Bluetooth::agentDisplayPinCode, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("RequestConfirmation", "ou", "", Bluetooth::agentRequestConfirmation, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("RequestAuthorization", "o", "", Bluetooth::agentRequestAuthorization,
                  SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("AuthorizeService", "os", "", Bluetooth::agentAuthorizeService, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("Cancel", "", "", Bluetooth::agentCancel, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_VTABLE_END};

// One-shot timers: the callback runs the function, then lets go of its own event source.
struct Timer {
  std::function<void()> fn;
  sd_event_source *src = nullptr;
};

int onTimer(sd_event_source *, uint64_t, void *u) {
  auto *t = static_cast<Timer *>(u);
  t->fn();
  sd_event_source_unref(t->src);
  delete t;
  return 0;
}

struct Call {
  std::function<void(const std::string &)> done;
};

int onReply(sd_bus_message *reply, void *u, sd_bus_error *) {
  auto *c = static_cast<Call *>(u);
  std::string name;
  if (sd_bus_message_is_method_error(reply, nullptr)) {
    const sd_bus_error *e = sd_bus_message_get_error(reply);
    name = e && e->name ? e->name : "error";
  }
  c->done(name);
  delete c;
  return 0;
}

}  // namespace

// ---------------- set-up ----------------

Bluetooth::Bluetooth(sd_event *event, const Options &options, const bool &sessionActive)
    : event_(event), options_(options), sessionActive_(sessionActive) {
  aaw_.bt = hsp_.bt = this;
  aaw_.kind = "aaw";
  aaw_.path = AAW_PATH;
  hsp_.kind = "hsp";
  hsp_.path = HSP_PATH;

  int r = sd_bus_open_system(&bus_);
  if (r < 0) throw std::runtime_error(log::fmt("system bus: %s", std::strerror(-r)));
  r = sd_bus_attach_event(bus_, event_, SD_EVENT_PRIORITY_NORMAL);
  if (r < 0) throw std::runtime_error(log::fmt("bus on the event loop: %s", std::strerror(-r)));
  for (Profile *p : {&aaw_, &hsp_}) {
    r = sd_bus_add_object_vtable(bus_, &p->slot, p->path.c_str(), "org.bluez.Profile1", PROFILE_VTABLE, p);
    if (r < 0) throw std::runtime_error(log::fmt("exporting %s: %s", p->path.c_str(), std::strerror(-r)));
  }
  r = sd_bus_add_object_vtable(bus_, &agentSlot_, AGENT_PATH, "org.bluez.Agent1", AGENT_VTABLE, this);
  if (r < 0) throw std::runtime_error(log::fmt("exporting %s: %s", AGENT_PATH, std::strerror(-r)));

  auto must = [&](int rr, sd_bus_error &e, const char *what) {
    if (rr < 0) {
      std::string n = errName(e, rr), msg = e.message ? e.message : "";
      sd_bus_error_free(&e);
      throw std::runtime_error(log::fmt("%s: %s %s", what, n.c_str(), msg.c_str()));
    }
    sd_bus_error_free(&e);
  };
  sd_bus_error e = SD_BUS_ERROR_NULL;
  must(sd_bus_call_method(bus_, "org.bluez", "/org/bluez", "org.bluez.ProfileManager1", "RegisterProfile", &e,
                          nullptr, "osa{sv}", AAW_PATH, AAW_UUID, 5, "Name", "s", "AA Wireless", "Role", "s",
                          "server", "Channel", "q", RFCOMM_CHANNEL, "RequireAuthentication", "b", 0,
                          "RequireAuthorization", "b", 0),
       e, "RegisterProfile (AA wireless)");
  must(sd_bus_call_method(bus_, "org.bluez", "/org/bluez", "org.bluez.ProfileManager1", "RegisterProfile", &e,
                          nullptr, "osa{sv}", HSP_PATH, HSP_HS_UUID, 3, "Name", "s", "HSP HS",
                          "RequireAuthentication", "b", 0, "RequireAuthorization", "b", 0),
       e, "RegisterProfile (HSP HS)");
  must(sd_bus_call_method(bus_, "org.bluez", "/org/bluez", "org.bluez.AgentManager1", "RegisterAgent", &e, nullptr,
                          "os", AGENT_PATH, "DisplayYesNo"),
       e, "RegisterAgent");
  must(sd_bus_call_method(bus_, "org.bluez", "/org/bluez", "org.bluez.AgentManager1", "RequestDefaultAgent", &e,
                          nullptr, "o", AGENT_PATH),
       e, "RequestDefaultAgent");

  setAdapterProperty("Powered", 'b', 1);
  // Pairable only while pairing is open. That only matters for pairing started by the other side: the paired phone
  // reconnects either way, and bt_pair.sh pairs keyboards from this side.
  clearPairingState(options_.pairingFile);       // in case a --bt-only that died left one behind
  if (options_.pair)
    startPairing("--pair");
  else
    setAdapterProperty("Pairable", 'b', 0);
  std::string mac = adapterString("Address");
  AA_INFO("bt: profiles registered, adapter %s", mac.c_str());
  after(10000, [this]() { tick(); });
}

Bluetooth::~Bluetooth() {
  if (pairing_) clearPairingState(options_.pairingFile);
  for (Profile *p : {&aaw_, &hsp_}) {
    for (auto &kv : p->fds) ::close(kv.second.fd);
    sd_bus_slot_unref(p->slot);
  }
  sd_bus_slot_unref(agentSlot_);
  sd_bus_flush_close_unref(bus_);
}

// ---------------- helpers ----------------

void Bluetooth::after(uint64_t ms, std::function<void()> fn) {
  auto *t = new Timer{std::move(fn)};
  int r = sd_event_add_time_relative(event_, &t->src, CLOCK_MONOTONIC, ms * 1000, 10000, onTimer, t);
  if (r < 0) {
    AA_ERR("bt: timer: %s", std::strerror(-r));
    delete t;
  }
}

void Bluetooth::callAsync(const std::string &path, const char *iface, const char *member, const char *stringArg,
                          uint64_t timeoutMs, std::function<void(const std::string &)> done) {
  sd_bus_message *m = nullptr;
  int r = sd_bus_message_new_method_call(bus_, &m, "org.bluez", path.c_str(), iface, member);
  if (r >= 0 && stringArg) r = sd_bus_message_append(m, "s", stringArg);
  auto *c = new Call{std::move(done)};
  if (r >= 0) r = sd_bus_call_async(bus_, nullptr, m, onReply, c, timeoutMs * 1000);
  sd_bus_message_unref(m);
  if (r < 0) {
    delete c;
    throw std::runtime_error(log::fmt("%s: %s", member, std::strerror(-r)));
  }
}

bool Bluetooth::setAdapterProperty(const char *name, char type, uint64_t value) {
  // bluetoothd answers "Busy" while the controller is busy, just after start-up or while the Wi-Fi side of the
  // shared chip is under load. Retry instead of exiting.
  for (int attempt = 0; attempt < 10; ++attempt) {
    sd_bus_error e = SD_BUS_ERROR_NULL;
    int r = type == 'u' ? sd_bus_set_property(bus_, "org.bluez", options_.adapter.c_str(), "org.bluez.Adapter1",
                                              name, &e, "u", static_cast<uint32_t>(value))
                        : sd_bus_set_property(bus_, "org.bluez", options_.adapter.c_str(), "org.bluez.Adapter1",
                                              name, &e, "b", static_cast<int>(value));
    if (r >= 0) {
      sd_bus_error_free(&e);
      return true;
    }
    AA_WARN("bt: setting %s: %s (try %d)", name, errName(e, r).c_str(), attempt + 1);
    sd_bus_error_free(&e);
    ::sleep(1);
  }
  return false;
}

std::string Bluetooth::adapterString(const char *name) {
  sd_bus_error e = SD_BUS_ERROR_NULL;
  char *s = nullptr;
  int r = sd_bus_get_property_string(bus_, "org.bluez", options_.adapter.c_str(), "org.bluez.Adapter1", name, &e, &s);
  std::string out = r >= 0 && s ? s : "?";
  std::free(s);
  sd_bus_error_free(&e);
  return out;
}

bool Bluetooth::paired(const std::string &device) {
  sd_bus_error e = SD_BUS_ERROR_NULL;
  int v = 0;
  int r = sd_bus_get_property_trivial(bus_, "org.bluez", device.c_str(), "org.bluez.Device1", "Paired", &e, 'b', &v);
  if (r < 0) AA_WARN("bt: is %s paired? %s", device.c_str(), errName(e, r).c_str());
  sd_bus_error_free(&e);
  return r >= 0 && v;
}

void Bluetooth::trust(const std::string &device) {
  sd_bus_error e = SD_BUS_ERROR_NULL;
  int r = sd_bus_set_property(bus_, "org.bluez", device.c_str(), "org.bluez.Device1", "Trusted", &e, "b", 1);
  if (r < 0) AA_WARN("bt: trust failed: %s", errName(e, r).c_str());
  sd_bus_error_free(&e);
}

// Every org.bluez.Device1 that BlueZ knows (GetManagedObjects on its root), with the properties this file uses.
bool Bluetooth::devices(std::vector<Device> &out, std::string &error) {
  sd_bus_error e = SD_BUS_ERROR_NULL;
  sd_bus_message *m = nullptr;
  int r = sd_bus_call_method(bus_, "org.bluez", "/", "org.freedesktop.DBus.ObjectManager", "GetManagedObjects", &e,
                             &m, "");
  if (r < 0) {
    error = errName(e, r);
    sd_bus_error_free(&e);
    return false;
  }
  sd_bus_error_free(&e);
  auto fail = [&](int rr) {
    error = log::fmt("reading the device list: %s", std::strerror(-rr));
    sd_bus_message_unref(m);
    return false;
  };
  if ((r = sd_bus_message_enter_container(m, 'a', "{oa{sa{sv}}}")) < 0) return fail(r);
  while ((r = sd_bus_message_enter_container(m, 'e', "oa{sa{sv}}")) > 0) {
    const char *path = nullptr;
    if ((r = sd_bus_message_read(m, "o", &path)) < 0) return fail(r);
    Device d;
    d.path = path;
    bool device = false;
    if ((r = sd_bus_message_enter_container(m, 'a', "{sa{sv}}")) < 0) return fail(r);
    while ((r = sd_bus_message_enter_container(m, 'e', "sa{sv}")) > 0) {
      const char *iface = nullptr;
      if ((r = sd_bus_message_read(m, "s", &iface)) < 0) return fail(r);
      if (std::strcmp(iface, "org.bluez.Device1") != 0) {
        if ((r = sd_bus_message_skip(m, "a{sv}")) < 0) return fail(r);
      } else {
        device = true;
        if ((r = sd_bus_message_enter_container(m, 'a', "{sv}")) < 0) return fail(r);
        while ((r = sd_bus_message_enter_container(m, 'e', "sv")) > 0) {
          const char *key = nullptr, *contents = nullptr;
          char type = 0;
          if ((r = sd_bus_message_read(m, "s", &key)) < 0) return fail(r);
          if ((r = sd_bus_message_peek_type(m, &type, &contents)) < 0) return fail(r);
          std::string k = key;
          char want = (k == "Paired" || k == "Connected")                 ? 'b'
                      : (k == "Icon" || k == "Alias" || k == "Address") ? 's'
                                                                         : 0;
          if (!want || std::strlen(contents) != 1 || contents[0] != want) {
            if ((r = sd_bus_message_skip(m, "v")) < 0) return fail(r);
          } else {
            if ((r = sd_bus_message_enter_container(m, 'v', contents)) < 0) return fail(r);
            if (want == 'b') {
              int b = 0;
              if ((r = sd_bus_message_read(m, "b", &b)) < 0) return fail(r);
              (k == "Paired" ? d.paired : d.connected) = b != 0;
            } else {
              const char *s = nullptr;
              if ((r = sd_bus_message_read(m, "s", &s)) < 0) return fail(r);
              (k == "Icon" ? d.icon : k == "Alias" ? d.alias : d.address) = s;
            }
            if ((r = sd_bus_message_exit_container(m)) < 0) return fail(r);
          }
          if ((r = sd_bus_message_exit_container(m)) < 0) return fail(r);
        }
        if (r < 0) return fail(r);
        if ((r = sd_bus_message_exit_container(m)) < 0) return fail(r);
      }
      if ((r = sd_bus_message_exit_container(m)) < 0) return fail(r);
    }
    if (r < 0) return fail(r);
    if ((r = sd_bus_message_exit_container(m)) < 0) return fail(r);
    if ((r = sd_bus_message_exit_container(m)) < 0) return fail(r);
    if (device) out.push_back(std::move(d));
  }
  if (r < 0) return fail(r);
  sd_bus_message_unref(m);
  return true;
}

// Paired devices whose icon says phone.
bool Bluetooth::phones(std::vector<Phone> &out, std::string &error) {
  std::vector<Device> all;
  if (!devices(all, error)) return false;
  for (auto &d : all)
    if (d.paired && d.icon.compare(0, 5, "phone") == 0) out.push_back(Phone{d.path, d.alias, d.address, d.connected});
  return true;
}

// ---------------- the profiles ----------------

void Bluetooth::Profile::shut(int fd, const std::string &device) {
  // Close the link itself: bluetoothd keeps its own copy of the socket, so a plain close of ours would leave the
  // phone's headset connection up.
  ::shutdown(fd, SHUT_RDWR);   // fails harmlessly if the phone closed it first
  ::close(fd);
  AA_INFO("bt: %s link to %s closed", kind.c_str(), device.c_str());
}

void Bluetooth::Profile::drop(const std::string &device) {
  auto it = fds.find(device);
  if (it == fds.end()) return;
  int fd = it->second.fd;
  fds.erase(it);               // its timer then finds nothing to do
  shut(fd, device);
}

int Bluetooth::profileRelease(sd_bus_message *m, void *, sd_bus_error *) {
  return sd_bus_reply_method_return(m, "");
}

int Bluetooth::profileNewConnection(sd_bus_message *m, void *u, sd_bus_error *) {
  auto *p = static_cast<Profile *>(u);
  Bluetooth *bt = p->bt;
  const char *dev = nullptr;
  int fd0 = -1;
  int r = sd_bus_message_read(m, "oh", &dev, &fd0);
  if (r < 0) return r;
  std::string device = dev;
  int fd = ::fcntl(fd0, F_DUPFD_CLOEXEC, 3);   // the message closes its own copy
  if (fd < 0) return -errno;
  if (!bt->paired(device)) {   // the Wi-Fi details are for paired phones only
    AA_WARN("bt: %s connection from %s, which is not paired: closed", p->kind.c_str(), device.c_str());
    ::close(fd);
    return sd_bus_reply_method_return(m, "");
  }
  AA_INFO("bt: %s connection from %s", p->kind.c_str(), device.c_str());
  if (p->kind == "aaw") {
    bt->kicked_ = monotonicSeconds();   // the phone is starting a session: no nudge while it joins the Wi-Fi
    bt->hsp_.drop(device);     // the headset link has done its job
    touchFile(bt->options_.joiningFile);   // aa-ap makes no home Wi-Fi tries while the phone joins
    WirelessStart start{bt->options_.apConf, bt->options_.port};
    std::thread([start, fd]() { start.run(fd); }).detach();
  } else {
    // The headset link is only there to make the phone start Android Auto. Never keep it: to the phone a connected
    // headset is where calls go, and it takes calls away from the car's own hands-free (the head unit then shows
    // "switched to handheld"). So it is shut as soon as the phone opens the AA wireless link, or after 10 s, or
    // after 1 s during a session.
    p->drop(device);
    uint64_t gen = ++p->gen;
    p->fds[device] = Profile::Held{fd, gen};
    bt->after(bt->sessionActive_ ? 1000 : HSP_HOLD_MS, [p, device, gen]() {
      auto it = p->fds.find(device);
      if (it != p->fds.end() && it->second.gen == gen) {
        int f = it->second.fd;
        p->fds.erase(it);
        p->shut(f, device);
      }
    });
  }
  return sd_bus_reply_method_return(m, "");
}

int Bluetooth::profileRequestDisconnection(sd_bus_message *m, void *u, sd_bus_error *) {
  auto *p = static_cast<Profile *>(u);
  const char *dev = nullptr;
  int r = sd_bus_message_read(m, "o", &dev);
  if (r < 0) return r;
  AA_INFO("bt: %s disconnect request %s", p->kind.c_str(), dev);
  p->drop(dev);
  return sd_bus_reply_method_return(m, "");
}

// ---------------- pairing, opened and closed ----------------

void Bluetooth::startPairing(const char *why) {
  const uint64_t gen = ++pairGen_;
  const bool again = pairing_;
  pairing_ = true;
  pairingDevice_.clear();
  pairUntil_ = monotonicSeconds() + PAIR_SECONDS;
  setAdapterProperty("Pairable", 'b', 1);
  setAdapterProperty("DiscoverableTimeout", 'u', PAIR_SECONDS);
  setAdapterProperty("Discoverable", 'b', 1);
  publishPairing(0);
  AA_INFO("bt: discoverable as \"%s\" for 3 minutes: pair from the phone's Bluetooth settings (%s%s)",
          adapterString("Alias").c_str(), why, again ? "; the 3 minutes start over" : "");
  after(static_cast<uint64_t>(PAIR_SECONDS) * 1000 + 5000, [this, gen]() {
    if (pairing_ && gen == pairGen_) stopPairing("pairing time is over");
  });
}

void Bluetooth::stopPairing(const char *why) {
  if (!pairing_) return;
  pairing_ = false;
  pairingDevice_.clear();
  ++pairGen_;                                    // the open window's timers now do nothing
  setAdapterProperty("Pairable", 'b', 0);
  setAdapterProperty("Discoverable", 'b', 0);
  clearPairingState(options_.pairingFile);
  AA_INFO("bt: %s: not pairable any more", why);
}

void Bluetooth::publishPairing(uint32_t passkey) {
  if (!writePairingState(options_.pairingFile, pairUntil_, passkey))
    AA_WARN("bt: cannot write %s: aaview will not show that pairing is open", options_.pairingFile.c_str());
}

void Bluetooth::pairedWith(const std::string &device) {
  // Bonding finishes after the agent's answer. Give it 20 s, then close so no second phone pairs by accident.
  const uint64_t gen = pairGen_;
  pairingDevice_ = device;
  AA_INFO("bt: %s is pairing: pairing closes in 20 s", device.c_str());
  after(20000, [this, gen]() {
    if (pairing_ && gen == pairGen_) stopPairing("a phone paired");
  });
}

// ---------------- the pairing agent ----------------

int Bluetooth::refuse(const char *what, const std::string &device, sd_bus_error *e, const char *why) {
  AA_WARN("bt: %s from %s refused (%s)", what, device.c_str(), why);
  return sd_bus_error_set(e, "org.bluez.Error.Rejected", "not pairing");
}

// Only the first device to ask can pair until pairing closes.
bool Bluetooth::otherDevice(const std::string &device) {
  if (pairingDevice_.empty()) pairingDevice_ = device;
  return pairingDevice_ != device;
}

int Bluetooth::agentRelease(sd_bus_message *m, void *, sd_bus_error *) { return sd_bus_reply_method_return(m, ""); }

int Bluetooth::agentRequestPinCode(sd_bus_message *m, void *u, sd_bus_error *e) {
  auto *bt = static_cast<Bluetooth *>(u);
  const char *dev = nullptr;
  int r = sd_bus_message_read(m, "o", &dev);
  if (r < 0) return r;
  if (!bt->pairing_) return bt->refuse("PIN request", dev, e);
  if (bt->otherDevice(dev)) return bt->refuse("PIN request", dev, e, "pairing is held for another device");
  AA_INFO("bt: PIN requested by %s: using 0000", dev);
  bt->pairedWith(dev);
  return sd_bus_reply_method_return(m, "s", "0000");
}

int Bluetooth::agentRequestPasskey(sd_bus_message *m, void *u, sd_bus_error *e) {
  auto *bt = static_cast<Bluetooth *>(u);
  const char *dev = nullptr;
  int r = sd_bus_message_read(m, "o", &dev);
  if (r < 0) return r;
  if (!bt->pairing_) return bt->refuse("passkey request", dev, e);
  if (bt->otherDevice(dev)) return bt->refuse("passkey request", dev, e, "pairing is held for another device");
  return sd_bus_reply_method_return(m, "u", static_cast<uint32_t>(0));
}

int Bluetooth::agentDisplayPasskey(sd_bus_message *m, void *u, sd_bus_error *) {
  auto *bt = static_cast<Bluetooth *>(u);
  const char *dev = nullptr;
  uint32_t passkey = 0;
  uint16_t entered = 0;
  int r = sd_bus_message_read(m, "ouq", &dev, &passkey, &entered);
  if (r < 0) return r;
  AA_INFO("bt: passkey %06u", passkey);
  if (bt->pairing_) bt->publishPairing(passkey);
  return sd_bus_reply_method_return(m, "");
}

int Bluetooth::agentDisplayPinCode(sd_bus_message *m, void *, sd_bus_error *) {
  const char *dev = nullptr, *pin = nullptr;
  int r = sd_bus_message_read(m, "os", &dev, &pin);
  if (r < 0) return r;
  AA_INFO("bt: PIN %s", pin);
  return sd_bus_reply_method_return(m, "");
}

int Bluetooth::agentRequestConfirmation(sd_bus_message *m, void *u, sd_bus_error *e) {
  auto *bt = static_cast<Bluetooth *>(u);
  const char *dev = nullptr;
  uint32_t passkey = 0;
  int r = sd_bus_message_read(m, "ou", &dev, &passkey);
  if (r < 0) return r;
  if (!bt->pairing_) return bt->refuse(log::fmt("pairing (passkey %06u)", passkey).c_str(), dev, e);
  if (bt->otherDevice(dev))
    return bt->refuse(log::fmt("pairing (passkey %06u)", passkey).c_str(), dev, e,
                      "pairing is held for another device");
  AA_INFO("bt: pairing %s, passkey %06u: accepted (check it matches the phone)", dev, passkey);
  bt->publishPairing(passkey);                   // aaview shows it; the phone asks to confirm the same code
  bt->trust(dev);
  bt->pairedWith(dev);
  return sd_bus_reply_method_return(m, "");
}

int Bluetooth::agentRequestAuthorization(sd_bus_message *m, void *u, sd_bus_error *e) {
  auto *bt = static_cast<Bluetooth *>(u);
  const char *dev = nullptr;
  int r = sd_bus_message_read(m, "o", &dev);
  if (r < 0) return r;
  if (!bt->pairing_) return bt->refuse("pairing", dev, e);
  if (bt->otherDevice(dev)) return bt->refuse("pairing", dev, e, "pairing is held for another device");
  bt->trust(dev);
  bt->pairedWith(dev);
  return sd_bus_reply_method_return(m, "");
}

int Bluetooth::agentAuthorizeService(sd_bus_message *m, void *u, sd_bus_error *e) {
  auto *bt = static_cast<Bluetooth *>(u);
  const char *dev = nullptr, *uuid = nullptr;
  int r = sd_bus_message_read(m, "os", &dev, &uuid);
  if (r < 0) return r;
  if (!bt->paired(dev)) return bt->refuse(log::fmt("service %s", uuid).c_str(), dev, e);
  return sd_bus_reply_method_return(m, "");
}

int Bluetooth::agentCancel(sd_bus_message *m, void *, sd_bus_error *) {
  AA_INFO("bt: pairing cancelled");
  return sd_bus_reply_method_return(m, "");
}

// ---------------- connecting to the phone ----------------

void Bluetooth::tick() {
  autoconnect();
  after(10000, [this]() { tick(); });
}

void Bluetooth::autoconnect() {
  // Every 10 s with no AA session, connect paired phones' headset profile, which is what makes Android Auto start.
  // A phone that is connected but has not started a session for a minute is nudged: disconnected completely, then
  // connected again, as WirelessAndroidAutoDongle and aa-proxy-rs do. Connecting only the profile again is not
  // enough, because the phone's side of the old link stays up. Nudges that don't help come further apart (1, 2, 4,
  // then every 5 minutes) to spare the shared chip.
  if (sessionActive_) {
    idleSince_.reset();
    noreplies_.clear();
    nudges_ = inprogress_ = 0;
    awaySince_.reset();
    return;
  }
  double t = monotonicSeconds();
  if (!idleSince_) idleSince_ = t;
  std::vector<Phone> list;
  std::string error;
  if (!phones(list, error)) {
    AA_WARN("bt: autoconnect: %s", error.c_str());
    return;
  }
  try {
    for (auto &p : list) {
      auto pend = pending_.find(p.path);
      if (pend != pending_.end() && pend->second > t) continue;   // still busy with this one
      std::string who = p.alias + " (" + p.address + ")";
      if (!p.connected) {
        AA_INFO("bt: connecting to %s", who.c_str());
        connect(p.path);
      } else if (t - *idleSince_ > NUDGE_AFTER &&
                 t - kicked_ > std::min(NUDGE_AFTER * (1 << std::min(std::max(nudges_ - 1, 0), 8)), 300.0)) {
        kicked_ = t;
        nudges_ += 1;
        AA_WARN("bt: nudging %s: no session, so disconnecting it and connecting again (nudge %d)", who.c_str(),
                nudges_);
        resetLink(p.path, true);
      }
    }
  } catch (const std::exception &ex) {
    AA_WARN("bt: autoconnect: %s", ex.what());
  }
}

void Bluetooth::connect(const std::string &path) {
  pending_[path] = monotonicSeconds() + CONNECT_LIMIT + 5;
  callAsync(path, "org.bluez.Device1", "ConnectProfile", HSP_AG_UUID, static_cast<uint64_t>(CONNECT_LIMIT * 1000),
            [this, path](const std::string &err) {
              if (err.empty()) pending_.erase(path);
              else connectFailed(path, err);
            });
}

void Bluetooth::resetLink(const std::string &path, bool thenConnect) {
  // Drop the whole Bluetooth link to this device, and any headset socket still open, so the next connect starts
  // from nothing on both sides. This also clears a connect that BlueZ still has "in progress".
  hsp_.drop(path);
  pending_[path] = monotonicSeconds() + 15;
  callAsync(path, "org.bluez.Device1", "Disconnect", nullptr, 10000, [this, path, thenConnect](const std::string &err) {
    if (!err.empty()) AA_WARN("bt: disconnect: %s", err.c_str());
    if (thenConnect) {
      after(2000, [this, path]() {
        pending_.erase(path);
        if (!sessionActive_) {
          try {
            connect(path);
          } catch (const std::exception &ex) {
            AA_WARN("bt: connect: %s", ex.what());
          }
        }
      });
    } else {
      pending_.erase(path);
    }
  });
}

void Bluetooth::connectFailed(const std::string &path, const std::string &name) {
  // A connect that got no answer in 20 s, or "in progress" twice running: disconnect the device so the next try
  // starts clean, as aa-proxy-rs does.
  // A hung Bluetooth chip (kernel: "hci0: command 0x0c52 tx timeout", seen after access point churn and after a
  // receiver restart) answers every connect with NoReply. So does a phone that is simply out of range: bluetoothd
  // logs "Unable to get HSP HS SDP record: Host is down" after 5 s and never answers the call. So after three NoReply
  // within 5 minutes, ask the chip directly. If it doesn't answer, reset it by rebinding its UART driver and exit so
  // systemd restarts us with fresh profiles. If it does, the phone is away and we keep trying, and reset only after
  // 15 minutes of that in case something else is stuck.
  pending_.erase(path);
  AA_WARN("bt: connect: %s", name.c_str());
  bool noReply = endsWith(name, "NoReply");
  bool stuck = noReply;
  if (endsWith(name, "InProgress")) {
    inprogress_ += 1;          // one alone may be the phone connecting to us at the same moment
    stuck = inprogress_ >= 2;
  }
  if (stuck) {
    inprogress_ = 0;
    try {
      resetLink(path, false);
    } catch (const std::exception &ex) {
      AA_WARN("bt: disconnect: %s", ex.what());
    }
  }
  if (noReply) {               // a session starting clears the count
    double t = monotonicSeconds();
    noreplies_.erase(std::remove_if(noreplies_.begin(), noreplies_.end(), [t](double x) { return t - x >= 300; }),
                     noreplies_.end());
    noreplies_.push_back(t);
  }
  if (noreplies_.size() >= 3) {
    double t = monotonicSeconds();
    noreplies_.clear();
    if (chipAnswers()) {
      if (!awaySince_) {
        awaySince_ = t;
        AA_WARN("bt: 3 connects unanswered in 5 min, but the Bluetooth chip answers: the phone is probably out of "
                "range. Trying on");
      }
      if (t - *awaySince_ < RESET_ANYWAY) return;
      AA_ERR("bt: no connect answered for %d min: resetting the Bluetooth chip and restarting anyway",
             static_cast<int>(RESET_ANYWAY / 60));
    } else {
      AA_ERR("bt: the Bluetooth chip is hung (3 connects unanswered, and no answer to a command): resetting it and "
             "restarting");
    }
    const std::string link = "/sys/bus/serial/devices/" + options_.serialDevice + "/driver";
    char drv[4096];
    if (::realpath(link.c_str(), drv)) {
      bool ok = true;
      {
        std::ofstream f(std::string(drv) + "/unbind");
        f << options_.serialDevice;
        ok = ok && static_cast<bool>(f);
      }
      ::sleep(2);
      {
        std::ofstream f(std::string(drv) + "/bind");
        f << options_.serialDevice;
        ok = ok && static_cast<bool>(f);
      }
      ::sleep(5);
      if (!ok) AA_ERR("bt: reset failed: could not write %s/unbind or bind", drv);
    } else {
      AA_ERR("bt: reset failed: %s: %s", link.c_str(), std::strerror(errno));
    }
    std::fflush(stdout);
    ::_exit(3);
  }
}

void Bluetooth::connectNow() {
  // Skip both waits: connect now, or nudge a connected phone.
  idleSince_ = 0.0;
  kicked_ = 0;
  nudges_ = 0;
  after(0, [this]() { autoconnect(); });
}

// ---------------- the chip ----------------

bool chipAnswers(double waitSeconds) {
  // Sends HCI Read_BD_ADDR on a raw HCI socket, as hcitool does (safe next to bluetoothd), and waits for its Command
  // Complete. Changes nothing on the chip or its links.
  const int AF_BT = 31, BTPROTO_HCI = 1, SOL_HCI = 0, HCI_FILTER = 2;
  struct {
    sa_family_t family;
    uint16_t dev, channel;
  } addr{};
  int s = ::socket(AF_BT, SOCK_RAW | SOCK_CLOEXEC, BTPROTO_HCI);
  if (s < 0) return false;
  addr.family = AF_BT;   // device 0, the raw channel
  bool answered = false;
  do {
    if (::bind(s, reinterpret_cast<sockaddr *>(&addr), sizeof addr) < 0) break;
    // Filter: event packets only (type 4), every event, no opcode match.
    struct {
      uint32_t typeMask;
      uint32_t eventMask[2];
      uint16_t opcode;
      uint16_t pad;
    } filter{1u << 4, {0xffffffffu, 0xffffffffu}, 0, 0};
    if (::setsockopt(s, SOL_HCI, HCI_FILTER, &filter, sizeof filter) < 0) break;
    const uint8_t cmd[] = {0x01, 0x09, 0x10, 0x00};   // command packet, opcode 0x1009 Read_BD_ADDR, no parameters
    if (::send(s, cmd, sizeof cmd, 0) != static_cast<ssize_t>(sizeof cmd)) break;
    auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(waitSeconds);
    for (;;) {
      auto left = std::chrono::duration_cast<std::chrono::milliseconds>(end - std::chrono::steady_clock::now());
      if (left.count() <= 0) break;
      pollfd p{s, POLLIN, 0};
      if (::poll(&p, 1, static_cast<int>(left.count())) <= 0) break;
      uint8_t ev[260];
      ssize_t n = ::recv(s, ev, sizeof ev, 0);   // Command Complete: 04 0e len ncmd opcode(2) status ...
      if (n < 0) break;
      if (n >= 7 && ev[1] == 0x0e && ev[4] == 0x09 && ev[5] == 0x10) {
        answered = ev[6] == 0;
        break;
      }
    }
  } while (false);
  ::close(s);
  return answered;
}

}  // namespace aarecvd::bt
