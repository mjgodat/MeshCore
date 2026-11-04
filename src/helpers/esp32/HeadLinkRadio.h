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

private:
  void ensureLoaded();
  bool save();
  void apply();
  bool sendingUdp() const;
  bool sendingEspNow() const;
  static void onEspNowObserver(const uint8_t* frame, size_t len, void* ctx);
  static void onUdpObserver(const uint8_t* frame, size_t len, void* ctx);
  void deliverObserver(const uint8_t* frame, size_t len, bool via_espnow);

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
};

#endif
