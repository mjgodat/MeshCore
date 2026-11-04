#pragma once

// Visual head data model (pure C++11, no Arduino).
//
// Consumes observer records (raw MeshCore packets + SNR/RSSI from the M5),
// observer vitals, and packets the head receives over the ESP-NOW bridge, and
// keeps: a node table (LRU), a feed ring, per-minute RX rates for the last hour
// and the path of the most recent packet with a known path.
//
// Clock contract: every input takes `now_ms`, the HEAD's own millis(). All
// model time (last_heard_ms, FeedItem::ms, the rates window, LRU) is head time;
// it may wrap at 2^32 (differences are taken modulo 2^32). The M5's rx_ms is
// kept only as FeedItem::m5_rx_ms for display against vitals uptime; it never
// drives windowing, so an M5 reboot or millis wrap does not stall the model.
//
// Signal attribution (spec "Data"): LoRa SNR/RSSI belongs to the last-hop
// transmitter. 0-hop adverts are attributed to the sender; flood packets with
// hops to the last path hash, resolved to a node only when exactly one known
// repeater's public key starts with that hash (compared on the hash's own size,
// 1..3 bytes). Direct-routed packets carry the remaining route, not the
// traversed one, and TRACE paths carry SNR bytes: neither is attributed.
//
// Duplicates: a packet (payload type + payload) seen again within the last 64
// packets updates nothing except signal: an observer repeat adds an SNR sample
// (and a `packets` count) to the relay it is attributed to; a bridged repeat is
// ignored entirely. Adverts, the feed and lastPath take each packet once, so the
// same packet arriving via observer and bridge (either order) counts once.
//
// Adverts are NOT signature-checked (display only): a spoofed advert can add or
// rename a node.

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <helpers/bridges/ObserverFrame.h>

namespace mesh { namespace head {

static const int8_t SNR_NONE = -128;   // empty snr_hist slot (below any LoRa SNR)

// A MeshCore path hash: the first `size` bytes (1..3) of a node's public key.
// size == 0 means unknown / none.
class RelayNameStore;   // RelayNameStore.h

struct PathHash {
  uint8_t size;
  uint8_t bytes[3];
  bool valid() const { return size >= 1 && size <= 3; }
  bool matchesKey(const uint8_t* pub_prefix) const { return valid() && memcmp(bytes, pub_prefix, size) == 0; }
  bool operator==(const PathHash& o) const { return size == o.size && memcmp(bytes, o.bytes, size) == 0; }
};

inline PathHash makePathHash(const uint8_t* bytes, uint8_t size) {
  PathHash h; memset(&h, 0, sizeof(h));
  if (bytes != nullptr && size >= 1 && size <= 3) { h.size = size; memcpy(h.bytes, bytes, size); }
  return h;
}

struct NodeInfo {
  uint8_t pub_prefix[4];  // first 4 key bytes; a path hash of size n is pub_prefix[0..n)
  char name[24];
  uint8_t type;           // ADV_TYPE_*
  bool has_pos;
  int32_t lat_e6, lon_e6;
  uint32_t last_heard_ms; // head clock
  int8_t last_snr_q4;
  int16_t last_rssi;
  uint8_t hops;           // hops of the most recent advert
  uint32_t packets;       // distinct packets sent by (adverts) or attributed to this node
  int8_t snr_hist[32];    // newest first; SNR_NONE when empty
  // How the most recent advert reached the M5: its last hop (invalid for a
  // 0-hop advert) and the SNR the M5 heard that hop at (SNR_NONE until an
  // observed copy arrives; later repeats over other relays do not move it).
  PathHash advert_via;
  int8_t advert_via_snr_q4;
};

static const size_t FEED_ROUTE = 8;   // FeedItem::route entries

struct FeedItem {
  uint32_t ms;            // head clock when received
  uint32_t m5_rx_ms;      // M5 millis at RX (observer only; 0 for bridged)
  bool observed;          // from the observer feed (has signal) vs bridged
  uint8_t payload_type;   // PAYLOAD_TYPE_*
  uint8_t hops;           // traversed hops (flood path count; 0 for direct/TRACE)
  int8_t snr_q4;          // 0 for bridged packets
  int16_t rssi;           // dBm; 0 for bridged packets
  bool has_sender;        // an advert whose sender parsed...
  uint8_t sender[4];      // ...its key prefix (whole, not a path hash)
  PathHash via;           // last-hop hash (sender's for 0-hop adverts); invalid = unknown
  char label[28];
  // The packet's path as lastPath() gives it (sender first for adverts, then
  // forwarders in travel order), its last FEED_ROUTE entries (the hops
  // nearest the receiver). route_len 0 = none (lastPath() was unchanged).
  uint8_t route_len;
  PathHash route[FEED_ROUTE];
};

// Storage allocator; on device pass a PSRAM allocator, null = malloc/free.
struct ModelAllocator {
  void* (*alloc)(size_t bytes, void* ctx);
  void (*release)(void* p, void* ctx);
  void* ctx;
};

class MeshModel {
 public:
  explicit MeshModel(size_t node_capacity = 256, size_t ring_capacity = 512,
                     const ModelAllocator* allocator = nullptr);
  ~MeshModel();

  void onObserverPacket(const observer::PacketRecord& rec, uint32_t now_ms);
  void onVitals(const observer::Vitals& v, uint32_t now_ms);
  // The repeater's heap and reset reason (observer kind 3, every ~30 s;
  // repeaters older than the reliability round never send it).
  void onHealth(const observer::Health& h, uint32_t now_ms);
  // Packets the head receives over the bridge: no signal data, no rate count.
  void onBridgedPacket(const uint8_t* raw, size_t len, uint32_t now_ms);
  // Advance the head clock without input (rates window rolls forward).
  void tick(uint32_t now_ms);

  size_t nodeCount() const { return _node_count; }
  const NodeInfo* node(size_t i) const { return i < _node_count ? &_nodes[i] : nullptr; }
  // The unique repeater whose key starts with `hash`; null when invalid, unknown or ambiguous.
  const NodeInfo* findByHash(const PathHash& hash) const;
  // How many repeaters in the node table match `hash` (0 when invalid);
  // key_out (may be null): the first match's key prefix.
  size_t repeatersMatching(const PathHash& hash, uint8_t key_out[4]) const;

  // Persistent relay names (RelayNameStore.h): every repeater advert's name
  // (the observing repeater's too) is learned into it; widgets::relayName
  // falls back to it for relays the model has not heard this boot. Not
  // owned; null = none.
  void setNameStore(RelayNameStore* store) { _names = store; }
  const RelayNameStore* nameStore() const { return _names; }

  size_t feedCount() const { return _feed_count; }
  const FeedItem* feed(size_t newest_index) const;   // 0 = newest

  bool repeaterPosition(int32_t& lat, int32_t& lon) const;
  const observer::Vitals* vitals() const { return _has_vitals ? &_vitals : nullptr; }
  uint32_t vitalsMs() const { return _vitals_ms; }   // head clock of the last vitals
  const observer::Health* health() const { return _has_health ? &_health : nullptr; }
  uint32_t healthMs() const { return _health_ms; }   // head clock of the last health frame

  // The repeater's free heap, lowest value reported in each kHeapSlotMs
  // slot over the last kHeapSlots slots (8 h): out[kHeapSlots - 1] = the
  // current slot. A leak shows as a falling line. has[k] false for slots
  // without a health frame.
  static const size_t kHeapSlots = 48;
  static const uint32_t kHeapSlotMs = 600000UL;   // 10 min
  void heapLowPerSlot(uint32_t out[kHeapSlots], bool has[kHeapSlots]) const;

  // RX packets per minute (observer records, head clock):
  // out[59] = current minute, out[0] = 59 minutes ago.
  void ratesPerMinute(uint16_t out[60]) const;
  // The repeater's noise floor per minute (the last vitals of each minute,
  // same window as ratesPerMinute); has[k] false for minutes without vitals.
  void noiseFloorPerMinute(int16_t out[60], bool has[60]) const;

  // Path of the most recent first-seen packet that has a known path: an advert
  // whose sender parsed (sender first, at the packet's hash size) and/or a flood
  // with hops (forwarders in travel order). Packets without one (direct, TRACE,
  // 0-hop non-advert, duplicates) leave it unchanged. Returns entries written.
  size_t lastPath(PathHash* out, size_t cap) const;
  // Bumped whenever lastPath() changes (0 = no path yet); wraps.
  uint32_t pathSeq() const { return _path_seq; }
  // The feed item of the packet that set lastPath(); null when none or
  // already overwritten in the feed ring.
  const FeedItem* lastPathItem() const;
  // Feed items ever added (wraps): new items since a previous value v are
  // feed(0 .. feedSeq() - v - 1).
  uint32_t feedSeq() const { return _feed_seq; }

  // The head's own key (first 4 bytes): its adverts never become a node.
  void setHeadKey(const uint8_t* pub_key);
  // The observing repeater's key prefix once learnt from its adverts (see
  // learnRepeater); its adverts never become a node either.
  bool repeaterKey(uint8_t out[4]) const;
  // The observing repeater's advertised name ("" until one of its own
  // adverts has been seen with the key known).
  const char* repeaterName() const { return _has_rep_key ? _rep_name : ""; }

  bool ok() const { return _nodes != nullptr && _feed != nullptr; }

 private:
  MeshModel(const MeshModel&);
  MeshModel& operator=(const MeshModel&);

  void ingest(const uint8_t* raw, size_t len, bool truncated, bool observed,
              uint32_t m5_ms, int8_t snr_q4, int16_t rssi);
  NodeInfo* findByPrefix(const uint8_t* pub_key);
  NodeInfo* upsertNode(const uint8_t* pub_key);
  NodeInfo* resolveHash(const PathHash& h);
  void learnRepeater(const uint8_t* pub_key, uint8_t adv_type, bool observed, uint8_t hops, bool repeat,
                     bool pos, int32_t lat, int32_t lon);
  void removeNode(NodeInfo* n);
  void heard(NodeInfo* n);
  void sample(NodeInfo* n, int8_t snr_q4, int16_t rssi);
  void addFeed(const FeedItem& item);
  void advance(uint32_t now_ms);
  void countRate();
  bool seenRecently(uint32_t digest) const;
  void rememberDigest(uint32_t digest);

  ModelAllocator _alloc;
  NodeInfo* _nodes;
  uint32_t* _lru;          // touch sequence per node slot
  size_t _node_cap, _node_count;
  uint32_t _lru_seq;
  FeedItem* _feed;
  size_t _feed_cap, _feed_count, _feed_head;   // head = next write slot
  uint32_t _feed_seq;
  // Head clock: last raw millis and a 64-bit monotonic extension (wrap-safe).
  bool _has_clock;
  uint32_t _clock_raw;
  uint64_t _clock_ms;
  uint16_t _rate[60];
  uint64_t _rate_min[60];
  bool _rate_valid[60];
  int16_t _nf[60];
  uint64_t _nf_min[60];
  bool _nf_valid[60];
  observer::Vitals _vitals;
  bool _has_vitals;
  uint32_t _vitals_ms;
  observer::Health _health;
  bool _has_health;
  uint32_t _health_ms;
  uint32_t _heap_low[kHeapSlots];
  uint64_t _heap_slot[kHeapSlots];
  bool _heap_valid[kHeapSlots];
  PathHash _path[65];
  size_t _path_len;
  uint32_t _path_seq;
  uint32_t _path_item_seq;   // _feed_seq right after the path's item was added
  bool _has_head_key, _has_rep_key, _rep_by_pos;
  char _rep_name[sizeof(((NodeInfo*)0)->name)];
  uint8_t _head_key[4], _rep_key[4];
  uint32_t _digest[64];
  size_t _digest_head, _digest_count;
  RelayNameStore* _names;
};

}}  // namespace mesh::head
