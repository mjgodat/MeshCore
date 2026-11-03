#include "HeadLinkRadio.h"

#if defined(ESP32) && defined(HEAD_UDP_TRANSPORT) && HEAD_UDP_TRANSPORT

#include <Arduino.h>
#include <WiFi.h>
#include <Preferences.h>
#include <SHA256.h>
#include <errno.h>
#include <fcntl.h>
#include <lwip/sockets.h>
#include <esp_system.h>
#include <new>
#include <string.h>
#include <helpers/esp32/WiFiRadioPolicy.h>
#include <helpers/esp32/WiFiStationPolicy.h>
#include <helpers/esp32/StationJoinDriver.h>
#include <helpers/bridges/ObserverFrame.h>

namespace udp = mesh::bridge::udp;
namespace hl = mesh::headlink;

// ---------------------------------------------------------------------------
// HeadUdpLink: one authenticated UDP bridge session with the repeater.
// Main loop only. Allocated only while the transport is not espnow.
// ---------------------------------------------------------------------------
class HeadUdpLink {
public:
  using ObserverSink = void (*)(const uint8_t* frame, size_t len, void* ctx);

  HeadUdpLink(const hl::Config& cfg, ObserverSink sink, void* ctx)
      : _cfg(cfg), _sink(sink), _ctx(ctx), _join(kDiagnoseEveryMs) {
    _secret_len = (uint8_t)strnlen(cfg.secret, sizeof(cfg.secret));
    _join.reset(millis());
    _driver.power_save = WIFI_PS_NONE;
    _driver.after_begin = ESPNOWRadio::forceLongRangeRate;
  }
  ~HeadUdpLink() {
    closeSocket();
    _join.stop(_driver);
    // Back to the LR-only ESP-NOW radio on its own channel.
    mesh::wifi::applyStationProtocolMask(WIFI_PROTOCOL_LR, true);
    mesh::wifi::restoreEspNowChannel();
    memset(_cfg.password, 0, sizeof(_cfg.password));
    memset(_cfg.secret, 0, sizeof(_cfg.secret));
  }

  void loop() {
    const uint32_t now = millis();
    serviceStation(now);
    if (_socket < 0) return;
    pumpReceive();
    pumpHello(now);
    pumpSubscribe(now);
  }

  bool send(const uint8_t* bytes, size_t len) {
    if (_socket < 0 || !_session.remote || len < 2 || len > udp::MAX_PAYLOAD) return false;
    const bool ok = sendFrame(udp::Kind::Mesh, _session.remote, bytes, (uint16_t)len);
    if (ok) ++_tx; else ++_tx_fail;
    return ok;
  }

  int recv(uint8_t* out, int cap) {
    if (!_rx_count || !out || cap <= 0) return 0;
    const uint8_t slot = (uint8_t)((_rx_head + kRxDepth - _rx_count) % kRxDepth);
    --_rx_count;
    if (_rx_len[slot] > cap) return 0;
    memcpy(out, _rx[slot], _rx_len[slot]);
    return _rx_len[slot];
  }

  hl::LinkState state() const {
    if (!hl::udpConfigured(_cfg)) return hl::LinkState::NotConfigured;
    if (_socket >= 0) return _session.remote ? hl::LinkState::Ready : hl::LinkState::Connected;
    const uint8_t ap = _join.apChannel();
    const bool off_channel = _join.reason() == mesh::wifi::JoinReason::ApMoved
        || (_join.reason() == mesh::wifi::JoinReason::NotOnChannel && ap
            && ap != mesh::wifi::activeEspNowChannel());
    return off_channel ? hl::LinkState::WrongChannel : hl::LinkState::Joining;
  }
  bool ready() const { return _socket >= 0 && _session.remote; }
  uint8_t apChannel() const { return _join.apChannel(); }
  uint32_t localAddress() const { return _local; }

private:
  static const uint8_t kRxDepth = 4;
  static const uint32_t kDiagnoseEveryMs = 600000;   // all-channel look, at most every 10 min

  static bool authenticate(void* context, const uint8_t* data, size_t size, uint8_t digest[32]) {
    const auto* self = static_cast<const HeadUdpLink*>(context);
    if (!self->_secret_len) return false;
    SHA256 sha;
    static const uint8_t domain[] = "MeshCore UDP bridge v1";   // as UDPBridge.cpp
    sha.resetHMAC(self->_cfg.secret, self->_secret_len);
    sha.update(domain, sizeof(domain));
    sha.update(data, size);
    sha.finalizeHMAC(self->_cfg.secret, self->_secret_len, digest, 32);
    return true;
  }
  static uint64_t randomNonce() {
    uint64_t v;
    do { v = (uint64_t(esp_random()) << 32) | esp_random(); } while (!v);
    return v;
  }

  // StationJoin.h: never blocks, joins only on the ESP-NOW channel, drops an
  // association that leaves it (AP channel switch) and backs off 15 s..2 min.
  // When the network is not on the ESP-NOW channel, one all-channel scan at
  // most every 10 min finds where it is (ESP-NOW receive pauses ~1.5 s).
  void serviceStation(uint32_t now) {
    _join.poll(_driver, now, _cfg.ssid, _cfg.password);
    if (_join.connected()) {
      if (_socket < 0) openSocket(static_cast<uint32_t>(WiFi.localIP()));
      return;
    }
    if (_socket >= 0) closeSocket();
  }

  void openSocket(uint32_t local) {
    _socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (_socket < 0) return;
    const int flags = fcntl(_socket, F_GETFL, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET; addr.sin_port = htons(_cfg.local_port); addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (flags < 0 || fcntl(_socket, F_SETFL, flags | O_NONBLOCK) < 0
        || bind(_socket, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
      closeSocket();
      return;
    }
    _local = local;
    _boot = randomNonce();
    _sequence = 0;
    _session = udp::Session{};
    _session.next_hello = millis();
    _subscribe_at = 0;
  }

  void closeSocket() {
    if (_socket >= 0) { close(_socket); _socket = -1; }
    _session = udp::Session{};
    _rx_count = 0;
    _local = 0;
  }

  bool sendFrame(udp::Kind kind, uint64_t recipient, const uint8_t* payload, uint16_t length) {
    if (_socket < 0 || _sequence == UINT32_MAX) return false;
    udp::Frame f;
    f.kind = kind; f.sender = _boot; f.recipient = recipient; f.sequence = ++_sequence;
    f.payload = payload; f.length = length;
    uint8_t bytes[udp::MAX_FRAME];
    const size_t size = udp::encode(bytes, sizeof(bytes), f, authenticate, this);
    if (!size) return false;
    sockaddr_in to{};
    to.sin_family = AF_INET; to.sin_port = htons(_cfg.repeater.port);
    memcpy(&to.sin_addr.s_addr, _cfg.repeater.ip, 4);
    return sendto(_socket, bytes, size, MSG_DONTWAIT, reinterpret_cast<const sockaddr*>(&to), sizeof(to))
        == static_cast<int>(size);
  }

  void pumpHello(uint32_t now) {
    if (_session.candidate && !udp::due(now, _session.challenge_deadline)
        && uint32_t(now - _session.challenge_sent_at) >= 1000) {
      uint8_t nonce[8]; udp::put64(nonce, _session.challenge);
      _session.challenge_sent_at = now;
      sendFrame(udp::Kind::Challenge, _session.candidate, nonce, sizeof(nonce));
      return;
    }
    if (!udp::due(now, _session.next_hello)) return;
    _session.next_hello = now + (_session.remote ? 10000U : 1000U) + esp_random() % 500;
    sendFrame(udp::Kind::Hello, 0, nullptr, 0);
  }

  void pumpSubscribe(uint32_t now) {
    // The repeater keeps a subscription 60 s; refresh every 10 s.
    if (!_session.remote || (_subscribe_at && !udp::due(now, _subscribe_at))) return;
    _subscribe_at = (now + 10000U) | 1U;
    const uint8_t on = 1;
    sendFrame(udp::Kind::Subscribe, _session.remote, &on, 1);
  }

  void pumpReceive() {
    for (uint8_t count = 0; count < 4 && _socket >= 0; ++count) {
      uint8_t bytes[udp::MAX_FRAME + 1];
      sockaddr_in from{}; socklen_t from_len = sizeof(from);
      const int n = recvfrom(_socket, bytes, sizeof(bytes), MSG_DONTWAIT,
                             reinterpret_cast<sockaddr*>(&from), &from_len);
      if (n < 0) return;
      if (from_len != sizeof(from) || from.sin_family != AF_INET
          || from.sin_port != htons(_cfg.repeater.port) || memcmp(&from.sin_addr.s_addr, _cfg.repeater.ip, 4))
        continue;   // only the configured repeater
      receiveFrame(bytes, (size_t)n);
    }
  }

  void receiveFrame(const uint8_t* data, size_t len) {
    udp::Frame f;
    if (udp::decode(data, len, f, authenticate, this) != udp::Decode::Ok || f.sender == _boot) return;
    const uint32_t now = millis();
    if (f.kind == udp::Kind::Hello) {
      if (_session.request(f.sender, randomNonce(), now)) {
        uint8_t nonce[8]; udp::put64(nonce, _session.challenge);
        sendFrame(udp::Kind::Challenge, f.sender, nonce, sizeof(nonce));
      }
      return;
    }
    if (f.recipient != _boot) return;
    if (f.kind == udp::Kind::Challenge) { sendFrame(udp::Kind::Proof, f.sender, f.payload, f.length); return; }
    if (f.kind == udp::Kind::Proof) {
      const bool changed = _session.remote != f.sender;
      if (_session.prove(f.sender, udp::get64(f.payload), now) && changed) {
        _session.next_hello = now;   // let the repeater prove itself to us at once
        _subscribe_at = 0;
      }
      return;
    }
    if (!_session.admit(f.sender, f.recipient, _boot, f.sequence)) return;
    if (f.kind == udp::Kind::Mesh) {
      if (_rx_count == kRxDepth) --_rx_count;   // drop the oldest
      memcpy(_rx[_rx_head], f.payload, f.length);
      _rx_len[_rx_head] = f.length;
      _rx_head = (uint8_t)((_rx_head + 1) % kRxDepth);
      ++_rx_count;
    } else if (f.kind == udp::Kind::Observer && _sink
               && mesh::observer::isObserverFrame(f.payload, f.length)) {
      _sink(f.payload, f.length, _ctx);
    }
  }

  hl::Config _cfg;
  ObserverSink _sink;
  void* _ctx;
  uint8_t _secret_len = 0;
  int _socket = -1;
  uint32_t _local = 0, _subscribe_at = 0, _sequence = 0;
  uint32_t _tx = 0, _tx_fail = 0;
  uint64_t _boot = 0;
  udp::Session _session;
  mesh::wifi::StationJoin<mesh::wifi::StationJoinDriver> _join;
  mesh::wifi::StationJoinDriver _driver;
  uint8_t _rx[kRxDepth][udp::MAX_PAYLOAD];
  uint16_t _rx_len[kRxDepth] = {};
  uint8_t _rx_head = 0, _rx_count = 0;
};

// ---------------------------------------------------------------------------
// HeadLinkRadio
// ---------------------------------------------------------------------------
HeadLinkRadio* HeadLinkRadio::_instance = nullptr;
static std::atomic<mesh::observer::FrameHandler> ui_handler{nullptr};
static std::atomic<void*> ui_ctx{nullptr};

HeadLinkRadio::HeadLinkRadio() {
  _instance = this;
  ESPNOWRadio::setObserverHandler(onEspNowObserver, this);
}

void HeadLinkRadio::setUiObserverHandler(mesh::observer::FrameHandler handler, void* ctx) {
  ui_ctx.store(ctx, std::memory_order_release);
  ui_handler.store(handler, std::memory_order_release);
}

void HeadLinkRadio::ensureLoaded() {
  if (_loaded) return;
  _loaded = true;
  Preferences nvs;
  if (nvs.begin("headlink", true)) {
    uint8_t image[hl::kImageSize];
    if (nvs.getBytesLength("cfg") == sizeof(image) && nvs.getBytes("cfg", image, sizeof(image)) == sizeof(image))
      hl::decodeConfig(image, sizeof(image), _cfg);   // a bad record leaves the defaults (espnow)
    memset(image, 0, sizeof(image));
    nvs.end();
  }
  _auto.begin(millis());
  apply();
}

bool HeadLinkRadio::save() {
  uint8_t image[hl::kImageSize];
  hl::encodeConfig(image, _cfg);
  Preferences nvs;
  bool ok = nvs.begin("headlink", false);
  if (ok) {
    ok = nvs.putBytes("cfg", image, sizeof(image)) == sizeof(image);
    nvs.end();
  }
  memset(image, 0, sizeof(image));
  return ok;
}

void HeadLinkRadio::apply() {
  delete _link;
  _link = nullptr;
  portENTER_CRITICAL(&_mux);
  _dedupe.reset();
  _auto.begin(millis());
  _rx_mode = _cfg.mode;
  portEXIT_CRITICAL(&_mux);
  if (!hl::usesWiFi(_cfg.mode)) return;
  _link = new (std::nothrow) HeadUdpLink(_cfg, onUdpObserver, this);
}

const hl::Config& HeadLinkRadio::config() { ensureLoaded(); return _cfg; }

bool HeadLinkRadio::sendingUdp() const {
  switch (_cfg.mode) {
    case hl::Mode::Udp: case hl::Mode::Both: return true;
    case hl::Mode::Auto: return _auto.onUdp();
    default: return false;
  }
}
bool HeadLinkRadio::sendingEspNow() const {
  return _cfg.mode == hl::Mode::EspNow || _cfg.mode == hl::Mode::Both
      || (_cfg.mode == hl::Mode::Auto && !_auto.onUdp());
}

void HeadLinkRadio::status(hl::Status& s) {
  ensureLoaded();
  const uint32_t now = millis();
  s = hl::Status{};
  s.espnow_channel = mesh::wifi::activeEspNowChannel();
  s.sending_udp = _cfg.mode == hl::Mode::Auto && _auto.onUdp();
  if (!hl::usesWiFi(_cfg.mode)) s.udp = hl::LinkState::Off;
  else if (!_link) s.udp = hl::LinkState::NoMemory;
  else {
    s.udp = _link->state();
    s.ap_channel = _link->apChannel();
    const uint32_t ip = _link->localAddress();
    memcpy(s.local_ip, &ip, 4);
  }
  portENTER_CRITICAL(&_mux);
  if (_espnow_vitals_seen) s.espnow_vitals_age_s = (now - _espnow_vitals_at) / 1000;
  if (_udp_vitals_seen) s.udp_vitals_age_s = (now - _udp_vitals_at) / 1000;
  portEXIT_CRITICAL(&_mux);
}

bool HeadLinkRadio::handleCommand(const char* command, char* reply, size_t capacity) {
  HeadLinkRadio* self = _instance;
  if (!self || !command) return false;
  self->ensureLoaded();
  hl::Status st;
  self->status(st);
  hl::Config candidate = self->_cfg;
  const hl::Result r = hl::handleCommand(candidate, command, reply, capacity, st);
  if (r == hl::Result::NotMine) return false;
  if (r == hl::Result::Changed) {
    const hl::Config previous = self->_cfg;
    self->_cfg = candidate;
    if (!self->save()) {
      self->_cfg = previous;
      snprintf(reply, capacity, "Error: transport settings not saved");
    } else {
      self->apply();
    }
    memset(&candidate, 0, sizeof(candidate));
  }
  return true;
}

// WiFi task (ESP-NOW receive callback).
void HeadLinkRadio::onEspNowObserver(const uint8_t* frame, size_t len, void* ctx) {
  static_cast<HeadLinkRadio*>(ctx)->deliverObserver(frame, len, true);
}
// Main loop (UDP link).
void HeadLinkRadio::onUdpObserver(const uint8_t* frame, size_t len, void* ctx) {
  static_cast<HeadLinkRadio*>(ctx)->deliverObserver(frame, len, false);
}

void HeadLinkRadio::deliverObserver(const uint8_t* frame, size_t len, bool via_espnow) {
  const bool vitals = len > 5 && frame[5] == (uint8_t)mesh::observer::Kind::Vitals;
  bool duplicate = false;
  portENTER_CRITICAL(&_mux);
  // _cfg belongs to the main loop; the WiFi task reads only _rx_mode, under the lock.
  const hl::Mode mode = _rx_mode;
  if (via_espnow && mode == hl::Mode::Udp) {   // udp: ESP-NOW is not used
    portEXIT_CRITICAL(&_mux);
    return;
  }
  if (vitals) {
    const uint32_t now = millis();
    if (via_espnow) {
      _espnow_vitals_at = now; _espnow_vitals_seen = true;
      _auto.noteEspNowVitals(now);
    } else {
      _udp_vitals_at = now; _udp_vitals_seen = true;
    }
  }
  if (mode == hl::Mode::Both || mode == hl::Mode::Auto) duplicate = _dedupe.duplicate(frame, len);
  portEXIT_CRITICAL(&_mux);
  if (duplicate) return;
  const mesh::observer::FrameHandler handler = ui_handler.load(std::memory_order_acquire);
  if (handler) handler(frame, len, ui_ctx.load(std::memory_order_acquire));
}

int HeadLinkRadio::recvRaw(uint8_t* bytes, int sz) {
  ensureLoaded();
  int n = ESPNOWRadio::recvRaw(bytes, sz);
  if (n > 0 && _cfg.mode == hl::Mode::Udp) n = 0;      // udp: ignore ESP-NOW mesh traffic
  if (n > 0) return n;
  if (_link) {
    n = _link->recv(bytes, sz);
    if (n > 0) { ++n_recv; return n; }
  }
  return 0;
}

bool HeadLinkRadio::startSendRaw(const uint8_t* bytes, int len) {
  ensureLoaded();
  _tx_espnow = false;
  bool udp_ok = false;
  if (sendingUdp() && _link && len > 0) udp_ok = _link->send(bytes, (size_t)len);
  if (sendingEspNow()) {
    _tx_espnow = ESPNOWRadio::startSendRaw(bytes, len);
    return _tx_espnow || udp_ok;
  }
  if (udp_ok) ++n_sent;
  return udp_ok;
}

bool HeadLinkRadio::isSendComplete() {
  return _tx_espnow ? ESPNOWRadio::isSendComplete() : true;
}

void HeadLinkRadio::onSendFinished() {
  ESPNOWRadio::onSendFinished();
  _tx_espnow = false;
}

void HeadLinkRadio::loop() {
  ESPNOWRadio::loop();
  ensureLoaded();
  if (!_link) return;
  _link->loop();
  if (_cfg.mode == hl::Mode::Auto) {
    portENTER_CRITICAL(&_mux);
    _auto.update(millis(), (uint32_t)_cfg.failover_s * 1000U, _link->ready());
    portEXIT_CRITICAL(&_mux);
  }
}

#endif
