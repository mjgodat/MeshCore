#include "MeshModel.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <Packet.h>
#include <helpers/AdvertDataHelpers.h>
#include <helpers/UTF8Helpers.h>

#include "GeoMath.h"
#include "RelayNameStore.h"

namespace mesh { namespace head {

namespace {

const uint32_t MS_PER_MIN = 60000UL;
const size_t ADVERT_FIXED = PUB_KEY_SIZE + 4 + SIGNATURE_SIZE;   // pub_key, timestamp, signature

void* defaultAlloc(size_t bytes, void*) { return malloc(bytes); }
void defaultRelease(void* p, void*) { free(p); }

const char* typeName(uint8_t t) {
  switch (t) {
    case PAYLOAD_TYPE_REQ: return "REQ";
    case PAYLOAD_TYPE_RESPONSE: return "RESPONSE";
    case PAYLOAD_TYPE_TXT_MSG: return "TXT_MSG";
    case PAYLOAD_TYPE_ACK: return "ACK";
    case PAYLOAD_TYPE_ADVERT: return "ADV";
    case PAYLOAD_TYPE_GRP_TXT: return "GRP_TXT";
    case PAYLOAD_TYPE_GRP_DATA: return "GRP_DATA";
    case PAYLOAD_TYPE_ANON_REQ: return "ANON_REQ";
    case PAYLOAD_TYPE_PATH: return "PATH";
    case PAYLOAD_TYPE_TRACE: return "TRACE";
    case PAYLOAD_TYPE_MULTIPART: return "MULTIPART";
    case PAYLOAD_TYPE_CONTROL: return "CONTROL";
    case PAYLOAD_TYPE_OTA: return "OTA";
    case PAYLOAD_TYPE_RAW_CUSTOM: return "RAW";
    default: return "OTHER";
  }
}

// FNV-1a over payload type + payload: identifies the same packet heard twice.
uint32_t digestOf(uint8_t type, const uint8_t* p, size_t n) {
  uint32_t h = 2166136261UL;
  h = (h ^ type) * 16777619UL;
  for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 16777619UL;
  return h;
}

// Advert name usable for display: whole string valid UTF-8, no control bytes.
// Copies at most cap-1 bytes, cut on a UTF-8 boundary. False when unusable.
bool cleanName(const char* src, char* dst, size_t cap) {
  const size_t n = strlen(src);
  if (n == 0 || validUtf8PrefixLength(src, n) != n) return false;
  for (size_t i = 0; i < n; i++) {
    const uint8_t c = (uint8_t)src[i];
    if (c < 0x20 || c == 0x7F) return false;
  }
  const size_t keep = validUtf8PrefixLength(src, cap - 1);
  if (keep == 0) return false;
  memcpy(dst, src, keep);
  dst[keep] = 0;
  return true;
}

}  // namespace

MeshModel::MeshModel(size_t node_capacity, size_t ring_capacity, const ModelAllocator* allocator)
    : _nodes(nullptr), _lru(nullptr), _node_cap(0), _node_count(0), _lru_seq(0),
      _feed(nullptr), _feed_cap(0), _feed_count(0), _feed_head(0), _feed_seq(0),
      _has_clock(false), _clock_raw(0), _clock_ms(0),
      _has_vitals(false), _vitals_ms(0), _has_health(false), _health_ms(0), _path_len(0), _path_seq(0), _path_item_seq(0), _has_head_key(false), _has_rep_key(false), _rep_by_pos(false), _digest_head(0), _digest_count(0), _names(nullptr) {
  _alloc.alloc = defaultAlloc;
  _alloc.release = defaultRelease;
  _alloc.ctx = nullptr;
  if (allocator != nullptr && allocator->alloc != nullptr && allocator->release != nullptr) _alloc = *allocator;
  memset(_rate, 0, sizeof(_rate));
  memset(_rate_min, 0, sizeof(_rate_min));
  memset(_rate_valid, 0, sizeof(_rate_valid));
  memset(_nf, 0, sizeof(_nf));
  memset(_nf_min, 0, sizeof(_nf_min));
  memset(_nf_valid, 0, sizeof(_nf_valid));
  memset(&_vitals, 0, sizeof(_vitals));
  memset(&_health, 0, sizeof(_health));
  memset(_heap_low, 0, sizeof(_heap_low));
  memset(_heap_slot, 0, sizeof(_heap_slot));
  memset(_heap_valid, 0, sizeof(_heap_valid));
  memset(_path, 0, sizeof(_path));
  memset(_digest, 0, sizeof(_digest));
  memset(_head_key, 0, sizeof(_head_key));
  memset(_rep_key, 0, sizeof(_rep_key));
  memset(_rep_name, 0, sizeof(_rep_name));
  if (node_capacity == 0 || ring_capacity == 0) return;
  _nodes = (NodeInfo*)_alloc.alloc(node_capacity * sizeof(NodeInfo), _alloc.ctx);
  _lru = (uint32_t*)_alloc.alloc(node_capacity * sizeof(uint32_t), _alloc.ctx);
  _feed = (FeedItem*)_alloc.alloc(ring_capacity * sizeof(FeedItem), _alloc.ctx);
  if (_nodes == nullptr || _lru == nullptr || _feed == nullptr) {
    // Inert model: every query reports empty, every input is ignored.
    if (_nodes) _alloc.release(_nodes, _alloc.ctx);
    if (_lru) _alloc.release(_lru, _alloc.ctx);
    if (_feed) _alloc.release(_feed, _alloc.ctx);
    _nodes = nullptr; _lru = nullptr; _feed = nullptr;
    return;
  }
  memset(_nodes, 0, node_capacity * sizeof(NodeInfo));
  memset(_lru, 0, node_capacity * sizeof(uint32_t));
  memset(_feed, 0, ring_capacity * sizeof(FeedItem));
  _node_cap = node_capacity;
  _feed_cap = ring_capacity;
}

MeshModel::~MeshModel() {
  if (_nodes) _alloc.release(_nodes, _alloc.ctx);
  if (_lru) _alloc.release(_lru, _alloc.ctx);
  if (_feed) _alloc.release(_feed, _alloc.ctx);
}

void MeshModel::advance(uint32_t now_ms) {
  if (!_has_clock) {
    _has_clock = true;
    _clock_ms = now_ms;
  } else {
    _clock_ms += (uint32_t)(now_ms - _clock_raw);   // modulo 2^32: survives the millis wrap
  }
  _clock_raw = now_ms;
}

void MeshModel::tick(uint32_t now_ms) { advance(now_ms); }

void MeshModel::onObserverPacket(const observer::PacketRecord& rec, uint32_t now_ms) {
  advance(now_ms);
  if (!ok()) return;
  countRate();
  const size_t len = rec.len <= observer::MAX_RAW ? rec.len : observer::MAX_RAW;
  ingest(rec.raw, len, rec.truncated, true, rec.rx_ms, rec.snr_q4, rec.rssi);
}

void MeshModel::onVitals(const observer::Vitals& v, uint32_t now_ms) {
  advance(now_ms);
  _vitals = v;
  _vitals_ms = now_ms;
  _has_vitals = true;
  {
    const uint64_t m = _clock_ms / MS_PER_MIN;
    const size_t slot = (size_t)(m % 60);
    _nf[slot] = v.noise_floor;
    _nf_min[slot] = m;
    _nf_valid[slot] = true;
  }
  // A node already known at the repeater's own position is the repeater.
  if (ok() && v.has_pos) {
    for (size_t i = 0; i < _node_count; i++) {
      const NodeInfo& n = _nodes[i];
      if (n.type == ADV_TYPE_REPEATER && n.has_pos) {
        learnRepeater(n.pub_prefix, n.type, false, 1, true, true, n.lat_e6, n.lon_e6);
        if (_has_rep_key && memcmp(n.pub_prefix, _rep_key, sizeof(_rep_key)) == 0) break;
      }
    }
  }
}

void MeshModel::onHealth(const observer::Health& h, uint32_t now_ms) {
  advance(now_ms);
  _health = h;
  _health_ms = now_ms;
  _has_health = true;
  const uint64_t s = _clock_ms / kHeapSlotMs;
  const size_t slot = (size_t)(s % kHeapSlots);
  if (!_heap_valid[slot] || _heap_slot[slot] != s || h.free_heap < _heap_low[slot]) _heap_low[slot] = h.free_heap;
  _heap_slot[slot] = s;
  _heap_valid[slot] = true;
}

void MeshModel::heapLowPerSlot(uint32_t out[kHeapSlots], bool has[kHeapSlots]) const {
  const uint64_t now = _clock_ms / kHeapSlotMs;
  for (size_t k = 0; k < kHeapSlots; k++) {
    out[k] = 0;
    has[k] = false;
    const uint64_t back = kHeapSlots - 1 - k;
    if (!_has_clock || now < back) continue;
    const uint64_t s = now - back;
    const size_t slot = (size_t)(s % kHeapSlots);
    if (_heap_valid[slot] && _heap_slot[slot] == s) { out[k] = _heap_low[slot]; has[k] = true; }
  }
}

void MeshModel::onBridgedPacket(const uint8_t* raw, size_t len, uint32_t now_ms) {
  advance(now_ms);
  if (!ok()) return;
  ingest(raw, len, false, false, 0, 0, 0);
}

void MeshModel::ingest(const uint8_t* raw, size_t len, bool truncated, bool observed,
                       uint32_t m5_ms, int8_t snr_q4, int16_t rssi) {
  if (raw == nullptr || len < 2 || len > MAX_TRANS_UNIT) return;

  Packet pkt;
  if (!pkt.readFrom(raw, (uint8_t)len)) return;   // header, transport codes, path checked here

  const uint8_t type = pkt.getPayloadType();
  const uint8_t hash_size = pkt.getPathHashSize();   // 1..3 (4 rejected by readFrom)
  const uint8_t count = pkt.getPathHashCount();
  // Flood paths list the forwarders so far; direct paths list the hops still to
  // go; TRACE paths carry SNR bytes. Only a flood path tells the last hop.
  const bool flood_path = pkt.isRouteFlood() && type != PAYLOAD_TYPE_TRACE && count > 0;
  const PathHash last_hop = flood_path ? makePathHash(&pkt.path[(size_t)(count - 1) * hash_size], hash_size)
                                       : makePathHash(nullptr, 0);

  const uint32_t digest = digestOf(type, pkt.payload, pkt.payload_len);
  const bool repeat = seenRecently(digest);
  if (repeat && !observed) return;   // bridged copy of a packet already seen: nothing new
  if (!repeat) rememberDigest(digest);

  // Advert: sender identity + app data (name, type, position).
  bool is_advert = false;
  char name[sizeof(((NodeInfo*)0)->name)];
  uint8_t adv_type = 0;
  bool pos = false;
  int32_t lat = 0, lon = 0;
  if (type == PAYLOAD_TYPE_ADVERT && !truncated && pkt.payload_len >= ADVERT_FIXED &&
      pkt.payload_len <= ADVERT_FIXED + MAX_ADVERT_DATA_SIZE) {
    AdvertDataParser parser(&pkt.payload[ADVERT_FIXED], (uint8_t)(pkt.payload_len - ADVERT_FIXED));
    adv_type = parser.getType();
    pos = parser.hasLatLon();
    lat = parser.getIntLat(); lon = parser.getIntLon();
    is_advert = parser.isValid() && adv_type >= ADV_TYPE_CHAT && adv_type <= ADV_TYPE_SENSOR && parser.hasName() &&
                cleanName(parser.getName(), name, sizeof(name)) && (!pos || geoValid(lat, lon));
    if (pos && lat == 0 && lon == 0) pos = false;   // 0,0 = no fix
  }

  // Self: the head's own adverts and the observing repeater's never become
  // nodes (the repeater is the centre of every view, the head is not shown).
  bool self_advert = false;
  if (is_advert) {
    if (_has_head_key && memcmp(pkt.payload, _head_key, sizeof(_head_key)) == 0) {
      self_advert = true;
    } else {
      learnRepeater(pkt.payload, adv_type, observed, count, repeat, pos, lat, lon);
      self_advert = _has_rep_key && memcmp(pkt.payload, _rep_key, sizeof(_rep_key)) == 0;
      if (self_advert) memcpy(_rep_name, name, sizeof(_rep_name));
      // Relay names outlive the model (reboots): new or renamed ones are
      // written back by the page task, throttled.
      if (_names != nullptr && adv_type == ADV_TYPE_REPEATER) _names->learn(pkt.payload, name);
    }
  }

  // Signal target: the sender of a 0-hop advert, else the unique last-hop repeater.
  NodeInfo* sender = nullptr;
  if (is_advert && !self_advert) {
    // A repeat updates signal only -- unless the node is missing because the
    // first (bridged) copy was taken for the repeater's own advert and this
    // observed 0-hop copy just proved it a neighbour.
    if (repeat && !(observed && count == 0 && findByPrefix(pkt.payload) == nullptr)) {
      sender = findByPrefix(pkt.payload);   // repeat: signal only, never create or rename
    } else {
      sender = upsertNode(pkt.payload);
      memcpy(sender->name, name, sizeof(name));
      sender->type = adv_type;
      sender->has_pos = pos;
      sender->lat_e6 = pos ? lat : 0;
      sender->lon_e6 = pos ? lon : 0;
      sender->hops = pkt.isRouteFlood() ? count : 0;
      sender->advert_via = last_hop;   // invalid for 0 hops (and direct routes)
      sender->advert_via_snr_q4 = observed ? snr_q4 : SNR_NONE;
      sender->packets++;
      heard(sender);
    }
    if (repeat && sender != nullptr && observed && sender->advert_via_snr_q4 == SNR_NONE &&
        sender->advert_via == last_hop)
      sender->advert_via_snr_q4 = snr_q4;   // the observer's copy of an advert first bridged
  }
  if (observed) {
    NodeInfo* target = nullptr;
    if (sender != nullptr && count == 0) target = sender;   // 0 hops: the sender transmitted it
    else if (last_hop.valid()) target = resolveHash(last_hop);
    if (target != nullptr) {
      // A relay is counted per observed reception attributed to it (bridged copies
      // never attribute, so bridge/observer order does not matter); an advert
      // sender was already counted for its advert.
      if (target != sender) target->packets++;
      sample(target, snr_q4, rssi);
    }
  }
  if (repeat) return;

  // Animation path: origin (advert sender) then forwarders in travel order.
  const bool new_path = (is_advert && sender != nullptr) || flood_path;
  if (new_path) {
    _path_len = 0;
    if (is_advert && sender != nullptr) _path[_path_len++] = makePathHash(sender->pub_prefix, hash_size);
    if (flood_path) {
      for (uint8_t i = 0; i < count && _path_len < sizeof(_path) / sizeof(_path[0]); i++)
        _path[_path_len++] = makePathHash(&pkt.path[(size_t)i * hash_size], hash_size);
    }
  }

  FeedItem item;
  memset(&item, 0, sizeof(item));
  item.ms = (uint32_t)_clock_ms;
  item.m5_rx_ms = observed ? m5_ms : 0;
  item.observed = observed;
  item.payload_type = type;
  item.hops = flood_path ? count : 0;
  item.snr_q4 = observed ? snr_q4 : 0;
  item.rssi = observed ? rssi : 0;
  if (last_hop.valid()) item.via = last_hop;
  else if (sender != nullptr && count == 0) item.via = makePathHash(sender->pub_prefix, hash_size);
  if (is_advert && sender != nullptr) {
    item.has_sender = true;
    memcpy(item.sender, sender->pub_prefix, sizeof(item.sender));
  }
  if (new_path) {
    const size_t skip = _path_len > FEED_ROUTE ? _path_len - FEED_ROUTE : 0;
    item.route_len = (uint8_t)(_path_len - skip);
    for (size_t i = 0; i < item.route_len; i++) item.route[i] = _path[skip + i];
  }
  if (is_advert) snprintf(item.label, sizeof(item.label), "ADV %s", name);
  else snprintf(item.label, sizeof(item.label), "%s", typeName(type));
  addFeed(item);
  if (new_path) {
    _path_seq++;
    _path_item_seq = _feed_seq;
  }
}

void MeshModel::setHeadKey(const uint8_t* pub_key) {
  _has_head_key = pub_key != nullptr;
  if (!_has_head_key) return;
  memcpy(_head_key, pub_key, sizeof(_head_key));
  NodeInfo* n = findByPrefix(pub_key);
  if (n != nullptr) removeNode(n);
}

bool MeshModel::repeaterKey(uint8_t out[4]) const {
  if (!_has_rep_key) return false;
  memcpy(out, _rep_key, sizeof(_rep_key));
  return true;
}

// The observer's own identity is not in its frames; learn it from adverts:
//  - a repeater advert whose position is the vitals position (< 100 m), or
//  - a 0-hop repeater advert that reached the head only over the bridge: on
//    LoRa a neighbour's 0-hop advert is received by the observer first, and
//    the repeater forwards it with its own hash appended (1 hop), so a
//    bridged 0-hop advert is the repeater's own (bridge "source tx").
// A 0-hop advert the observer itself received cannot be its own: clear.
void MeshModel::learnRepeater(const uint8_t* pub_key, uint8_t adv_type, bool observed, uint8_t hops,
                              bool repeat, bool pos, int32_t lat, int32_t lon) {
  if (adv_type != ADV_TYPE_REPEATER) return;
  const bool same = _has_rep_key && memcmp(pub_key, _rep_key, sizeof(_rep_key)) == 0;
  if (observed && hops == 0) {
    if (same) { _has_rep_key = false; _rep_by_pos = false; }
    return;
  }
  bool by_pos = false;
  if (pos && _has_vitals && _vitals.has_pos && geoValid(_vitals.lat_e6, _vitals.lon_e6) &&
      distanceKm(lat, lon, _vitals.lat_e6, _vitals.lon_e6) < 0.1) {
    by_pos = true;
  }
  const bool by_bridge = !observed && hops == 0 && !repeat;
  if (!by_pos && !by_bridge) return;
  // A different key replaces a known one only on position evidence: another
  // bridged-only 0-hop repeater must not flap the key (and delete nodes).
  if (!same && _has_rep_key && !by_pos) return;
  if (!same) _rep_name[0] = 0;   // the caller names it from this advert
  memcpy(_rep_key, pub_key, sizeof(_rep_key));
  _has_rep_key = true;
  _rep_by_pos = _rep_by_pos && same ? true : by_pos;
  NodeInfo* n = findByPrefix(pub_key);
  if (n != nullptr) removeNode(n);
}

void MeshModel::removeNode(NodeInfo* n) {
  const size_t i = (size_t)(n - _nodes);
  const size_t last = _node_count - 1;
  if (i != last) {
    _nodes[i] = _nodes[last];
    _lru[i] = _lru[last];
  }
  _node_count--;
}

NodeInfo* MeshModel::findByPrefix(const uint8_t* pub_key) {
  for (size_t i = 0; i < _node_count; i++) {
    if (memcmp(_nodes[i].pub_prefix, pub_key, sizeof(_nodes[i].pub_prefix)) == 0) return &_nodes[i];
  }
  return nullptr;
}

NodeInfo* MeshModel::upsertNode(const uint8_t* pub_key) {
  NodeInfo* existing = findByPrefix(pub_key);
  if (existing != nullptr) return existing;
  size_t slot;
  if (_node_count < _node_cap) {
    slot = _node_count++;
  } else {
    slot = 0;   // evict the least recently heard
    for (size_t i = 1; i < _node_count; i++) if (_lru[i] < _lru[slot]) slot = i;
  }
  NodeInfo* n = &_nodes[slot];
  memset(n, 0, sizeof(*n));
  memcpy(n->pub_prefix, pub_key, sizeof(n->pub_prefix));
  memset(n->snr_hist, (uint8_t)SNR_NONE, sizeof(n->snr_hist));
  n->advert_via_snr_q4 = SNR_NONE;
  return n;
}

NodeInfo* MeshModel::resolveHash(const PathHash& h) {
  if (!h.valid()) return nullptr;
  NodeInfo* found = nullptr;
  for (size_t i = 0; i < _node_count; i++) {
    NodeInfo& n = _nodes[i];
    if (n.type != ADV_TYPE_REPEATER || !h.matchesKey(n.pub_prefix)) continue;
    if (found != nullptr) return nullptr;   // ambiguous
    found = &n;
  }
  return found;
}

const NodeInfo* MeshModel::findByHash(const PathHash& hash) const {
  return const_cast<MeshModel*>(this)->resolveHash(hash);
}

size_t MeshModel::repeatersMatching(const PathHash& h, uint8_t key_out[4]) const {
  if (!h.valid()) return 0;
  size_t n = 0;
  for (size_t i = 0; i < _node_count; i++) {
    const NodeInfo& node = _nodes[i];
    if (node.type != ADV_TYPE_REPEATER || !h.matchesKey(node.pub_prefix)) continue;
    if (n == 0 && key_out != nullptr) memcpy(key_out, node.pub_prefix, sizeof(node.pub_prefix));
    n++;
  }
  return n;
}

void MeshModel::heard(NodeInfo* n) {
  n->last_heard_ms = (uint32_t)_clock_ms;
  _lru[n - _nodes] = ++_lru_seq;
}

void MeshModel::sample(NodeInfo* n, int8_t snr_q4, int16_t rssi) {
  heard(n);
  n->last_snr_q4 = snr_q4;
  n->last_rssi = rssi;
  memmove(&n->snr_hist[1], &n->snr_hist[0], sizeof(n->snr_hist) - 1);
  n->snr_hist[0] = snr_q4;
}

void MeshModel::addFeed(const FeedItem& item) {
  _feed[_feed_head] = item;
  _feed_head = (_feed_head + 1) % _feed_cap;
  if (_feed_count < _feed_cap) _feed_count++;
  _feed_seq++;
}

const FeedItem* MeshModel::feed(size_t newest_index) const {
  if (newest_index >= _feed_count) return nullptr;
  return &_feed[(_feed_head + _feed_cap - 1 - newest_index) % _feed_cap];
}

void MeshModel::countRate() {
  const uint64_t m = _clock_ms / MS_PER_MIN;
  const size_t slot = (size_t)(m % 60);
  if (!_rate_valid[slot] || _rate_min[slot] != m) {
    _rate_valid[slot] = true;
    _rate_min[slot] = m;
    _rate[slot] = 0;
  }
  if (_rate[slot] < 0xFFFF) _rate[slot]++;
}

void MeshModel::ratesPerMinute(uint16_t out[60]) const {
  const uint64_t now = _clock_ms / MS_PER_MIN;
  for (size_t k = 0; k < 60; k++) {
    out[k] = 0;
    const uint64_t back = 59 - k;
    if (!_has_clock || now < back) continue;
    const uint64_t m = now - back;
    const size_t slot = (size_t)(m % 60);
    if (_rate_valid[slot] && _rate_min[slot] == m) out[k] = _rate[slot];
  }
}

void MeshModel::noiseFloorPerMinute(int16_t out[60], bool has[60]) const {
  const uint64_t now = _clock_ms / MS_PER_MIN;
  for (size_t k = 0; k < 60; k++) {
    out[k] = 0;
    has[k] = false;
    const uint64_t back = 59 - k;
    if (!_has_clock || now < back) continue;
    const uint64_t m = now - back;
    const size_t slot = (size_t)(m % 60);
    if (_nf_valid[slot] && _nf_min[slot] == m) { out[k] = _nf[slot]; has[k] = true; }
  }
}

bool MeshModel::repeaterPosition(int32_t& lat, int32_t& lon) const {
  if (!_has_vitals || !_vitals.has_pos || !geoValid(_vitals.lat_e6, _vitals.lon_e6)) return false;
  if (_vitals.lat_e6 == 0 && _vitals.lon_e6 == 0) return false;
  lat = _vitals.lat_e6;
  lon = _vitals.lon_e6;
  return true;
}

size_t MeshModel::lastPath(PathHash* out, size_t cap) const {
  if (out == nullptr) return 0;
  const size_t n = _path_len < cap ? _path_len : cap;
  for (size_t i = 0; i < n; i++) out[i] = _path[i];
  return n;
}

const FeedItem* MeshModel::lastPathItem() const {
  if (_path_seq == 0) return nullptr;
  return feed((size_t)(uint32_t)(_feed_seq - _path_item_seq));
}

bool MeshModel::seenRecently(uint32_t digest) const {
  for (size_t i = 0; i < _digest_count; i++) if (_digest[i] == digest) return true;
  return false;
}

void MeshModel::rememberDigest(uint32_t digest) {
  const size_t cap = sizeof(_digest) / sizeof(_digest[0]);
  _digest[_digest_head] = digest;
  _digest_head = (_digest_head + 1) % cap;
  if (_digest_count < cap) _digest_count++;
}

}}  // namespace mesh::head
