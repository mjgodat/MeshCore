#pragma once

#if defined(ESP32) && defined(WITH_UDP_BRIDGE)
#include "helpers/bridges/BridgeBase.h"
#include "helpers/bridges/UDPBridgeProtocol.h"
#include "helpers/UDPBridgePrefs.h"
#include "helpers/bridges/ObserverRateLimiter.h"
#include "helpers/bridges/DiscoveryFrame.h"

// All methods run on the mesh loop. No WiFi callbacks own packet memory.
// Network ownership belongs to the role; this class never starts/stops WiFi.
class UDPBridge : public BridgeBase {
public:
  UDPBridge(NodePrefs* prefs, mesh::PacketManager* mgr, mesh::RTCClock* rtc,
            const mesh::bridge::udp::Prefs* settings);
  ~UDPBridge() override;
  void begin() override;
  void end() override;
  void loop() override;
  void serviceNetwork(bool ready, uint32_t local_address = 0, uint32_t netmask = 0);
  void sendPacket(mesh::Packet* packet) override;
  void onPacketReceived(mesh::Packet* packet) override;
  void formatStatus(char* out, size_t capacity) const;
  void formatStats(const char* page, char* out, size_t capacity) const;
  bool sendTest(uint8_t peer_index, uint32_t nonce);
  void resetStats();
  // observer.transport udp|both: frames for the peers that subscribed (a
  // visual head sends Subscribe every 20 s; a subscription lapses after 60 s
  // or when the peer's session changes). Mesh frames always go first.
  // The outbox (~1 KB) is allocated on the first subscription, so a bridge
  // without visual heads keeps its 3072-byte object budget.
  bool ensureObserverOutbox();
  mesh::observer::Outbox& observerOutbox() { return *_observer; }  // after ensureObserverOutbox()
  bool observerMeshPending() const { return _tx_mask != 0; }
  uint8_t observerSubscribers() const;
  void discardObserverFrames() { if (_observer) _observer->clear(); }
  // Discovery (DiscoveryFrame.h). Announces go as plain datagrams to the
  // subnet broadcast address on udp.port and to every configured peer.
  // Received discovery frames (from any address) go to `sink`.
  bool sendDiscovery(const uint8_t* frame, size_t len);
  void setDiscoverySink(mesh::discovery::FrameSink sink, void* ctx) { _discovery_sink = sink; _discovery_ctx = ctx; }
  // Auto-paired heads occupy free peer slots at run time (never saved).
  // Returns the slot, -2 when a manual udp.peer.N already has the address,
  // -1 when stopped, full or the address is unusable.
  int addAutoPeer(const uint8_t ip[4], uint16_t port);
  void removeAutoPeer(const uint8_t ip[4], uint16_t port);
  uint8_t autoPeerCount() const;
  bool localAddress(uint8_t ip[4], uint16_t& port) const;

private:
  const mesh::bridge::udp::Prefs* _settings;
  mesh::bridge::udp::Prefs _active{};
  mesh::bridge::udp::Session _sessions[mesh::bridge::udp::MAX_PEERS]{};
  uint64_t _boot = 0;
  uint32_t _sequence = 0, _retry_at = 0;
  uint32_t _local_address = 0, _netmask = 0;
  int _socket = -1, _last_error = 0;
  bool _network_ready = false, _secret_valid = false;
  uint8_t _secret[16]{};
  uint8_t _secret_length = 0, _hello_cursor = 0;
  // One logical fan-out packet; each peer receives at most one send attempt.
  uint8_t _tx_payload[mesh::bridge::udp::MAX_PAYLOAD]{};
  uint16_t _tx_length = 0;
  uint8_t _tx_mask = 0;
  mesh::bridge::udp::Kind _tx_kind = mesh::bridge::udp::Kind::Mesh;
  uint32_t _tx_sequence = 0;
  // One verified receive slot survives packet-pool pressure; socket ingress
  // stops until it drains. No packet allocation for probes or invalid frames.
  uint8_t _rx_payload[mesh::bridge::udp::MAX_PAYLOAD]{};
  mesh::observer::Outbox* _observer = nullptr;
  uint32_t _observer_until[mesh::bridge::udp::MAX_PEERS]{};  // 0 = not subscribed
  uint32_t _observer_peer_sends = 0, _observer_peer_fails = 0;
  uint16_t _rx_length = 0;
  uint8_t _rx_peer = 0;
  uint8_t _auto_mask = 0;   // peers added by discovery, not from settings
  mesh::discovery::FrameSink _discovery_sink = nullptr;
  void* _discovery_ctx = nullptr;
  struct Stats {
    uint32_t frames = 0, authenticated = 0, delivered = 0;
    uint32_t malformed = 0, authentication = 0, foreign = 0, replay = 0;
    uint32_t filtered = 0, duplicate = 0, pool = 0, parse = 0;
    uint32_t requested = 0, queued = 0, sent = 0, failed = 0;
    uint32_t busy = 0, stopped = 0, oversize = 0, tx_filtered = 0, tx_duplicate = 0;
    uint32_t hellos = 0, challenges = 0, established = 0;
    uint32_t probe_queued = 0, probe_sent = 0, probe_failed = 0, probe_received = 0;
    uint32_t last_tx_nonce = 0, last_rx_nonce = 0;
  } _stats;
  bool _debug_dirty = true, _debug_was_enabled = false;
  uint32_t _debug_last_at = 0;

  static bool authenticate(void* context, const uint8_t* data, size_t size, uint8_t digest[32]);
  static uint64_t randomNonce();
  uint8_t peerMask(bool established_only) const;
  uint32_t nextSequence();
  bool sendFrame(uint8_t peer, mesh::bridge::udp::Kind kind, uint64_t recipient,
                 const uint8_t* payload, uint16_t length, uint32_t sequence = 0);
  void receiveFrame(uint8_t peer, const uint8_t* data, size_t length);
  void pumpReceive();
  void drainReceive();
  void pumpTransmit();
  void pumpHello();
  void pumpObserver();
  uint8_t observerMask() const;
  void emitDebug();
  void note(uint32_t& value);
  void error(int code);
  void onIngressResult(IngressResult result) override;
};
#endif
