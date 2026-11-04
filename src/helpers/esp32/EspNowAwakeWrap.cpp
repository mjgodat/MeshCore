// Linked with -Wl,--wrap=esp_wifi_set_ps (MESH_WRAP_ESP_WIFI_SET_PS envs).
//
// Every call to esp_wifi_set_ps in the image -- Arduino's WiFi.setSleep()
// and its STA_START handler replaying a cached preference, MQTT, WebConfig,
// the UDP station, any library -- reaches this first. While an ESP-NOW bridge
// runs the radio must never doze (modem sleep makes it deaf to ESP-NOW
// between beacons), so a request for modem sleep becomes WIFI_PS_NONE before
// it reaches the driver: not even briefly asleep. The ESPNowBridge 250 ms
// watch stays as a backstop for anything inside the precompiled WiFi
// libraries that changes the mode without calling this symbol.
#if defined(ESP32) && defined(MESH_WRAP_ESP_WIFI_SET_PS) && MESH_WRAP_ESP_WIFI_SET_PS

#include <esp_wifi.h>
#include <helpers/BridgeAwake.h>
#include <helpers/esp32/WiFiRadioPolicy.h>

extern "C" esp_err_t __real_esp_wifi_set_ps(wifi_ps_type_t type);

extern "C" esp_err_t __wrap_esp_wifi_set_ps(wifi_ps_type_t type) {
  const bool bridge_running = mesh::wifi::bridgeEspNowChannel().load() != 0;
  return __real_esp_wifi_set_ps(static_cast<wifi_ps_type_t>(
      mesh::wifi::powerSaveWithBridge(static_cast<uint8_t>(type), bridge_running)));
}

#endif
