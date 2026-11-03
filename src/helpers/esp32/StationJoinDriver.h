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
  bool (*after_begin)() = nullptr;   // heads: re-pin the ESP-NOW LR rate

  uint8_t espNowChannel() const { return espNowChannelConstrained() ? activeEspNowChannel() : 0; }
  bool mutationAllowed() const { return stationMutationAllowed(); }

  // Station mode with the shared protocol mask; auto-reconnect stays off
  // while ESP-NOW is constrained (only this machine rejoins, on-channel).
  bool prepare() {
    WiFi.persistent(false);
    if (WiFi.getMode() != WIFI_STA && !WiFi.mode(WIFI_STA)) return false;
    if (applyProtocolMask(WIFI_IF_STA) != ESP_OK) return false;
    setStationAutoReconnect(true);
    return true;
  }

  bool startScan(uint8_t channel, const char* ssid) {
    if (WiFi.scanComplete() == WIFI_SCAN_RUNNING || !prepare()) return false;
    if (channel && restoreEspNowChannel() != ESP_OK) return false;
    WiFi.scanDelete();
    // Asynchronous; 300 ms on one channel (it stays there), 110 ms per
    // channel for the heads' rare all-channel diagnosis.
    return WiFi.scanNetworks(true, false, false, channel ? 300 : 110, channel, ssid, nullptr)
        == WIFI_SCAN_RUNNING;
  }
  int scanResult() const {
    const int16_t n = WiFi.scanComplete();
    return n == WIFI_SCAN_RUNNING ? -1 : n < 0 ? -2 : n;
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
  void scanDelete() { WiFi.scanDelete(); }

  bool begin(const char* ssid, const char* password, uint8_t channel, const uint8_t* bssid) {
    if (!prepare()) return false;
    (void)WiFi.setSleep(power_save);
    applyStationPowerSave(power_save);
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
  void restoreChannel() { restoreEspNowChannel(); }
};

}}  // namespace mesh::wifi
#endif
