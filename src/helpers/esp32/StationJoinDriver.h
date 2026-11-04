#pragma once

// ESP32 driver for helpers/StationJoin.h: Arduino WiFi calls that return at
// once (asynchronous scan, WiFi.begin's asynchronous start). Used by the
// repeater's UDP bridge station and the visual heads' UDP link.

#if defined(ESP32)
#include <string.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_event.h>
#include <esp_netif.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
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
  // The ESP-NOW bridge starts the WiFi driver through ESP-IDF directly
  // (esp_wifi_init/start, no network interface). Arduino's facade cannot see
  // that, so WiFi.mode(WIFI_STA) runs its first-time init: it creates the
  // default station netif, finds the driver already initialised and
  // started, and returns. The driver's WIFI_EVENT_STA_START was posted
  // before the netif existed, so the netif is never started: no lwIP input
  // hook, "dhcp client start failed" at association, and the first received
  // frame jumps through a null pointer in esp_netif_receive (the M5 panic
  // of 2026-11-03). The driver is never stopped or re-initialised here;
  // instead STA_START is replayed once for the late netif, exactly as the
  // driver would have announced it, before any join or DHCP.
  bool prepare() {
    WiFi.persistent(false);
    if (WiFi.getMode() != WIFI_STA) {
      wifi_mode_t sdk_mode = WIFI_MODE_NULL;
      const bool sdk_started_without_facade = WiFi.getMode() == WIFI_OFF
          && esp_wifi_get_mode(&sdk_mode) == ESP_OK && sdk_mode != WIFI_MODE_NULL;
      if (!WiFi.mode(WIFI_STA)) return false;
      if (sdk_started_without_facade && !startLateStationNetif()) return false;
    }
    if (applyProtocolMask(WIFI_IF_STA) != ESP_OK) return false;
    setStationAutoReconnect(true);
    return true;
  }

  static bool startLateStationNetif() {
    esp_netif_t* sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!sta) return false;
    if (esp_netif_is_netif_up(sta)) return true;
    if (esp_event_post(WIFI_EVENT, WIFI_EVENT_STA_START, nullptr, 0, pdMS_TO_TICKS(50)) != ESP_OK)
      return false;
    // The event task starts the netif; wait for it (once per boot, a few ms).
    for (int i = 0; i < 50 && !esp_netif_is_netif_up(sta); ++i) vTaskDelay(1);
    return esp_netif_is_netif_up(sta);
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
