#pragma once

#include <WiFi.h>
#include <esp_wifi.h>
#if defined(WITH_ESPNOW_BRIDGE)
#include <helpers/esp32/WiFiRadioPolicy.h>
#endif

namespace mesh {
namespace wifi {

inline wifi_power_t stationTransmitPower(wifi_ps_type_t mode) {
  if (mode != WIFI_PS_NONE) {
#ifdef MQTT_WIFI_POWER_SAVE_TX_POWER
    return MQTT_WIFI_POWER_SAVE_TX_POWER;
#else
    return WIFI_POWER_11dBm;
#endif
  }
#ifdef MQTT_WIFI_TX_POWER
  return MQTT_WIFI_TX_POWER;
#else
  return WIFI_POWER_19_5dBm;
#endif
}

// Restore normal transmit power when modem sleep is disabled, including after
// a live settings change or reconnect. The SDK enforces its radio limits.
//
// While an ESP-NOW bridge runs, modem sleep would make the radio deaf to
// ESP-NOW between beacons: every station owner (MQTT, WebConfig, the UDP
// station, ...) goes through here, so the request is replaced by
// WIFI_PS_NONE whatever order WiFi and the bridge started in, and the
// bridge keeps the TX power it set.
inline esp_err_t applyStationPowerSave(wifi_ps_type_t mode) {
#if defined(WITH_ESPNOW_BRIDGE)
  noteOwnerPowerSave(static_cast<uint8_t>(mode));   // handed back when the bridge stops
  if (bridgeEspNowChannel().load() != 0) return esp_wifi_set_ps(WIFI_PS_NONE);
#endif
  const esp_err_t result = esp_wifi_set_ps(mode);
  if (result != ESP_OK) return result;
  return esp_wifi_set_max_tx_power(static_cast<int8_t>(stationTransmitPower(mode)));
}

}  // namespace wifi
}  // namespace mesh
