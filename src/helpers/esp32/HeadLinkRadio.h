#pragma once

// The visual heads' radio when built with HEAD_UDP_TRANSPORT: ESPNOWRadio
// plus an optional UDP link to the repeater's WiFi UDP bridge, chosen by the
// head's `transport` setting (helpers/HeadTransport.h has the modes, the
// saved config, the auto fail-over and the CLI text).
//
// Mode espnow (the default) is the plain ESPNOWRadio: the UDP link is not
// allocated, WiFi is never joined, observer frames go straight to the UI.
// Every other mode allocates HeadUdpLink (about 1.5 KB of heap) and joins
// the configured access point, but only on this head's ESP-NOW channel
// (mesh::wifi::beginStation); a network found on another channel is
// reported (wrong-channel) and not joined, so ESP-NOW peers never lose it.
//
// The UDP link speaks the UDP bridge framing (UDPBridgeProtocol.h) to one
// peer, the repeater: HMAC-SHA256 with the repeater's bridge.secret, a
// challenge handshake per boot, replay-checked sequence numbers. Mesh frames
// carry raw MeshCore packets both ways; Observer frames carry the repeater's
// observer feed, requested with Subscribe every 10 s.

#if defined(ESP32) && defined(HEAD_UDP_TRANSPORT) && HEAD_UDP_TRANSPORT

#include <helpers/esp32/ESPNOWRadio.h>
#include <helpers/HeadTransport.h>
#include <helpers/HeadDiscovery.h>
#include <helpers/bridges/DiscoveryKey.h>
#include <helpers/bridges/UDPBridgeProtocol.h>
#include <helpers/StationJoin.h>
#include <freertos/FreeRTOS.h>

class HeadUdpLink;

class HeadLinkRadio : public ESPNOWRadio {
public:
  HeadLinkRadio();

  // mesh::Radio: mesh packets over the active transport(s).
  int recvRaw(uint8_t* bytes, int sz) override;
  bool startSendRaw(const uint8_t* bytes, int len) override;
  bool isSendComplete() override;
  void onSendFinished() override;
  void loop() override;

  // The visual head's observer sink (instead of ESPNOWRadio's), fed by both
  // transports; frames seen on both are passed on once.
  static void setUiObserverHandler(mesh::observer::FrameHandler handler, void* ctx);

  // `get/set transport`, `transport.failover`, `udp.*` (HeadTransport.h).
  // False when the command is not one of these.
  static bool handleCommand(const char* command, char* reply, size_t capacity);

  const mesh::headlink::Config& config();
  void status(mesh::headlink::Status& out);
#ifdef FREENOVE_UITEST
  void joinDebug(char* out, size_t cap);
#endif
  static HeadLinkRadio* instance() { return _instance; }

  // Discovery (HeadDiscovery.h): this head's key prefix and name for its
  // replies; set once by the companion at boot.
  static void setIdentity(const uint8_t* pub_key, const char* name);
  // The repeater learned from announces (udp.peer off); port 0 = none yet.
  const mesh::bridge::udp::Peer& learnedPeer() const { return _disc.learnedPeer(); }
  // The first announced repeater, offered once as home (main loop).
  bool takeAnnouncedHome(uint8_t out[6]) { return _disc.takeHome(out); }

private:
  void ensureLoaded();
  bool save();
  void apply();
  bool sendingUdp() const;
  bool sendingEspNow() const;
  static void onEspNowObserver(const uint8_t* frame, size_t len, void* ctx);
  static void onUdpObserver(const uint8_t* frame, size_t len, void* ctx);
  void deliverObserver(const uint8_t* frame, size_t len, bool via_espnow);
  static void onEspNowDiscovery(const uint8_t* frame, size_t len, void* ctx);
  static void onUdpDiscovery(const uint8_t* frame, size_t len, const uint8_t ip[4], uint16_t port, void* ctx);
  void queueDiscovery(const uint8_t* frame, size_t len, const uint8_t* ip, uint16_t port, bool via_espnow);
  void serviceDiscovery();

  static HeadLinkRadio* _instance;
  mesh::headlink::Config _cfg;
  // The mode as the WiFi task sees it: written only under _mux (apply()).
  mesh::headlink::Mode _rx_mode = mesh::headlink::Mode::EspNow;
  HeadUdpLink* _link = nullptr;
  bool _loaded = false;
  bool _tx_espnow = false;        // the current send went out on ESP-NOW
  mesh::headlink::AutoSelector _auto;
  mesh::headlink::FrameDeduper _dedupe;
  uint32_t _espnow_vitals_at = 0, _udp_vitals_at = 0;
  bool _espnow_vitals_seen = false, _udp_vitals_seen = false;
  portMUX_TYPE _mux = portMUX_INITIALIZER_UNLOCKED;
  // Announces handed over by the WiFi task (ESP-NOW) or the UDP link, under _mux.
  struct PendingAnnounce {
    uint8_t frame[mesh::discovery::MAX_FRAME];
    uint8_t len;
    uint8_t ip[4];
    uint16_t port;
    bool via_espnow;
  };
  // Room for a few frames; a flood (good or bad tags) drops the oldest, so
  // the latest real announce is never starved out.
  static const uint8_t kPendingAnnounces = 4;
  PendingAnnounce _pending[kPendingAnnounces];
  uint8_t _pending_count = 0;
  mesh::headlink::HeadDiscovery _disc;
  mesh::discovery::KeyCache _udp_key, _espnow_key;   // derived once per secret
  uint32_t _disc_bad = 0;
};

#endif
