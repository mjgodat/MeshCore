#include "ESPNOWRadio.h"
#include <esp_now.h>
#include <esp_idf_version.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <helpers/ESPNowRawFragmentation.h>
#include <helpers/bridges/ObserverDemux.h>
#include <helpers/esp32/EspNowRepeatScheduler.h>
#include <helpers/RadioDirectionPolicy.h>
#include <helpers/esp32/WiFiRadioPolicy.h>
#include <helpers/WirelessControl.h>
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  #include <esp_mac.h>
#endif

static uint8_t broadcastAddress[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
static esp_now_peer_info_t peerInfo;
static volatile bool is_send_complete = false;
static esp_err_t last_send_result;
static volatile esp_now_send_status_t last_send_status =
    ESP_NOW_SEND_SUCCESS;
static bool tx_second_pending = false;
static uint16_t tx_second_length = 0;
static uint8_t tx_second_frame[mesh::espnow::ESPNOW_RAW_MAX_FRAME_SIZE];
// espnow.repeat: one retained logical packet, re-encoded for each copy.
// Encoding is deterministic, so every copy (and fragment) is byte-identical.
// A new packet replaces it and cancels any copies still owed.
static mesh::espnow::EspNowRepeatScheduler tx_repeat;
static uint8_t tx_repeat_packet[mesh::espnow::ESPNOW_RAW_MAX_PACKET_SIZE];
static uint16_t tx_repeat_length = 0;
static bool tx_repeat_in_flight = false;  // a copy's frame awaits its callback
static uint32_t tx_repeat_frame_at = 0;   // when that copy frame was submitted
#ifdef PAYLOAD_TYPE_OTA
static_assert(PAYLOAD_TYPE_OTA == mesh::espnow::kRepeatOtaPayloadType,
              "espnow.repeat must recognise OTA payloads");
#endif
static constexpr uint16_t RX_FRAME_MAX =
    mesh::espnow::ESPNOW_RAW_MAX_FRAME_SIZE;
static constexpr uint8_t RX_QUEUE_DEPTH = 4;
static uint8_t rx_frames[RX_QUEUE_DEPTH][RX_FRAME_MAX];
static uint16_t rx_lengths[RX_QUEUE_DEPTH];
static uint8_t rx_sources[RX_QUEUE_DEPTH]
                         [mesh::espnow::ESPNOW_RAW_SOURCE_MAC_SIZE];
static volatile uint8_t rx_head = 0;
static volatile uint8_t rx_tail = 0;
static volatile uint8_t rx_count = 0;
static volatile uint32_t rx_dropped = 0;
static uint32_t rx_dropped_reported = 0;
static portMUX_TYPE rx_mux = portMUX_INITIALIZER_UNLOCKED;
static mesh::espnow::ESPNowRawReassembler rx_reassembler;
static std::atomic<bool> accepting_packets{false};
// Visual head observer feed (setObserverHandler). The context is published
// before the handler (release) and read after it (acquire).
static std::atomic<mesh::observer::FrameHandler> observer_handler{nullptr};
static std::atomic<void*> observer_ctx{nullptr};

void ESPNOWRadio::setObserverHandler(mesh::observer::FrameHandler handler,
                                     void* ctx) {
  observer_ctx.store(ctx, std::memory_order_release);
  observer_handler.store(handler, std::memory_order_release);
}

bool ESPNOWRadio::forceLongRangeRate() {
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 2, 0)
  esp_now_rate_config_t rate_config = {};
  rate_config.phymode = WIFI_PHY_MODE_LR;
  rate_config.rate = WIFI_PHY_RATE_LORA_250K;
  rate_config.ersu = false;
  rate_config.dcm = false;
  return esp_now_set_peer_rate_config(broadcastAddress, &rate_config) == ESP_OK;
#else
  return esp_wifi_config_espnow_rate(WIFI_IF_STA, WIFI_PHY_RATE_LORA_250K) == ESP_OK;
#endif
}

// callback when data is sent
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 5, 0)
static void OnDataSent(const esp_now_send_info_t *info,
                       esp_now_send_status_t status) {
  (void)info;
#else
static void OnDataSent(const uint8_t *mac_addr, esp_now_send_status_t status) {
  (void)mac_addr;
#endif
  if (!accepting_packets.load()) return;
  last_send_status = status;
  is_send_complete = true;
  ESPNOW_DEBUG_PRINTLN("Send Status: %d", (int)status);
}

#if ESP_ARDUINO_VERSION_MAJOR >= 3
static void OnDataRecv(const esp_now_recv_info_t *info, const uint8_t *data,
                       int len) {
  const uint8_t *mac = info != nullptr ? info->src_addr : nullptr;
#else
static void OnDataRecv(const uint8_t *mac, const uint8_t *data, int len) {
#endif
  if (!accepting_packets.load()) return;
  ESPNOW_DEBUG_PRINTLN("Recv: len = %d", len);
  // Observer frames go to the visual head; MeshCore/WiChat never see them.
  // Without a handler every frame takes the path below, as before.
  if (data != nullptr && len > 0
      && mesh::observer::demuxObserverFrame(
          data, static_cast<size_t>(len),
          observer_handler.load(std::memory_order_acquire),
          observer_ctx.load(std::memory_order_acquire))) {
    return;
  }
  portENTER_CRITICAL(&rx_mux);
  if (data != nullptr && len > 0 && len <= RX_FRAME_MAX
      && rx_count < RX_QUEUE_DEPTH) {
    memcpy(rx_frames[rx_head], data, static_cast<size_t>(len));
    rx_lengths[rx_head] = static_cast<uint16_t>(len);
    if (mac != nullptr) {
      memcpy(rx_sources[rx_head], mac,
             mesh::espnow::ESPNOW_RAW_SOURCE_MAC_SIZE);
    } else {
      memset(rx_sources[rx_head], 0,
             mesh::espnow::ESPNOW_RAW_SOURCE_MAC_SIZE);
    }
    rx_head = static_cast<uint8_t>((rx_head + 1) % RX_QUEUE_DEPTH);
    rx_count++;
  } else {
    rx_dropped++;
  }
  portEXIT_CRITICAL(&rx_mux);
}

void ESPNOWRadio::init() {
  if (initialized_ || mesh::wireless::control().blocked(mesh::wireless::EspNow)) return;
  // A conventional AP and the LR PHY share one enable flag on IDF 4.4. Do not
  // hide the AP or claim a successful new LR start while AP requires B/G/N.
  if (mesh::wifi::checkLongRangeRadioStart() != ESP_OK) {
    ESPNOW_DEBUG_PRINTLN("ESP-NOW LR start deferred while a WiFi AP is active");
    return;
  }
  portENTER_CRITICAL(&rx_mux);
  rx_head = 0;
  rx_tail = 0;
  rx_count = 0;
  rx_dropped = 0;
  portEXIT_CRITICAL(&rx_mux);
  rx_dropped_reported = 0;
  rx_reassembler.reset();
  tx_second_pending = false;
  tx_second_length = 0;
  tx_repeat.cancel();
  tx_repeat_in_flight = false;
  last_send_status = ESP_NOW_SEND_SUCCESS;

  // Set device as a Wi-Fi Station
  // ESP-NOW's LR protocol is local to this radio transport. Keeping WiFi
  // driver writes in RAM prevents a later conventional WiFi image from
  // inheriting the proprietary protocol bit through NVS.
  WiFi.persistent(false);
  const wifi_mode_t existing_mode = WiFi.getMode();
#if defined(MESH_PRIMARY_ESPNOW) && MESH_PRIMARY_ESPNOW
  // Arduino defaults station auto-reconnect to enabled. Disable it before the
  // driver starts so credentials left by another image cannot pull the radio
  // away from the configured mesh channel behind this policy's back.
  if (existing_mode == WIFI_OFF) WiFi.setAutoReconnect(false);
#endif
  // Restarting ESP-NOW must retain a live infrastructure setup AP.
  if (!WiFi.mode((existing_mode & WIFI_AP) ? WIFI_AP_STA : WIFI_STA)) return;
#if defined(MESH_ESPNOW_RADIO) && MESH_ESPNOW_RADIO
  // Full Companion also exposes ordinary WiFi. Preserve B/G/N for normal
  // access points while retaining LR for ESP-NOW, and pin both transports to
  // one channel because an ESP32 cannot associate and send ESP-NOW on two
  // different channels simultaneously.
  if (mesh::wifi::applyStationProtocolMask(mesh::wifi::kProtocolMask, true) != ESP_OK) {
    ESPNOW_DEBUG_PRINTLN("Error configuring ESP-NOW/WiFi coexistence");
    return;
  }
#else
  // Dedicated ESP-NOW targets retain the original LR-only behavior.
  if (mesh::wifi::applyStationProtocolMask(WIFI_PROTOCOL_LR, true) != ESP_OK) {
    ESPNOW_DEBUG_PRINTLN("Error configuring ESP-NOW LR protocol");
    return;
  }
#endif
#if defined(MESH_PRIMARY_ESPNOW) && MESH_PRIMARY_ESPNOW
  if (mesh::wifi::restoreEspNowChannel() != ESP_OK) {
    ESPNOW_DEBUG_PRINTLN("Error setting primary ESP-NOW channel");
    return;
  }
#endif

  // Init ESP-NOW
  if (esp_now_init() != ESP_OK) {
    ESPNOW_DEBUG_PRINTLN("Error initializing ESP-NOW");
    return;
  }
  initialized_ = true;

#if defined(MESH_PRIMARY_ESPNOW) && MESH_PRIMARY_ESPNOW
  // Modem sleep is still required when BLE and infrastructure WiFi coexist,
  // but unicast/broadcast ESP-NOW frames are not buffered by an access point.
  // Hold the driver's documented wake reference for the lifetime of this
  // primary mesh radio so receive stays continuous without disabling the
  // WiFi/Bluetooth coexistence policy globally.
  if (esp_wifi_force_wakeup_acquire() != ESP_OK) {
    ESPNOW_DEBUG_PRINTLN("Error keeping primary ESP-NOW receive awake");
    end();
    return;
  }
  wake_held_ = true;
#endif

  if (!setTxPower(tx_power_dbm_)) {
    end();
    return;
  }

  if (esp_now_register_send_cb(OnDataSent) != ESP_OK
      || esp_now_register_recv_cb(OnDataRecv) != ESP_OK) {
    end();
    return;
  }

  // Register peer
  memset(&peerInfo, 0, sizeof(peerInfo));
  memcpy(peerInfo.peer_addr, broadcastAddress, 6);
#if defined(MESH_PRIMARY_ESPNOW) && MESH_PRIMARY_ESPNOW
  // Zero follows the station/AP's current channel. The policy pins that radio
  // channel and rejects off-channel infrastructure associations, avoiding a
  // stale fixed peer entry after a configured-channel reboot.
#endif
  peerInfo.channel = 0;
  peerInfo.ifidx = WIFI_IF_STA;
  peerInfo.encrypt = false;

  is_send_complete = true;

  // Add peer        
  if (esp_now_add_peer(&peerInfo) == ESP_OK) {
#if defined(MESH_ESPNOW_RADIO) && MESH_ESPNOW_RADIO
    // Enabling interoperable B/G/N alongside LR changes the interface's
    // default rate selection. Force ESP-NOW packets back to the legacy LR rate
    // so Full images remain compatible with existing LR-only mesh nodes.
  #if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 2, 0)
    esp_now_rate_config_t rate_config = {};
    rate_config.phymode = WIFI_PHY_MODE_LR;
    rate_config.rate = WIFI_PHY_RATE_LORA_250K;
    rate_config.ersu = false;
    rate_config.dcm = false;
    if (esp_now_set_peer_rate_config(broadcastAddress, &rate_config) != ESP_OK) {
      ESPNOW_DEBUG_PRINTLN("Error configuring ESP-NOW LR peer rate");
      end();
      return;
    }
  #else
    if (esp_wifi_config_espnow_rate(WIFI_IF_STA, WIFI_PHY_RATE_LORA_250K)
        != ESP_OK) {
      ESPNOW_DEBUG_PRINTLN("Error configuring ESP-NOW LR interface rate");
      end();
      return;
    }
  #endif
#endif
    accepting_packets.store(true);
    mesh::wifi::setLongRangeOwner(mesh::wifi::kLongRangeRadioOwner, true);
    ESPNOW_DEBUG_PRINTLN("init success");
  } else {
    end();
  }
}

void ESPNOWRadio::end() {
  if (send_active_) send_cancelled_ = true;
  accepting_packets.store(false);
  mesh::wifi::setLongRangeOwner(mesh::wifi::kLongRangeRadioOwner, false);
  if (initialized_) {
    esp_now_unregister_send_cb();
    esp_now_unregister_recv_cb();
    esp_now_deinit();
    initialized_ = false;
  }
#if defined(MESH_PRIMARY_ESPNOW) && MESH_PRIMARY_ESPNOW
  if (wake_held_) {
    esp_wifi_force_wakeup_release();
    wake_held_ = false;
  }
#endif
  portENTER_CRITICAL(&rx_mux);
  rx_head = rx_tail = rx_count = 0;
  portEXIT_CRITICAL(&rx_mux);
  rx_reassembler.reset();
  tx_second_pending = false;
  tx_second_length = 0;
  tx_repeat.cancel();
  tx_repeat_in_flight = false;
  // A cancelled transmission must time out, never become a false TX success.
  is_send_complete = false;
}

uint32_t ESPNOWRadio::getRngSeed() {
  return millis() + intID();  // TODO: where to get some entropy?
}

bool ESPNOWRadio::setTxPower(int8_t dbm) {
  if (esp_wifi_set_max_tx_power(dbm * 4) != ESP_OK) return false;
  tx_power_dbm_ = dbm;
  return true;
}

uint32_t ESPNOWRadio::intID() {
  uint8_t mac[8];
  memset(mac, 0, sizeof(mac));
  esp_efuse_mac_get_default(mac);
  uint32_t n, m;
  memcpy(&n, &mac[0], 4);
  memcpy(&m, &mac[4], 4);
  
  return n + m;
}

// Stage and submit the retained packet's first frame for an espnow.repeat
// copy. Its second fragment, if any, follows its callback from loop().
static bool submitRetainedPacket() {
  mesh::espnow::ESPNowRawFrames frames;
  if (!mesh::espnow::encodeEspNowRawFrames(tx_repeat_packet, tx_repeat_length, frames)) {
    return false;
  }
  tx_second_pending = frames.count == 2;
  tx_second_length = tx_second_pending ? frames.lengths[1] : 0;
  if (tx_second_pending) memcpy(tx_second_frame, frames.data[1], tx_second_length);
  last_send_status = ESP_NOW_SEND_SUCCESS;
  is_send_complete = false;
  const esp_err_t result = esp_now_send(
      broadcastAddress, frames.data[0], frames.lengths[0]);
  if (result == ESP_OK) return true;
  last_send_result = result;
  tx_second_pending = false;
  tx_second_length = 0;
  return false;
}

bool ESPNOWRadio::startSendRaw(const uint8_t* bytes, int len) {
  if (!initialized_ || send_cancelled_) return false;
  mesh::espnow::ESPNowRawFrames frames;
  if (len < 0
      || !mesh::espnow::encodeEspNowRawFrames(
          bytes, static_cast<size_t>(len), frames)) {
    ESPNOW_DEBUG_PRINTLN("Invalid TX length: %d", len);
    return false;
  }

  // ESP-NOW completions carry no frame identity, so never start while an
  // espnow.repeat copy is on air. Dispatcher asks holdTransmitStart() first;
  // this only guards other callers, which then use their normal retry path.
  if (tx_repeat_in_flight) return false;
  // Fresh traffic wins over copies still owed for the previous packet. With
  // repeat=1 nothing is retained and the send below is unchanged.
  const mesh::espnow::EspNowRepeatConfig& repeat = mesh::espnow::espNowRepeatConfig();
  const uint8_t copies = mesh::espnow::isRepeatablePacket(bytes, static_cast<size_t>(len))
      ? repeat.count : 1;
  tx_repeat.cancel();
  if (copies > 1) {
    memcpy(tx_repeat_packet, bytes, static_cast<size_t>(len));
    tx_repeat_length = static_cast<uint16_t>(len);
  }

  tx_second_pending = frames.count == 2;
  tx_second_length = 0;
  if (tx_second_pending) {
    tx_second_length = frames.lengths[1];
    memcpy(tx_second_frame, frames.data[1], tx_second_length);
  }

  // Send the first (or only) message via ESP-NOW. isSendComplete() submits a
  // second fragment only after this frame's callback, preserving Espressif's
  // required callback ordering without blocking the mesh dispatcher.
  last_send_status = ESP_NOW_SEND_SUCCESS;
  is_send_complete = false;
  esp_err_t result = esp_now_send(
      broadcastAddress, frames.data[0], frames.lengths[0]);
  if (result == ESP_OK) {
    send_active_ = true;
    n_sent++;
    tx_repeat.begin(millis(), copies, repeat.gap_ms);
    ESPNOW_DEBUG_PRINTLN("Send started: frames=%u raw_len=%d",
                         (unsigned)frames.count, len);
    return true;
  }
  last_send_result = result;
  tx_second_pending = false;
  tx_second_length = 0;
  is_send_complete = true;
  ESPNOW_DEBUG_PRINTLN("Send failed: %d", result);
  return false;
}

bool ESPNOWRadio::isSendComplete() {
  if (!initialized_ || send_cancelled_) return false;
  if (!is_send_complete) return false;
  if (last_send_status != ESP_NOW_SEND_SUCCESS) {
    // A failed callback means the logical packet was not transmitted. Do not
    // send an orphan second fragment; remaining incomplete lets Dispatcher use
    // its bounded timeout/retry path.
    tx_second_pending = false;
    tx_second_length = 0;
    is_send_complete = false;
    ESPNOW_DEBUG_PRINTLN("Send callback failed: %d", (int)last_send_status);
    return false;
  }
  if (tx_second_pending) {
    tx_second_pending = false;
    last_send_status = ESP_NOW_SEND_SUCCESS;
    is_send_complete = false;
    const esp_err_t result = esp_now_send(
        broadcastAddress, tx_second_frame, tx_second_length);
    if (result != ESP_OK) {
      // Leave the operation incomplete. Dispatcher will use its bounded radio
      // timeout/retry path instead of reporting a partially sent packet as a
      // success.
      last_send_result = result;
      tx_second_length = 0;
      ESPNOW_DEBUG_PRINTLN("Second fragment failed to start: %d", result);
      return false;
    }
    tx_second_length = 0;
    ESPNOW_DEBUG_PRINTLN("Second fragment started");
    return false;
  }
  tx_repeat.finished();
  return is_send_complete;
}
void ESPNOWRadio::onSendFinished() {
  // Copies follow only a packet whose send completed successfully.
  tx_repeat.abortIfUnfinished();
  send_active_ = false;
  send_cancelled_ = false;
  tx_second_pending = false;
  tx_second_length = 0;
  last_send_status = ESP_NOW_SEND_SUCCESS;
  is_send_complete = true;
}

bool ESPNOWRadio::isInRecvMode() const {
  return initialized_ && !send_cancelled_ && is_send_complete && !tx_second_pending;
}

// Forget an in-flight copy (lost callback or radio recovery). Owed copies are
// cancelled; the radio is idle again for Dispatcher.
static void abandonRepeatCopy() {
  tx_repeat.cancel();
  if (!tx_repeat_in_flight) return;
  tx_repeat_in_flight = false;
  tx_second_pending = false;
  tx_second_length = 0;
  last_send_status = ESP_NOW_SEND_SUCCESS;
  is_send_complete = true;
}

static bool repeatCopyTimedOut() {
  if (!tx_repeat_in_flight || is_send_complete
      || static_cast<uint32_t>(millis() - tx_repeat_frame_at)
          < mesh::espnow::kRepeatCopyCallbackTimeoutMs) {
    return false;
  }
  mesh::espnow::espNowRepeatConfig().radio_copy_timeouts++;
  ESPNOW_DEBUG_PRINTLN("Repeat copy callback timed out");
  abandonRepeatCopy();
  return true;
}

bool ESPNOWRadio::holdTransmitStart() {
  if (!tx_repeat_in_flight || repeatCopyTimedOut()) return false;
  tx_repeat.cancel();  // fresh traffic is waiting: owe no further copies
  return true;
}

bool ESPNOWRadio::recoverRadio(bool hard) {
  (void)hard;
  abandonRepeatCopy();
  return true;
}

void ESPNOWRadio::loop() {
  // Dispatcher owns the radio while its own packet is being sent.
  if (!initialized_ || send_cancelled_ || send_active_) return;
  if (tx_repeat_in_flight) {
    if (repeatCopyTimedOut() || !is_send_complete) return;
    if (tx_second_pending && last_send_status == ESP_NOW_SEND_SUCCESS) {
      tx_second_pending = false;
      is_send_complete = false;
      if (esp_now_send(broadcastAddress, tx_second_frame, tx_second_length) == ESP_OK) {
        tx_second_length = 0;
        tx_repeat_frame_at = millis();
        return;
      }
      is_send_complete = true;  // abandon this copy; the next one is whole
    }
    tx_second_pending = false;
    tx_second_length = 0;
    last_send_status = ESP_NOW_SEND_SUCCESS;
    tx_repeat_in_flight = false;
    tx_repeat.finished();
    return;
  }
  // Copies follow Dispatcher's TX policy: none while transmit is disabled.
  if (tx_repeat.pending() && !mesh::radioDirection::radioTransmitAllowed()) {
    tx_repeat.cancel();
    return;
  }
  const uint32_t now = millis();
  if (!tx_repeat.due(now, is_send_complete && !tx_second_pending)) return;
  if (!submitRetainedPacket()) {
    tx_repeat.cancel();
    is_send_complete = true;
    ESPNOW_DEBUG_PRINTLN("Repeat copy failed to start: %d", last_send_result);
    return;
  }
  tx_repeat.copyStarted(now);
  tx_repeat_in_flight = true;
  tx_repeat_frame_at = now;
  mesh::espnow::espNowRepeatConfig().radio_copies++;
}

float ESPNOWRadio::getLastRSSI() const { return 0; }
float ESPNOWRadio::getLastSNR() const { return 0; }

int ESPNOWRadio::recvRaw(uint8_t* bytes, int sz) {
  if (!initialized_ || bytes == nullptr || sz <= 0) return 0;

  uint32_t dropped = 0;
  portENTER_CRITICAL(&rx_mux);
  dropped = rx_dropped;
  portEXIT_CRITICAL(&rx_mux);

  if (dropped != rx_dropped_reported) {
    n_recv_errors += dropped - rx_dropped_reported;
    ESPNOW_DEBUG_PRINTLN("RX queue/length drops: %lu",
                         (unsigned long)dropped);
    rx_dropped_reported = dropped;
  }

  // Drain fragment-only and malformed frames in one dispatcher poll. This
  // lets a queued two-frame packet become one ordinary MeshCore receive while
  // retaining the callback's fixed, bounded handoff queue.
  for (uint8_t processed = 0; processed < RX_QUEUE_DEPTH; ++processed) {
    uint8_t frame[RX_FRAME_MAX];
    uint8_t source[mesh::espnow::ESPNOW_RAW_SOURCE_MAC_SIZE];
    uint16_t frame_len = 0;

    portENTER_CRITICAL(&rx_mux);
    if (rx_count > 0) {
      frame_len = rx_lengths[rx_tail];
      if (frame_len > 0 && frame_len <= sizeof(frame)) {
        memcpy(frame, rx_frames[rx_tail], frame_len);
        memcpy(source, rx_sources[rx_tail], sizeof(source));
      } else {
        frame_len = 0;
      }
      rx_tail = static_cast<uint8_t>((rx_tail + 1) % RX_QUEUE_DEPTH);
      rx_count--;
    }
    portEXIT_CRITICAL(&rx_mux);

    if (frame_len == 0) break;
    size_t packet_len = 0;
    const mesh::espnow::ESPNowRawReassemblyResult result =
        rx_reassembler.acceptFrame(
            source, frame, frame_len, millis(), bytes,
            static_cast<size_t>(sz), packet_len);
    if (result == mesh::espnow::ESPNowRawReassemblyResult::PASSTHROUGH
        || result ==
            mesh::espnow::ESPNowRawReassemblyResult::PACKET_COMPLETE) {
      n_recv++;
      return static_cast<int>(packet_len);
    }
    if (result == mesh::espnow::ESPNowRawReassemblyResult::REJECTED
        || result ==
            mesh::espnow::ESPNowRawReassemblyResult::OUTPUT_TOO_SMALL) {
      n_recv_errors++;
      ESPNOW_DEBUG_PRINTLN("RX fragment rejected: result=%u len=%u size=%d",
                           (unsigned)result, (unsigned)frame_len, sz);
    }
  }
  return 0;
}

uint32_t ESPNOWRadio::getEstAirtimeFor(int len_bytes) {
  if (len_bytes > static_cast<int>(
          mesh::espnow::ESPNOW_RAW_MAX_FRAME_SIZE)) {
    // A maximum raw packet becomes one full ESP-NOW LR frame plus one short
    // frame. Include callback scheduling margin for both transmissions.
    return 40;
  }
  // At the forced 250-kbit/s LR rate, a 250-byte payload alone takes 8 ms.
  // Dispatcher applies a 1.5x deadline multiplier, so 20 ms leaves room for
  // ESP-NOW/MAC overhead and callback scheduling without false timeouts.
  return 20;
}
