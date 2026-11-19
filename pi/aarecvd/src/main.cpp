/*
 *  aarecvd - wireless Android Auto receiver for the Veloster head unit (veloster-aa).
 *  Copyright (C) 2026 veloster-aa contributors
 *  Built on aasdk (GPLv3, f1x.studio / opencardev) and structured like openauto (GPLv3, f1x.studio).
 *
 *  This program is free software: you can redistribute it and/or modify it under the terms of the GNU General
 *  Public License as published by the Free Software Foundation, either version 3 of the License, or (at your
 *  option) any later version. It is distributed WITHOUT ANY WARRANTY; see <http://www.gnu.org/licenses/>.
 */
// The Pi's Android Auto receiver, in C++ on aasdk. The normal mode runs the session: the phone's TCP connection
// (after the Bluetooth start-up), TLS, every channel, and the stream LUN to aaview on the head unit. With
// --bt-only it runs the other half instead, the Bluetooth start-up (Bluetooth/BtOnly.hpp), in a process of its
// own next to the session one, the way OpenAuto runs btservice next to autoapp. Two small tools for the shell
// scripts round it out: --show-settings (veloaa.txt) and --quit-aaview / --unquit-aaview.
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

#include <aasdk/Common/ModernLogger.hpp>
#include <boost/asio.hpp>

#include "App.hpp"
#include "Bluetooth/BtOnly.hpp"
#include "Config.hpp"
#include "Log.hpp"
#include "Projection/RingInputDevice.hpp"
#include "Projection/StreamRing.hpp"
#include "Service/ServiceFactory.hpp"

namespace {

const char *USAGE =
    "aarecvd: the Android Auto session (TCP 5288, TLS, all channels) for veloster-aa, on aasdk.\n"
    "Pairs with a second process for the Bluetooth start-up: 'aarecvd --bt-only'.\n"
    "\n"
    "  aarecvd [options]                      the session\n"
    "  aarecvd --bt-only [--pair] [--no-bt]   the Bluetooth start-up: AA wireless and headset profiles, pairing\n"
    "                                         agent, connecting to the paired phone; listens on --notify-socket\n"
    "  aarecvd --show-settings [--shell]      the settings in force (--settings, --loglevel-file), in words; --shell:\n"
    "                                         as key=value lines (for aa-ap and install.sh)\n"
    "  aarecvd --quit-aaview | --unquit-aaview   set / clear the quit flag in the stream LUN's header (--stream):\n"
    "                                         aaview on the head unit quits cleanly; clear it before starting aaview\n"
    "    --pair                 (--bt-only) discoverable and pairable for 3 minutes; logs the passkey (a 3 s hold\n"
    "                           on aaview's icon does the same)\n"
    "    --pairing-file PATH    where --bt-only says pairing is open, for aaview (/run/veloaa/pairing)\n"
    "    --joining-file PATH    touched by --bt-only when a phone starts joining the Wi-Fi, for aa-ap\n"
    "                           (/run/veloaa/joining)\n"
    "    --no-bt                (--bt-only) no Bluetooth: only listen (the phone starts some other way)\n"
    "    --bt-serial-device D   (--bt-only) the chip's UART device under /sys/bus/serial/devices, rebound to reset\n"
    "                           a hung chip (serial0-0)\n"
    "    --port N               TCP port (5288)\n"
    "    --any-client           accept the phone's TCP connection from any network, not only the access point's\n"
    "    --ap-conf PATH         where the access point's ip= is (/etc/aarecv.conf)\n"
    "    --stream PATH          the stream LUN image (/run/veloaa/stream.img)\n"
    "    --notify-socket PATH   datagram socket of the --bt-only process (/run/veloaa/aarecv.sock; '' = none)\n"
    "    --settings PATH        settings file (/boot/firmware/veloaa.txt)\n"
    "    --loglevel-file PATH   dev log level file (/etc/aarecv/loglevel)\n"
    "    --keys-conf PATH       key overrides (/etc/aarecv-keys.conf)\n"
    "    --cert PATH, --key PATH   the head unit's TLS certificate and key, PEM (/etc/aarecv/hu.crt, hu.key); when\n"
    "                           missing or not a matching pair: aasdk's built-in one ('' = always the built-in one)\n"
    "    --level N              rewrite SPS level_idc (12; 0 = leave it)\n"
    "    --dpi N                (140)\n"
    "    --fps 30|60            video frame rate asked of the phone (30)\n"
    "    --driver-position left|right|centre   (left: the Veloster)\n"
    "    --unacked N            video frames the phone may send before our ack (8)\n"
    "    --audio-unacked N      audio chunks the phone may send before our ack (16)\n"
    "    --ping S               seconds between our pings to the phone (5; 0 = none)\n"
    "    --timeout S            seconds of silence (or of a blocked send) after which the phone is taken for gone (15)\n"
    "    --status-services      also offer phone status, media playback and navigation status (logged only)\n"
    "    --aasdk-log LEVEL      aasdk's own log on stderr: off (default), error, warning, info, debug\n"
    "    --threads N            I/O threads (4)\n";

}  // namespace

int main(int argc, char **argv) {
  using namespace aarecvd;
  service::Options options;
  AppOptions appOptions;
  log::Paths logPaths;
  std::string stream = "/run/veloaa/stream.img", notifyPath = "/run/veloaa/aarecv.sock", aasdkLog = "off";
  int threads = 4;
  bool btOnly = false, pair = false, noBt = false, showSettings = false, shell = false, dump = false;
  int quitAaview = -1;   // 1 --quit-aaview, 0 --unquit-aaview
  std::string btSerial = "serial0-0", pairingFile = "/run/veloaa/pairing", joiningFile = "/run/veloaa/joining";

  std::vector<std::string> args(argv + 1, argv + argc);
  for (size_t i = 0; i < args.size(); ++i) {
    std::string a = args[i], v;
    auto eq = a.find('=');
    bool inlineValue = a.rfind("--", 0) == 0 && eq != std::string::npos;
    if (inlineValue) {
      v = a.substr(eq + 1);
      a = a.substr(0, eq);
    }
    auto value = [&]() -> std::string {
      if (inlineValue) return v;
      if (i + 1 >= args.size()) {
        std::fprintf(stderr, "aarecvd: %s needs a value\n", a.c_str());
        std::exit(2);
      }
      return args[++i];
    };
    try {
      if (a == "--help" || a == "-h") {
        std::fputs(USAGE, stdout);
        return 0;
      } else if (a == "--port") appOptions.port = static_cast<uint16_t>(std::stoi(value()));
      else if (a == "--any-client") appOptions.anyClient = true;
      else if (a == "--bt-only") btOnly = true;
      else if (a == "--show-settings") showSettings = true;
      else if (a == "--shell") shell = true;
      else if (a == "--dump") dump = true;          // --show-settings output for the desk test
      else if (a == "--quit-aaview") quitAaview = 1;
      else if (a == "--unquit-aaview") quitAaview = 0;
      else if (a == "--pair") pair = true;
      else if (a == "--no-bt") noBt = true;
      else if (a == "--bt-serial-device") btSerial = value();   // the tests point this away from the real chip
      else if (a == "--pairing-file") pairingFile = value();
      else if (a == "--joining-file") joiningFile = value();
      else if (a == "--ap-conf") appOptions.apConf = value();
      else if (a == "--stream") stream = value();
      else if (a == "--notify-socket") notifyPath = value();
      else if (a == "--settings") logPaths.settings = value();
      else if (a == "--loglevel-file") logPaths.loglevelFile = value();
      else if (a == "--keys-conf") options.keysConf = value();
      else if (a == "--cert") options.certFile = value();
      else if (a == "--key") options.keyFile = value();
      else if (a == "--level") options.level = std::stoi(value());
      else if (a == "--dpi") options.dpi = std::stoi(value());
      else if (a == "--fps") {
        options.fps = std::stoi(value());
        if (options.fps != 30 && options.fps != 60) throw std::invalid_argument("--fps is 30 or 60");
      } else if (a == "--driver-position") {
        const std::string dp = value();
        if (dp == "left") options.driverPosition = 0;
        else if (dp == "right") options.driverPosition = 1;
        else if (dp == "centre" || dp == "center") options.driverPosition = 2;
        else throw std::invalid_argument("--driver-position is left, right or centre");
      }
      else if (a == "--unacked") options.unacked = std::stoi(value());
      else if (a == "--audio-unacked") options.audioUnacked = std::stoi(value());
      else if (a == "--ping") options.ping = std::stod(value());
      else if (a == "--timeout") options.timeout = std::stod(value());
      else if (a == "--status-services") options.statusServices = true;
      else if (a == "--aasdk-log") aasdkLog = value();
      else if (a == "--threads") threads = std::max(2, std::stoi(value()));
      else {
        std::fprintf(stderr, "aarecvd: unknown argument %s\n\n%s", a.c_str(), USAGE);
        return 2;
      }
    } catch (const std::exception &) {
      std::fprintf(stderr, "aarecvd: bad value for %s\n", a.c_str());
      return 2;
    }
  }

  if (showSettings) return config::showSettings(logPaths.settings, logPaths.loglevelFile, shell, dump);
  if (shell || dump) {
    std::fprintf(stderr, "aarecvd: --shell and --dump go with --show-settings\n");
    return 2;
  }
  if (quitAaview >= 0) {
    try {
      projection::StreamRing::setQuitFlag(stream, quitAaview == 1);
    } catch (const std::exception &e) {
      std::fprintf(stderr, "aarecvd: %s\n", e.what());
      return 1;
    }
    std::printf("quit flag %s\n", quitAaview == 1 ? "set" : "cleared");
    return 0;
  }

  std::signal(SIGPIPE, SIG_IGN);
  log::init(logPaths);
  {
    using aasdk::common::LogLevel;
    auto &l = aasdk::common::ModernLogger::getInstance();
    l.setLevel(aasdkLog == "debug"     ? LogLevel::DEBUG
               : aasdkLog == "info"    ? LogLevel::INFO
               : aasdkLog == "warning" ? LogLevel::WARN
               : aasdkLog == "error"   ? LogLevel::ERROR
                                       : LogLevel::FATAL);
  }
  if (btOnly) {
    bt::Options btOptions;
    btOptions.pair = pair;
    btOptions.apConf = appOptions.apConf;
    btOptions.port = appOptions.port;
    btOptions.serialDevice = btSerial;
    btOptions.pairingFile = pairingFile;
    btOptions.joiningFile = joiningFile;
    if (notifyPath.empty()) {
      AA_ERR("aa: --bt-only needs a --notify-socket");
      return 2;
    }
    return bt::runBtOnly(btOptions, notifyPath, noBt);
  }
  if (pair || noBt) AA_WARN("aa: --pair and --no-bt are for --bt-only: ignored");

  std::shared_ptr<projection::StreamRing> ring;
  try {
    ring = std::make_shared<projection::StreamRing>(stream);
  } catch (const std::exception &e) {
    AA_ERR("aa: %s", e.what());
    return 1;
  }

  boost::asio::io_service ioService;
  boost::asio::io_service::work work(ioService);
  boost::asio::signal_set signals(ioService, SIGINT, SIGTERM);
  signals.async_wait([&ioService](const boost::system::error_code &, int) { ioService.stop(); });

  auto notifier = std::make_shared<Notifier>(notifyPath);
  auto inputDevice = std::make_shared<projection::RingInputDevice>(ring, options.keysConf, 800, 480);
  inputDevice->run([notifier](int state) {
    AA_INFO("aa: icon tapped (state %d) with no phone session: %s", state,
            "telling aarecvd --bt-only to connect to the phone now");
    notifier->send("icon " + std::to_string(state));
  }, [notifier](bool open) { notifier->send(open ? "pair on" : "pair off"); }, pairingFile);

  service::ServiceFactory serviceFactory(ioService, options, ring, inputDevice);
  service::AndroidAutoEntityFactory entityFactory(ioService, options, serviceFactory, inputDevice);
  std::shared_ptr<App> app;
  try {
    app = std::make_shared<App>(ioService, appOptions, entityFactory, ring, notifier);
  } catch (const std::exception &e) {
    AA_ERR("aa: cannot listen on TCP %d: %s", static_cast<int>(appOptions.port), e.what());
    inputDevice->shutdown();
    return 1;
  }
  app->start();

  std::vector<std::thread> pool;
  for (int i = 0; i < threads; ++i)
    pool.emplace_back([&ioService]() {
      for (;;) {
        try {
          ioService.run();
          return;
        } catch (const std::exception &e) {
          AA_ERR("aa: unexpected exception: %s", e.what());
        }
      }
    });
  for (auto &t : pool) t.join();
  AA_INFO("aa: stopping");
  inputDevice->shutdown();
  std::fflush(stdout);
  std::_Exit(0);   // sessions may still hold sockets and timers; nothing more needs to run
}
