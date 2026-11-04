#pragma once

// ESP32 driver for helpers/StationJoin.h: Arduino WiFi calls that return at
// once (asynchronous scan, WiFi.begin's asynchronous start). Used by the
// repeater's UDP bridge station and the visual heads' UDP link.

#if defined(ESP32)
#include <string.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <helpers/StationJoin.h>
#include <helpers/esp32/WiFiRadioPolicy.h>
#include <helpers/esp32/WiFiStationPolicy.h>
#include <helpers/esp32/WiFiStationPower.h>

namespace mesh { namespace wifi {

struct StationJoinDriver {
  wifi_ps_type_t power_save = WIFI_PS_NONE;
  // Repeater: the configured bridge channel while the ESP-NOW bridge is
  // enabled, even when it is momentarily stopped (a channel change), so the
  // station never joins elsewhere in that window. 0 = use the live policy.
  uint8_t pinned_channel = 0;
  bool (*after_begin)() = nullptr;   // heads: re-pin the ESP-NOW LR rate

  uint8_t espNowChannel() const {
    if (pinned_channel) return pinned_channel;
    return espNowChannelConstrained() ? activeEspNowChannel() : 0;
  }
  bool mutationAllowed() const { return stationMutationAllowed(); }

  // Station mode with the shared protocol mask; auto-reconnect stays off
  // while ESP-NOW is constrained (only this machine rejoins, on-channel).
  //
  // The driver must have been started through Arduino's facade (the heads'
  // ESPNOWRadio and, in UDP builds, the ESP-NOW bridge do): then the
  // station netif existed before STA_START and is started. A driver started
  // through ESP-IDF alone is refused here instead of letting WiFi.mode()
  // create a netif after STA_START, which is never started (no lwIP input;
  // "dhcp client start failed" and the 2026-11-03 panic in
  // esp_netif_receive). Never stops, re-initialises or restarts the radio.
  bool prepare() {
    WiFi.persistent(false);
    if (WiFi.getMode() != WIFI_STA) {
      if (sdkStartedWithoutFacade()) return false;   // see above: no late netif
      if (!WiFi.mode(WIFI_STA)) return false;
    }
    if (applyProtocolMask(WIFI_IF_STA) != ESP_OK) return false;
    setStationAutoReconnect(true);
    return true;
  }

  static bool sdkStartedWithoutFacade() {
    wifi_mode_t sdk_mode = WIFI_MODE_NULL;
    return WiFi.getMode() == WIFI_OFF && esp_wifi_get_mode(&sdk_mode) == ESP_OK
        && sdk_mode != WIFI_MODE_NULL;
  }

  // Arduino 2.0.17 reports an asynchronous scan as failed once 20 x its
  // per-channel time has passed (2.2 s for the all-channel diagnosis),
  // while the driver keeps scanning and stores the result later. Our own
  // scan counts as running until it lands or kScanLimitMs has passed.
  static const uint32_t kScanLimitMs = 10000;
  uint32_t scan_started_ms = 0;
  bool scan_in_flight = false;

  bool startScan(uint8_t channel, const char* ssid) {
    if (scanResult() == -1 || !prepare()) return false;
    if (channel && restoreEspNowChannel() != ESP_OK) return false;
    WiFi.scanDelete();
    // Asynchronous; 300 ms on one channel (it stays there), 110 ms per
    // channel for the heads' rare all-channel diagnosis.
    if (WiFi.scanNetworks(true, false, false, channel ? 300 : 110, channel, ssid, nullptr)
        != WIFI_SCAN_RUNNING) return false;
    scan_started_ms = millis();
    scan_in_flight = true;
    return true;
  }
  int scanResult() {
    const int16_t n = WiFi.scanComplete();
    if (n >= 0) { scan_in_flight = false; return n; }
    if (n == WIFI_SCAN_RUNNING) return -1;
    if (scan_in_flight && millis() - scan_started_ms < kScanLimitMs) return -1;   // Arduino gave up early
    scan_in_flight = false;
    return -2;
  }
  bool pick(const char* ssid, uint8_t channel, uint8_t bssid[6], uint8_t& strongest) {
    const int16_t n = WiFi.scanComplete();
    int best = -1, best_any = -1;
    int32_t best_rssi = INT32_MIN, best_any_rssi = INT32_MIN;
    for (int16_t i = 0; i < n; ++i) {
      if (WiFi.SSID(i) != ssid) continue;
      const int32_t rssi = WiFi.RSSI(i);
      if (rssi > best_any_rssi) { best_any_rssi = rssi; best_any = i; }
      if (channel && WiFi.channel(i) != channel) continue;
      if (rssi > best_rssi) { best_rssi = rssi; best = i; }
    }
    strongest = best_any >= 0 ? (uint8_t)WiFi.channel(best_any) : 0;
    if (best < 0) return false;
    const uint8_t* b = WiFi.BSSID(best);
    if (!b) return false;
    memcpy(bssid, b, 6);
    return true;
  }
  void scanDelete() { WiFi.scanDelete(); scan_in_flight = false; }

  bool begin(const char* ssid, const char* password, uint8_t channel, const uint8_t* bssid) {
    if (!prepare()) return false;
    const wifi_ps_type_t mode = pinned_channel || bridgeEspNowChannel().load() != 0 ? WIFI_PS_NONE : power_save;
    (void)WiFi.setSleep(mode);
    applyStationPowerSave(mode);
    beginStationDiagnostics(ssid);
    const wl_status_t r = beginStationWithSecurity(ssid, password, channel, bssid);
    if (after_begin) after_begin();
    return r != WL_CONNECT_FAILED;
  }

  bool linkUp() const {
    return WiFi.getMode() == WIFI_STA && WiFi.status() == WL_CONNECTED
        && static_cast<uint32_t>(WiFi.localIP()) != 0;
  }
  uint8_t linkChannel() const { return (uint8_t)WiFi.channel(); }
  void disconnect() { WiFi.disconnect(false, false); }
  void restoreChannel() {
    if (!pinned_channel) { restoreEspNowChannel(); return; }
    uint8_t current = 0;
    wifi_second_chan_t secondary = WIFI_SECOND_CHAN_NONE;
    if (esp_wifi_get_channel(&current, &secondary) == ESP_OK && current == pinned_channel) return;
    (void)esp_wifi_set_channel(pinned_channel, WIFI_SECOND_CHAN_NONE);
  }
};

}}  // namespace mesh::wifi
#endif
