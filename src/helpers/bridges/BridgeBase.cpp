#include "BridgeBase.h"

#include <Arduino.h>

bool BridgeBase::isRunning() const {
  return _initialized;
}

const char *BridgeBase::getLogDateTime() {
  static char tmp[32];
  uint32_t now = _rtc->getCurrentTime();
  DateTime dt = DateTime(now);
  sprintf(tmp, "%02d:%02d:%02d - %d/%d/%d U", dt.hour(), dt.minute(), dt.second(), dt.day(), dt.month(),
          dt.year());
  return tmp;
}

uint16_t BridgeBase::fletcher16(const uint8_t *data, size_t len) {
  uint8_t sum1 = 0, sum2 = 0;

  for (size_t i = 0; i < len; i++) {
    sum1 = (sum1 + data[i]) % 255;
    sum2 = (sum2 + sum1) % 255;
  }

  return (sum2 << 8) | sum1;
}

bool BridgeBase::validateChecksum(const uint8_t *data, size_t len, uint16_t received_checksum) {
  uint16_t calculated_checksum = fletcher16(data, len);
  return received_checksum == calculated_checksum;
}

void BridgeBase::handleReceivedPacket(mesh::Packet *packet) {
  // Guard against uninitialized state
  if (_initialized == false) {
    BRIDGE_DEBUG_PRINTLN("RX packet received before initialization\n");
    _mgr->free(packet);
#if defined(WITH_ESPNOW_BRIDGE) || defined(WITH_RS232_BRIDGE) || defined(WITH_UDP_BRIDGE)
    onIngressResult(IngressResult::Stopped);
#endif
    return;
  }

  if (!preparePacketIngress(packet) || !allowsPacket(packet)) {
    _mgr->free(packet);
#if defined(WITH_ESPNOW_BRIDGE) || defined(WITH_RS232_BRIDGE) || defined(WITH_UDP_BRIDGE)
    onIngressResult(IngressResult::Filtered);
#endif
    return;
  }

  // OTA retries deliberately reuse the same request/data bytes. The mesh
  // owns their bounded retry/longer-path echo cache and hop ceiling; the
  // ordinary bridge FIFO has no expiry and would pin a lost burst forever.
  const bool ota = packet->getPayloadType() == PAYLOAD_TYPE_OTA;
  if (ota || !_seen_packets.wasSeen(packet)) {
#if defined(WITH_ESPNOW_BRIDGE) || defined(WITH_RS232_BRIDGE) || defined(WITH_UDP_BRIDGE)
    // bridge.rxlimit: only packets that are new to this bridge spend it.
    if (!admitIngress()) {
      _mgr->free(packet);
      onIngressResult(IngressResult::Filtered);
      return;
    }
#endif
    if (!ota) _seen_packets.markSeen(packet);
    // bridge_delay provides a buffer to prevent immediate processing conflicts in the mesh network.
    _mgr->queueInbound(packet, millis() + _prefs->bridge_delay);
#if defined(WITH_ESPNOW_BRIDGE) || defined(WITH_RS232_BRIDGE) || defined(WITH_UDP_BRIDGE)
    onIngressResult(IngressResult::Queued);
#endif
  } else {
    _mgr->free(packet);
#if defined(WITH_ESPNOW_BRIDGE) || defined(WITH_RS232_BRIDGE) || defined(WITH_UDP_BRIDGE)
    onIngressResult(IngressResult::Duplicate);
#endif
  }
}
