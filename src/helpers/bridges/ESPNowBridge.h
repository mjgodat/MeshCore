#pragma once

#include "MeshCore.h"
#include "esp_now.h"
#include "esp_idf_version.h"
#include <freertos/FreeRTOS.h>
#include "helpers/ESPNowRawFragmentation.h"
#include "helpers/esp32/EspNowRepeatScheduler.h"
#include "helpers/bridges/BridgeBase.h"
#include "helpers/bridges/ESPNowBridgeFormat.h"
#include "helpers/bridges/ESPNowWrappedFrame.h"
#include "helpers/bridges/DiscoveryFrame.h"
#include "helpers/bridges/ESPNowBridgeDiagnostics.h"
#include "helpers/bridges/ESPNowNodeTable.h"
#include "helpers/bridges/ObserverRateLimiter.h"
#include <atomic>

#ifdef WITH_ESPNOW_BRIDGE

/**
 * @brief Bridge implementation using ESP-NOW protocol for packet transport
 *
 * This bridge enables mesh packet transport over ESP-NOW, a connectionless communication
 * protocol provided by Espressif that allows ESP32 devices to communicate directly
 * without WiFi router infrastructure.
 *
 * Features:
 * - Broadcast-based communication (all bridges receive all packets)
 * - Selectable wrapped or raw MeshCore wire format
 * - Network isolation using XOR encryption with shared secret in wrapped mode
 * - Duplicate packet detection using SimpleMeshTables tracking
 * - Raw MeshCore packets up to 255 bytes using two frames only when needed
 *
 * Wrapped Packet Structure (the backward-compatible default):
 * [2 bytes] Magic Header - Used to identify ESPNowBridge packets
 * [2 bytes] Fletcher-16 checksum of the plaintext mesh payload
 * [246 bytes max] Mesh packet payload
 *
 * The Fletcher-16 checksum is used to validate packet integrity and detect
 * corrupted packets. It is calculated over the plaintext payload; the
 * checksum and payload are then XORed together using bridge.secret. A receiver
 * with the wrong secret will normally fail checksum validation.
 *
 * Raw mode sends the exact bytes produced by mesh::Packet::writeTo(). It is
 * compatible with primary ESPNOWRadio nodes, but has no bridge-secret wrapper.
 * Packets up to the ESP-NOW frame limit remain byte-for-byte unchanged;
 * 251-255-byte packets use a versioned two-frame envelope understood by
 * updated raw endpoints. Receive parsing is deliberately strict to the
 * selected format so a mixed deployment cannot silently become asymmetric.
 *
 * Configuration:
 * - Define WITH_ESPNOW_BRIDGE to enable this bridge
 * - Define _prefs->bridge_secret with a string to set the network encryption key
 * - Set _prefs->bridge_format to wrapped (default) or raw
 *
 * Network Isolation (wrapped mode only):
 * Multiple independent wrapped mesh networks can coexist by using different
 * _prefs->bridge_secret values. Packets encrypted with a different key will
 * fail the checksum validation and be discarded.
 */
class ESPNowBridge : public BridgeBase {
private:
  static ESPNowBridge *_instance;
#if ESP_IDF_VERSION_MAJOR >= 5
  static void recv_cb(const esp_now_recv_info_t *info, const uint8_t *data, int len);
#else
  static void recv_cb(const uint8_t *mac, const uint8_t *data, int len);
#endif
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 5, 0)
  static void send_cb(const esp_now_send_info_t *info,
                      esp_now_send_status_t status);
#else
  static void send_cb(const uint8_t *mac, esp_now_send_status_t status);
#endif

  /**
   * ESP-NOW Protocol Structure:
   * - ESP-NOW header: 20 bytes (handled by ESP-NOW protocol)
   * - ESP-NOW payload: 250 bytes maximum
   * Total ESP-NOW packet: 270 bytes
   *
   * Our Bridge Packet Structure (must fit in ESP-NOW payload):
   * - Magic header: 2 bytes
   * - Checksum: 2 bytes
   * - Available payload: 246 bytes
   */
  static const size_t MAX_ESPNOW_PACKET_SIZE = mesh::bridge::ESPNOW_MAX_FRAME_SIZE;
  static const uint8_t RX_QUEUE_DEPTH = 4;
  static const uint8_t TX_QUEUE_DEPTH = 6;

  /** Bounded callback-to-loop queue for receiving ESP-NOW packets. */
  uint8_t _rx_buffers[RX_QUEUE_DEPTH][MAX_ESPNOW_PACKET_SIZE];
  uint16_t _rx_lengths[RX_QUEUE_DEPTH];
  uint8_t _rx_sources[RX_QUEUE_DEPTH]
                     [mesh::espnow::ESPNOW_RAW_SOURCE_MAC_SIZE];

  // The ESP-NOW callback runs on the WiFi task. It only prefilters and copies
  // bounded frames here; loop() owns PacketManager and duplicate processing.
  portMUX_TYPE _rx_mux = portMUX_INITIALIZER_UNLOCKED;
  volatile uint8_t _rx_head;
  volatile uint8_t _rx_tail;
  volatile uint8_t _rx_count;
  volatile uint32_t _rx_dropped;
  uint32_t _rx_dropped_reported;

  // Keep logical packets intact in the queue. Espressif recommends waiting
  // for a frame's send callback before submitting the next one, and retaining
  // the packet boundary lets a failed first fragment discard its sibling
  // instead of transmitting an orphan fragment.
  struct QueuedTransmit {
    mesh::espnow::ESPNowRawFrames frames;
    SimpleMeshTables::SeenKey seen_key;
    uint8_t next_frame;
    bool started;
    uint8_t start_retries;
    bool diagnostic;
    // espnow.repeat: the delivered entry stays at the head, unchanged, while
    // it owes copies and nothing newer is queued. Copies are never retried.
    bool repeat_copy;
    bool repeatable;  // false for OTA firmware frames and diagnostics
  };
  static_assert(sizeof(QueuedTransmit) <= sizeof(mesh::espnow::ESPNowRawFrames) +
                    sizeof(SimpleMeshTables::SeenKey) + 8,
                "ESP-NOW TX must retain encoded frames without a second packet copy");
  QueuedTransmit _tx_queue[TX_QUEUE_DEPTH];
  portMUX_TYPE _tx_mux = portMUX_INITIALIZER_UNLOCKED;
  volatile uint8_t _tx_head;
  volatile uint8_t _tx_tail;
  volatile uint8_t _tx_count;
  volatile bool _tx_waiting;
  volatile bool _tx_callback_done;
  volatile int _tx_callback_status;
  volatile uint32_t _tx_dropped;
  uint32_t _tx_dropped_reported;
  uint32_t _tx_started_at = 0, _tx_retry_at = 0;
  bool _sdk_teardown_failed = false;
  uint8_t _previous_wifi_sleep = 0;
  bool _wifi_sleep_preference_owned = false;
  static constexpr uint32_t TX_CALLBACK_TIMEOUT_MS = 1000;
  static constexpr uint32_t TX_RETRY_DELAY_MS = 20;
  static constexpr uint8_t TX_MAX_START_RETRIES = 3;

  /** Owed espnow.repeat copies of the head entry; loop() context only. */
  mesh::espnow::EspNowRepeatScheduler _repeat;

  /**
   * espnow.observer frames, loop() context only. They never enter _tx_queue:
   * a frame starts only when no mesh entry is queued or on air, is never
   * retried or repeated, and bypasses the seen table. _observer_in_flight
   * (guarded by _tx_mux) is meaningful only while _tx_waiting is set; every
   * mesh frame start clears it.
   */
  mesh::observer::Outbox _observer;
  volatile bool _observer_in_flight = false;

  /**
   * One pending discovery announce (DiscoveryFrame.h), loop() context only.
   * Like observer frames it goes out only into an idle link, never queued
   * behind or retried; a newer announce replaces one not yet sent. It shares
   * _observer_in_flight; _aux_in_flight tells the two apart.
   */
  uint8_t _aux[mesh::discovery::MAX_FRAME];
  uint8_t _aux_len = 0;
  volatile bool _aux_in_flight = false;
  uint32_t _aux_sent = 0, _aux_failed = 0, _discovery_rx = 0;
  mesh::discovery::FrameSink _discovery_sink = nullptr;
  void* _discovery_ctx = nullptr;
  void deliverDiscovery(const uint8_t* frame, size_t len);

  /**
   * Modem sleep watch: while the bridge runs the radio stays awake
   * (WIFI_PS_NONE) whoever else owns the station. Checked every
   * kAwakeCheckMs from loop(); repairs count in _awake_repairs.
   */
  static constexpr uint32_t kAwakeCheckMs = 250;
  uint32_t _awake_check_at = 0;
  uint32_t _awake_repairs = 0;
  // A refused "stay awake" (e.g. Bluetooth needs modem sleep) backs off
  // 500 ms .. 10 s and is logged once per streak.
  uint32_t _awake_refusals = 0;
  uint32_t _awake_backoff_ms = 0;
  bool _awake_refusal_logged = false;
  void keepRadioAwake();

  /** Raw-mode fragment assemblies, keyed by source MAC. */
  mesh::espnow::ESPNowRawReassembler _raw_reassembler;
  /** Directly heard ESP-NOW stations; guarded by _rx_mux. */
  mesh::bridge::ESPNowNodeTable _nodes;

  // Snapshotted by begin() and immutable until end(), so callbacks and sends
  // never observe a partially applied preference change.
  uint8_t _active_format;
  uint8_t _active_peer[6] = {255, 255, 255, 255, 255, 255};
  mesh::bridge::diag::Stats _stats;
  std::atomic<bool> _debug_dirty{true};
  uint32_t _debug_last_at = 0;
  bool _debug_was_enabled = false;

  mesh::bridge::diag::Stats snapshotStats();
  void noteRx(uint32_t mesh::bridge::diag::RxStats::* field);
  void noteTx(uint32_t mesh::bridge::diag::TxStats::* field);
  void recordError(mesh::bridge::diag::Stage stage, int32_t code);
  void emitDebug();
  void onIngressResult(IngressResult result) override;
  bool admitIngress() override;

  /**
   * Wraps one payload (a MeshCore packet or an observer frame) in the
   * bridge.secret wrapper (ESPNowWrappedFrame.h, shared with primary
   * ESP-NOW nodes). Returns the frame length, 0 when it does not fit or the
   * secret is unusable (recorded as a Secret error).
   */
  size_t wrapFrame(const uint8_t* payload, size_t len, uint8_t* out, size_t cap);

  /** Parse and queue one serialized MeshCore packet. */
  void receiveMeshPacket(mesh::Packet* packet, const uint8_t *data, size_t len);

  /**
   * ESP-NOW receive callback
   * Called by ESP-NOW when a packet is received
   *
   * @param mac Source MAC address
   * @param data Received data
   * @param len Length of received data
   */
  void queueReceivedFrame(const uint8_t *mac, const uint8_t *data, int len);

  /** Parse a queued frame from loop(), never from the WiFi callback. */
  bool processReceivedFrame(mesh::Packet* packet, const uint8_t *mac,
                            const uint8_t *data, size_t len);

  /** Atomically append one logical packet's one or two transport frames. */
  bool queueTransmitFrames(const mesh::espnow::ESPNowRawFrames& frames,
                           const mesh::Packet& packet, bool diagnostic = false);

  /** Submit at most one queued frame after the previous send callback. */
  void pumpTransmitQueue();

  /**
   * ESP-NOW send callback
   * Called by ESP-NOW after a transmission attempt
   *
   * @param mac_addr Destination MAC address
   * @param status Transmission status
   */
  void onDataSent(const uint8_t *mac_addr, esp_now_send_status_t status);

public:
  /**
   * Constructs an ESPNowBridge instance
   *
   * @param prefs Node preferences for configuration settings
   * @param mgr PacketManager for allocating and queuing packets
   * @param rtc RTCClock for timestamping debug messages
   */
  ESPNowBridge(NodePrefs *prefs, mesh::PacketManager *mgr, mesh::RTCClock *rtc);

  /**
   * Initializes the ESP-NOW bridge
   *
   * - Configures WiFi in station mode
   * - Initializes ESP-NOW protocol
   * - Registers callbacks
   * - Sets up broadcast peer
   */
  void begin() override;

  /**
   * Stops the ESP-NOW bridge
   *
   * - Removes broadcast peer
   * - Unregisters callbacks
   * - Deinitializes ESP-NOW protocol
   * - Turns off WiFi to release radio resources
   */
  void end() override;

  /**
   * Main loop handler
   * Drains the bounded frames handed off by the WiFi receive callback.
   */
  void loop() override;

  /**
   * Called when a packet is received via ESP-NOW
   * Queues the packet for mesh processing if not seen before
   *
   * @param packet The received mesh packet
   */
  void onPacketReceived(mesh::Packet *packet) override;

  /**
   * Called when a packet needs to be transmitted via ESP-NOW
   * Encrypts and broadcasts the packet if not seen before
   *
   * @param packet The mesh packet to transmit
   */
  void sendPacket(mesh::Packet *packet) override;

  // Runtime diagnostics work in ordinary firmware, without BRIDGE_DEBUG.
  // All output is bounded and generated on the main loop, never a callback.
  void formatStatus(char* reply, size_t reply_size);
  void formatStats(const char* page, char* reply, size_t reply_size);
  // ESP-NOW stations heard directly in the last 15 minutes.
  uint8_t activeNodeCount();
  void formatNodes(char* reply, size_t reply_size);
  bool sendTest(uint32_t nonce);
  /**
   * Queues one encoded observer frame (mesh::observer) for broadcast, or for
   * the espnow.peer destination when one is configured. Returns false when the
   * bridge is stopped, the frame is malformed, a packet record is rate limited,
   * or mesh traffic is pending (mesh frames always win). Never blocks.
   */
  bool sendObserverFrame(const uint8_t* frame, size_t len);
  /**
   * Queues one discovery frame (announce) for the idle link; replaces one
   * not yet sent. Wrapped when the bridge format is wrapped. False when the
   * bridge is stopped or the frame is not a discovery frame.
   */
  bool sendAuxFrame(const uint8_t* frame, size_t len);
  /** Received discovery frames (replies) go to `sink` from loop(). */
  void setDiscoverySink(mesh::discovery::FrameSink sink, void* ctx) {
    _discovery_sink = sink;
    _discovery_ctx = ctx;
  }
  uint32_t auxSent() const { return _aux_sent; }
  uint32_t discoveryReceived() const { return _discovery_rx; }
  /** Drops queued observer frames (espnow.observer off; also done by end()). */
  void discardObserverFrames() { _observer.clear(); }
  /**
   * observer.transport fan-out (ObserverRateLimiter.h Fanout): the outbox the
   * shared rate limiter offers admitted frames to, and whether mesh traffic
   * is queued (packet records then yield). loop() context only.
   */
  mesh::observer::Outbox& observerOutbox() { return _observer; }
  bool observerMeshPending();
  void resetStats();
};

#endif
