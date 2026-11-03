"""Host tests for the Freenove visual head data model (MeshModel + GeoMath).

Pure C++: MeshModel.cpp is compiled with the production Packet.cpp and
AdvertDataHelpers.cpp (packet/advert parsing is reused, not re-implemented).
Sanitizers (ASan/UBSan) run on non-Windows hosts; MinGW has no libasan.
"""
import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
UI = ROOT / 'examples' / 'companion_radio' / 'ui-freenove'

HARNESS = r'''
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <Packet.h>
#include <helpers/AdvertDataHelpers.h>
#include "MeshModel.h"
#include "GeoMath.h"
using namespace mesh;
using namespace mesh::head;
static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL line %d: %s\n", __LINE__, #cond); failures++; } } while (0)

typedef std::vector<uint8_t> Bytes;

static void key(uint8_t pub[32], uint8_t b0, uint8_t b1, uint8_t b2) {
  for (int i = 0; i < 32; i++) pub[i] = (uint8_t)(0x40 + i);
  pub[0] = b0; pub[1] = b1; pub[2] = b2;
}

static PathHash H1(uint8_t a) { uint8_t b[1] = {a}; return makePathHash(b, 1); }
static PathHash H2(uint8_t a, uint8_t c) { uint8_t b[2] = {a, c}; return makePathHash(b, 2); }
static PathHash H3(uint8_t a, uint8_t c, uint8_t d) { uint8_t b[3] = {a, c, d}; return makePathHash(b, 3); }

// header | [transport codes] | path_len | path | payload
static Bytes packet(uint8_t type, uint8_t route, uint8_t path_len, const Bytes& path, const Bytes& payload) {
  Bytes b;
  b.push_back((uint8_t)((type << PH_TYPE_SHIFT) | route));
  if (route == ROUTE_TYPE_TRANSPORT_FLOOD || route == ROUTE_TYPE_TRANSPORT_DIRECT) { b.push_back(0x12); b.push_back(0x34); b.push_back(0x56); b.push_back(0x78); }
  b.push_back(path_len);
  b.insert(b.end(), path.begin(), path.end());
  b.insert(b.end(), payload.begin(), payload.end());
  return b;
}

// Advert payload as MeshCore puts it on air: pub_key[32] timestamp[4] signature[64] app_data.
// app_data is hand-encoded (flags, int32 lat/lon micro-degrees LE, name) to pin the wire format.
static Bytes advertPayload(const uint8_t pub[32], uint8_t type, const char* name, bool pos, int32_t lat, int32_t lon,
                           uint32_t ts = 1793000000u) {
  Bytes p(pub, pub + 32);
  p.insert(p.end(), (uint8_t*)&ts, (uint8_t*)&ts + 4);
  for (int i = 0; i < 64; i++) p.push_back((uint8_t)(0xA0 ^ i));
  p.push_back((uint8_t)(type | (pos ? ADV_LATLON_MASK : 0) | (name ? ADV_NAME_MASK : 0)));
  if (pos) { p.insert(p.end(), (uint8_t*)&lat, (uint8_t*)&lat + 4); p.insert(p.end(), (uint8_t*)&lon, (uint8_t*)&lon + 4); }
  if (name) p.insert(p.end(), name, name + std::strlen(name));
  return p;
}

static observer::PacketRecord rec(const Bytes& raw, uint32_t m5_ms, int8_t snr_q4, int16_t rssi) {
  observer::PacketRecord r; std::memset(&r, 0, sizeof(r));
  r.rx_ms = m5_ms; r.snr_q4 = snr_q4; r.rssi = rssi; r.disp = observer::Disposition::Forwarded;
  CHECK(observer::setPacketBytes(r, raw.data(), raw.size()));
  return r;
}

// Observer record received by the head at head time `now` (M5 rx_ms deliberately different).
static void obs(MeshModel& m, const Bytes& raw, uint32_t now, int8_t snr_q4, int16_t rssi) {
  m.onObserverPacket(rec(raw, now + 777777u, snr_q4, rssi), now);
}

static Bytes repeaterAdvert(uint8_t b0, uint8_t b1, uint8_t b2, const char* name, uint32_t ts = 1793000000u) {
  uint8_t pub[32]; key(pub, b0, b1, b2);
  return packet(PAYLOAD_TYPE_ADVERT, ROUTE_TYPE_FLOOD, 0, Bytes(), advertPayload(pub, ADV_TYPE_REPEATER, name, false, 0, 0, ts));
}

static const NodeInfo* byName(const MeshModel& m, const char* name) {
  for (size_t i = 0; i < m.nodeCount(); i++) if (std::strcmp(m.node(i)->name, name) == 0) return m.node(i);
  return nullptr;
}

static Bytes grpPayload(uint8_t salt = 0) { Bytes p; p.push_back(0x5C); p.push_back(salt); for (int i = 0; i < 40; i++) p.push_back((uint8_t)(i * 3)); return p; }

static void advert_creates_node_with_gps() {
  MeshModel m;
  CHECK(m.ok());
  uint8_t pub[32]; key(pub, 0x4A, 0x75, 0x70);
  Bytes raw = packet(PAYLOAD_TYPE_ADVERT, ROUTE_TYPE_FLOOD, 0, Bytes(),
                     advertPayload(pub, ADV_TYPE_REPEATER, "Jupiter", true, 47606209, -122332069));
  CHECK(raw[0] == 0x11 && raw[1] == 0x00 && raw.size() == 2 + 100 + 1 + 8 + 7);
  m.onObserverPacket(rec(raw, 55555, -29, -104), 1000);
  CHECK(m.nodeCount() == 1);
  const NodeInfo* n = m.node(0);
  CHECK(n != nullptr);
  if (!n) return;
  CHECK(std::strcmp(n->name, "Jupiter") == 0);
  CHECK(n->type == ADV_TYPE_REPEATER);
  CHECK(n->has_pos && n->lat_e6 == 47606209 && n->lon_e6 == -122332069);
  CHECK(std::fabs(n->lat_e6 / 1e6 - 47.606209) < 1e-9 && std::fabs(n->lon_e6 / 1e6 + 122.332069) < 1e-9);
  CHECK(std::memcmp(n->pub_prefix, pub, 4) == 0);
  CHECK(n->hops == 0 && n->packets == 1 && n->last_heard_ms == 1000);   // head clock, not M5 rx_ms
  CHECK(n->last_snr_q4 == -29 && n->last_rssi == -104);
  CHECK(n->snr_hist[0] == -29 && n->snr_hist[1] == SNR_NONE && n->snr_hist[31] == SNR_NONE);
  CHECK(m.findByHash(H2(0x4A, 0x75)) == n && m.findByHash(H1(0x4A)) == n && m.findByHash(H3(0x4A, 0x75, 0x70)) == n);
  const FeedItem* f = m.feed(0);
  CHECK(f && f->payload_type == PAYLOAD_TYPE_ADVERT && f->hops == 0 && f->snr_q4 == -29);
  CHECK(f && f->ms == 1000 && f->m5_rx_ms == 55555 && f->observed);
  CHECK(f && f->via == H1(0x4A));   // 0-hop advert: sender, at the packet's hash size (mode 0 = 1 byte)
  CHECK(f && std::strstr(f->label, "Jupiter") != nullptr);
  CHECK(m.feed(1) == nullptr && m.feedCount() == 1);
  PathHash path[8];
  CHECK(m.lastPath(path, 8) == 1 && path[0] == H1(0x4A));

  // The same node re-advertised over 2 hops (1-byte hashes) via transport flood: updated in place.
  Bytes hop; hop.push_back(0x11); hop.push_back(0x22);
  Bytes raw2 = packet(PAYLOAD_TYPE_ADVERT, ROUTE_TYPE_TRANSPORT_FLOOD, 2, hop,
                      advertPayload(pub, ADV_TYPE_REPEATER, "Jupiter2", true, 47000000, -122000000, 1793000100u));
  obs(m, raw2, 2000, 8, -90);
  CHECK(m.nodeCount() == 1 && std::strcmp(m.node(0)->name, "Jupiter2") == 0 && m.node(0)->hops == 2);
  CHECK(m.node(0)->packets == 2 && m.node(0)->lat_e6 == 47000000 && m.node(0)->last_heard_ms == 2000);
  CHECK(m.node(0)->last_snr_q4 == -29);   // 2-hop advert: signal belongs to hop 0x22, not the sender
  CHECK(m.lastPath(path, 8) == 3 && path[0] == H1(0x4A) && path[1] == H1(0x11) && path[2] == H1(0x22));
  CHECK(m.lastPath(path, 2) == 2);

  // An advert built by the production encoder parses the same way.
  uint8_t app[MAX_ADVERT_DATA_SIZE];
  AdvertDataBuilder builder(ADV_TYPE_CHAT, "Io", 47.5, -122.25);
  uint8_t alen = builder.encodeTo(app);
  uint8_t pub3[32]; key(pub3, 0x10, 0x20, 0x30);
  Bytes pay(pub3, pub3 + 32); for (int i = 0; i < 68; i++) pay.push_back(0);
  pay.insert(pay.end(), app, app + alen);
  m.onBridgedPacket(packet(PAYLOAD_TYPE_ADVERT, ROUTE_TYPE_FLOOD, 0, Bytes(), pay).data(), 2 + pay.size(), 3000);
  const NodeInfo* io = byName(m, "Io");
  CHECK(io && io->type == ADV_TYPE_CHAT && io->has_pos && io->lat_e6 == 47500000 && io->lon_e6 == -122250000);
  CHECK(io && io->snr_hist[0] == SNR_NONE && io->last_heard_ms == 3000);   // bridged: no signal sample
  CHECK(m.feed(0) && !m.feed(0)->observed && m.feed(0)->m5_rx_ms == 0);

  // Long names are cut at a UTF-8 boundary and stay terminated.
  uint8_t pub4[32]; key(pub4, 0x77, 0x01, 0x02);
  obs(m, packet(PAYLOAD_TYPE_ADVERT, ROUTE_TYPE_FLOOD, 0, Bytes(),
      advertPayload(pub4, ADV_TYPE_ROOM, "ABCDEFGHIJKLMNOPQRSTUV\xC3\xA9xyz", false, 0, 0)), 4000, 0, -80);
  CHECK(m.findByHash(H2(0x77, 0x01)) == nullptr);   // rooms do not relay: findByHash only resolves repeaters
  bool found = false;
  for (size_t i = 0; i < m.nodeCount(); i++) if (m.node(i)->pub_prefix[0] == 0x77) {
    found = true; CHECK(std::strcmp(m.node(i)->name, "ABCDEFGHIJKLMNOPQRSTUV") == 0); }
  CHECK(found);
}

// How a node's latest advert reached the M5: the last hop and the SNR the M5
// heard that copy at (NODES "-4.2 via Downtown"); none for a 0-hop advert.
static void advert_via_recorded() {
  MeshModel m;
  obs(m, repeaterAdvert(0x7E, 0x76, 0x62, "Downtown"), 100, -12, -110);
  const NodeInfo* d = byName(m, "Downtown");
  CHECK(d && d->hops == 0 && !d->advert_via.valid() && d->advert_via_snr_q4 == -12);
  // A chat node two hops out (1-byte hashes), last hop 0x7E, M5 hears it at -4.25 dB.
  uint8_t pub[32]; key(pub, 0x31, 0x32, 0x33);
  Bytes path; path.push_back(0x11); path.push_back(0x7E);
  obs(m, packet(PAYLOAD_TYPE_ADVERT, ROUTE_TYPE_FLOOD, 2, path, advertPayload(pub, ADV_TYPE_CHAT, "Zed", false, 0, 0)), 200, -17, -100);
  const NodeInfo* z = byName(m, "Zed");
  CHECK(z && z->hops == 2 && z->advert_via == H1(0x7E) && z->advert_via_snr_q4 == -17);
  CHECK(z && z->snr_hist[0] == SNR_NONE);   // its own link SNR is still unknown
  // Bridged first (no signal), then the observer's copy of the same advert adds the SNR.
  uint8_t pub2[32]; key(pub2, 0x41, 0x42, 0x43);
  Bytes yan = packet(PAYLOAD_TYPE_ADVERT, ROUTE_TYPE_FLOOD, 1, Bytes(1, 0x7E), advertPayload(pub2, ADV_TYPE_CHAT, "Yan", false, 0, 0));
  m.onBridgedPacket(yan.data(), yan.size(), 300);
  const NodeInfo* y = byName(m, "Yan");
  CHECK(y && y->advert_via == H1(0x7E) && y->advert_via_snr_q4 == SNR_NONE);
  obs(m, yan, 310, 9, -95);
  CHECK(y && y->advert_via == H1(0x7E) && y->advert_via_snr_q4 == 9);
  // A later observed repeat over another relay does not move it.
  Bytes yan2 = packet(PAYLOAD_TYPE_ADVERT, ROUTE_TYPE_FLOOD, 1, Bytes(1, 0x55), advertPayload(pub2, ADV_TYPE_CHAT, "Yan", false, 0, 0));
  obs(m, yan2, 320, 20, -80);
  CHECK(y && y->advert_via == H1(0x7E) && y->advert_via_snr_q4 == 9);
  // Zed re-adverts at 0 hops: heard directly, no relay.
  obs(m, packet(PAYLOAD_TYPE_ADVERT, ROUTE_TYPE_FLOOD, 0, Bytes(), advertPayload(pub, ADV_TYPE_CHAT, "Zed", false, 0, 0, 1793000500u)), 400, 6, -90);
  CHECK(z && z->hops == 0 && !z->advert_via.valid() && z->advert_via_snr_q4 == 6 && z->snr_hist[0] == 6);
}

static void snr_attributed_to_last_hop() {
  MeshModel m;
  obs(m, repeaterAdvert(0xA1, 0x01, 1, "Alpha"), 100, -40, -120);
  obs(m, repeaterAdvert(0xB2, 0x02, 2, "Bravo"), 200, -40, -120);
  obs(m, repeaterAdvert(0xC3, 0x03, 3, "Charlie"), 300, -40, -120);
  CHECK(m.nodeCount() == 3);
  // 3-hop group text, 2-byte hashes (path.hash.mode 2): path_len = (2-1)<<6 | 3.
  Bytes path; path.push_back(0xA1); path.push_back(0x01); path.push_back(0xB2); path.push_back(0x02);
  path.push_back(0xC3); path.push_back(0x03);
  obs(m, packet(PAYLOAD_TYPE_GRP_TXT, ROUTE_TYPE_FLOOD, 0x43, path, grpPayload()), 5000, 22, -88);
  const NodeInfo* a = byName(m, "Alpha"); const NodeInfo* b = byName(m, "Bravo"); const NodeInfo* c = byName(m, "Charlie");
  CHECK(a && b && c);
  if (!a || !b || !c) return;
  CHECK(c->last_snr_q4 == 22 && c->last_rssi == -88 && c->last_heard_ms == 5000 && c->packets == 2);
  CHECK(c->snr_hist[0] == 22 && c->snr_hist[1] == -40 && c->snr_hist[2] == SNR_NONE);
  CHECK(a->last_snr_q4 == -40 && a->snr_hist[1] == SNR_NONE && a->packets == 1 && a->last_heard_ms == 100);
  const FeedItem* f = m.feed(0);
  CHECK(f && f->payload_type == PAYLOAD_TYPE_GRP_TXT && f->hops == 3 && f->via == H2(0xC3, 0x03) && f->snr_q4 == 22);
  PathHash lp[8];
  CHECK(m.lastPath(lp, 8) == 3 && lp[0] == H2(0xA1, 0x01) && lp[1] == H2(0xB2, 0x02) && lp[2] == H2(0xC3, 0x03));

  // Direct route: the path is the remaining route, not the last hop: no attribution,
  // feed via unknown, and lastPath keeps the most recent packet that had a path.
  Bytes route; route.push_back(0xA1); route.push_back(0x01);
  obs(m, packet(PAYLOAD_TYPE_TXT_MSG, ROUTE_TYPE_DIRECT, 0x41, route, grpPayload(1)), 7000, 30, -70);
  CHECK(a->last_snr_q4 == -40 && a->packets == 1);
  CHECK(m.feed(0) && !m.feed(0)->via.valid() && m.feed(0)->hops == 0);
  CHECK(m.lastPath(lp, 8) == 3 && lp[2] == H2(0xC3, 0x03));
  // TRACE (path holds SNR bytes) and 0-hop non-advert floods also leave it unchanged.
  obs(m, packet(PAYLOAD_TYPE_TRACE, ROUTE_TYPE_FLOOD, 2, route, grpPayload(2)), 7100, 30, -70);
  obs(m, packet(PAYLOAD_TYPE_GRP_TXT, ROUTE_TYPE_FLOOD, 0, Bytes(), grpPayload(3)), 7200, 30, -70);
  CHECK(m.lastPath(lp, 8) == 3 && lp[0] == H2(0xA1, 0x01));
  CHECK(a->packets == 1 && b->packets == 1);

  // Bridged packets carry no signal: no sample even with a resolvable last hop.
  Bytes pc; pc.push_back(0xC3); pc.push_back(0x03);
  Bytes pb = packet(PAYLOAD_TYPE_ACK, ROUTE_TYPE_FLOOD, 0x41, pc, Bytes(4, 0x99));
  m.onBridgedPacket(pb.data(), pb.size(), 8000);
  CHECK(c->packets == 2 && c->last_snr_q4 == 22 && c->last_heard_ms == 5000);
  CHECK(m.feed(0) && m.feed(0)->payload_type == PAYLOAD_TYPE_ACK && m.feed(0)->via == H2(0xC3, 0x03) && m.feed(0)->ms == 8000);

  // Truncated record (236..255 B on air): header and path intact, payload cut; still attributed.
  // 32 two-byte hashes (64-byte path, last hop Charlie) + 184-byte payload = 250 bytes.
  Bytes longpath;
  for (int i = 0; i < 31; i++) { longpath.push_back((uint8_t)(0xE0 + (i & 7))); longpath.push_back((uint8_t)i); }
  longpath.push_back(0xC3); longpath.push_back(0x03);
  Bytes big = packet(PAYLOAD_TYPE_GRP_TXT, ROUTE_TYPE_FLOOD, 0x60, longpath, Bytes(184, 0x33));
  CHECK(big.size() == 250);
  observer::PacketRecord t = rec(big, 1, 4, -99);
  CHECK(t.truncated && t.len == observer::MAX_RAW);
  m.onObserverPacket(t, 9000);
  CHECK(c->last_snr_q4 == 4 && c->last_heard_ms == 9000 && c->packets == 3);
  PathHash all[70];
  CHECK(m.lastPath(all, 70) == 32 && all[0] == H2(0xE0, 0x00) && all[31] == H2(0xC3, 0x03));
}

static void one_byte_hash_resolves() {
  // T114-style path.hash.mode 1... as 1-byte hashes (path_len top bits 00).
  MeshModel m;
  obs(m, repeaterAdvert(0xA1, 0x01, 1, "Alpha"), 100, -40, -120);
  obs(m, repeaterAdvert(0xB2, 0x02, 2, "Bravo"), 200, -40, -120);
  Bytes p; p.push_back(0x10); p.push_back(0xB2);
  obs(m, packet(PAYLOAD_TYPE_GRP_TXT, ROUTE_TYPE_FLOOD, 2, p, grpPayload()), 1000, 12, -95);
  const NodeInfo* b = byName(m, "Bravo");
  CHECK(b && b->last_snr_q4 == 12 && b->snr_hist[0] == 12 && b->packets == 2);
  CHECK(m.findByHash(H1(0xB2)) == b && m.findByHash(H1(0x10)) == nullptr);
  CHECK(m.feed(0) && m.feed(0)->via == H1(0xB2) && m.feed(0)->via.size == 1 && m.feed(0)->hops == 2);
  PathHash lp[4];
  CHECK(m.lastPath(lp, 4) == 2 && lp[0] == H1(0x10) && lp[1] == H1(0xB2));
  CHECK(!(H1(0xB2) == H2(0xB2, 0x00)));   // sizes differ: never equal
}

static void three_byte_hash_resolves_and_no_false_match() {
  MeshModel m;
  obs(m, repeaterAdvert(0xA1, 0xB2, 0xC3, "Three"), 100, -40, -120);
  obs(m, repeaterAdvert(0xA1, 0xB2, 0xD4, "Other"), 200, -40, -120);
  CHECK(m.findByHash(H2(0xA1, 0xB2)) == nullptr);   // 2 bytes: ambiguous
  CHECK(m.findByHash(H3(0xA1, 0xB2, 0xC3)) == byName(m, "Three"));
  CHECK(m.findByHash(H3(0xA1, 0xB2, 0xEE)) == nullptr);   // shares 2 bytes, not 3
  // path_len = (3-1)<<6 | 1 = 0x81
  Bytes p; p.push_back(0xA1); p.push_back(0xB2); p.push_back(0xC3);
  obs(m, packet(PAYLOAD_TYPE_GRP_TXT, ROUTE_TYPE_FLOOD, 0x81, p, grpPayload()), 1000, 9, -90);
  const NodeInfo* t = byName(m, "Three"); const NodeInfo* o = byName(m, "Other");
  CHECK(t && t->last_snr_q4 == 9 && t->packets == 2);
  CHECK(o && o->last_snr_q4 == -40 && o->packets == 1);
  CHECK(m.feed(0) && m.feed(0)->via == H3(0xA1, 0xB2, 0xC3));
  // No false match: last hop A1 B2 EE matches neither.
  Bytes q; q.push_back(0xA1); q.push_back(0xB2); q.push_back(0xEE);
  obs(m, packet(PAYLOAD_TYPE_GRP_TXT, ROUTE_TYPE_FLOOD, 0x81, q, grpPayload(1)), 1100, 30, -60);
  CHECK(t && o && t->last_snr_q4 == 9 && o->last_snr_q4 == -40);
  CHECK(m.feed(0) && m.feed(0)->via == H3(0xA1, 0xB2, 0xEE));
}

static void ambiguous_hash_stays_hash() {
  MeshModel m;
  obs(m, repeaterAdvert(0xA1, 0xB2, 0x01, "One"), 100, -10, -100);
  obs(m, repeaterAdvert(0xA1, 0xB2, 0x02, "Two"), 200, -11, -101);
  CHECK(m.nodeCount() == 2);
  CHECK(m.findByHash(H2(0xA1, 0xB2)) == nullptr);
  Bytes p; p.push_back(0xA1); p.push_back(0xB2);
  obs(m, packet(PAYLOAD_TYPE_GRP_TXT, ROUTE_TYPE_FLOOD, 0x41, p, grpPayload()), 1000, 40, -60);
  const NodeInfo* one = byName(m, "One"); const NodeInfo* two = byName(m, "Two");
  CHECK(one && two && one->last_snr_q4 == -10 && two->last_snr_q4 == -11);
  CHECK(one && two && one->packets == 1 && two->packets == 1);
  CHECK(m.feed(0) && m.feed(0)->via == H2(0xA1, 0xB2));
  CHECK(m.feed(0) && std::strstr(m.feed(0)->label, "One") == nullptr && std::strstr(m.feed(0)->label, "Two") == nullptr);
  // An unknown hash also stays a hash.
  Bytes q; q.push_back(0x0F); q.push_back(0x0E);
  obs(m, packet(PAYLOAD_TYPE_GRP_TXT, ROUTE_TYPE_FLOOD, 0x41, q, grpPayload(1)), 1100, 40, -60);
  CHECK(m.nodeCount() == 2 && m.feed(0)->via == H2(0x0F, 0x0E) && m.findByHash(H2(0x0F, 0x0E)) == nullptr);
  // 1-byte collisions: two repeaters starting 0x5A are ambiguous on 1 byte, unique on 2.
  obs(m, repeaterAdvert(0x5A, 0x01, 0, "P1"), 1200, 1, -1);
  obs(m, repeaterAdvert(0x5A, 0x02, 0, "P2"), 1300, 2, -2);
  CHECK(m.findByHash(H1(0x5A)) == nullptr && m.findByHash(H2(0x5A, 0x02)) == byName(m, "P2"));
  Bytes r1; r1.push_back(0x5A);
  obs(m, packet(PAYLOAD_TYPE_GRP_TXT, ROUTE_TYPE_FLOOD, 1, r1, grpPayload(2)), 1400, 33, -50);
  CHECK(byName(m, "P1")->last_snr_q4 == 1 && byName(m, "P2")->last_snr_q4 == 2);
  CHECK(m.feed(0)->via == H1(0x5A));
  // A unique repeater resolves; a chat node sharing its hash does not make it ambiguous.
  obs(m, repeaterAdvert(0x55, 0x66, 0x01, "Rep"), 1500, 1, -1);
  uint8_t pub[32]; key(pub, 0x55, 0x66, 0x09);
  obs(m, packet(PAYLOAD_TYPE_ADVERT, ROUTE_TYPE_FLOOD, 0, Bytes(), advertPayload(pub, ADV_TYPE_CHAT, "Chat", false, 0, 0)), 1600, 2, -2);
  CHECK(m.findByHash(H2(0x55, 0x66)) == byName(m, "Rep"));
  CHECK(m.findByHash(makePathHash(nullptr, 0)) == nullptr);
}

static void bridged_duplicates_not_double_counted() {
  MeshModel m;
  obs(m, repeaterAdvert(0xC3, 0x03, 3, "Charlie"), 100, -40, -120);
  const NodeInfo* c = byName(m, "Charlie");
  CHECK(c && c->packets == 1 && c->last_heard_ms == 100);
  // Bridged echo of the observed advert: nothing changes.
  Bytes adv = repeaterAdvert(0xC3, 0x03, 3, "Charlie");
  m.onBridgedPacket(adv.data(), adv.size(), 200);
  CHECK(c->packets == 1 && c->last_heard_ms == 100 && m.feedCount() == 1);
  // A relayed packet, then the bridged echo of the advert: lastPath stays the relayed one.
  Bytes p; p.push_back(0xA1); p.push_back(0x01); p.push_back(0xC3); p.push_back(0x03);
  Bytes grp = packet(PAYLOAD_TYPE_GRP_TXT, ROUTE_TYPE_FLOOD, 0x42, p, grpPayload());
  obs(m, grp, 300, 10, -90);
  CHECK(c->packets == 2 && c->last_heard_ms == 300 && m.feedCount() == 2);
  m.onBridgedPacket(adv.data(), adv.size(), 400);
  m.onBridgedPacket(grp.data(), grp.size(), 450);
  PathHash lp[4];
  CHECK(m.lastPath(lp, 4) == 2 && lp[1] == H2(0xC3, 0x03));
  CHECK(c->packets == 2 && c->last_heard_ms == 300 && m.feedCount() == 2);
  // Bridged first, observed second: counted once, and the signal sample still lands.
  Bytes grp2 = packet(PAYLOAD_TYPE_GRP_TXT, ROUTE_TYPE_FLOOD, 0x42, p, grpPayload(9));
  m.onBridgedPacket(grp2.data(), grp2.size(), 500);
  CHECK(c->packets == 2 && c->last_snr_q4 == 10 && m.feedCount() == 3);
  obs(m, grp2, 600, -5, -100);
  CHECK(c->packets == 3 && c->last_snr_q4 == -5 && c->last_heard_ms == 600 && m.feedCount() == 3);
  // The same (identical) advert bridged then observed: one advert, one sample on the sender.
  // Bridged alone, a 0-hop repeater advert is taken for the observing repeater's own
  // (no node); the observer hearing it at 0 hops proves it a neighbour.
  Bytes adv2 = repeaterAdvert(0xD4, 0x04, 4, "Delta");
  m.onBridgedPacket(adv2.data(), adv2.size(), 700);
  uint8_t rk[4];
  CHECK(byName(m, "Delta") == nullptr && m.repeaterKey(rk) && rk[0] == 0xD4);
  obs(m, adv2, 800, 7, -77);
  const NodeInfo* d = byName(m, "Delta");
  CHECK(!m.repeaterKey(rk));
  CHECK(d && d->packets == 1 && d->snr_hist[0] == 7 && d->last_heard_ms == 800 && m.feedCount() == 4);
}

static void node_table_evicts_lru() {
  MeshModel m(4, 8);
  const char* names[] = {"N0", "N1", "N2", "N3", "N4"};
  for (int i = 0; i < 4; i++) obs(m, repeaterAdvert((uint8_t)(0x10 * (i + 1)), 1, 0, names[i]), (uint32_t)(i + 1) * 100, 0, -100);
  CHECK(m.nodeCount() == 4);
  obs(m, repeaterAdvert(0x10, 1, 0, "N0", 1793000500u), 500, 0, -100);    // N0 heard again (new advert)
  obs(m, repeaterAdvert(0x50, 1, 0, "N4"), 600, 0, -100);                  // evicts N1 (least recent)
  CHECK(m.nodeCount() == 4);
  CHECK(byName(m, "N0") && byName(m, "N4") && byName(m, "N2") && byName(m, "N3"));
  CHECK(byName(m, "N1") == nullptr);
  const NodeInfo* n4 = byName(m, "N4");
  CHECK(n4 && n4->packets == 1 && n4->snr_hist[1] == SNR_NONE);   // reused slot fully reset
  // Attribution also refreshes recency: N2 heard via path, then N5 evicts N3.
  Bytes p; p.push_back(0x30); p.push_back(0x01);
  obs(m, packet(PAYLOAD_TYPE_GRP_TXT, ROUTE_TYPE_FLOOD, 0x41, p, grpPayload()), 700, 0, -100);
  obs(m, repeaterAdvert(0x60, 1, 0, "N5"), 800, 0, -100);
  CHECK(byName(m, "N2") && byName(m, "N3") == nullptr && byName(m, "N5"));
  // Feed ring keeps the newest 8.
  CHECK(m.feedCount() == 8 && m.feed(0)->ms == 800 && m.feed(7) && m.feed(8) == nullptr);
  // Capacity 0 (allocation failure path) stays inert.
  MeshModel z(0, 0);
  obs(z, repeaterAdvert(0x10, 1, 0, "Z"), 1, 0, 0);
  CHECK(z.nodeCount() == 0 && z.feed(0) == nullptr);
}

static uint32_t rng = 0x12345678u;
static uint32_t rnd() { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

static void checkInvariants(const MeshModel& m, size_t cap) {
  CHECK(m.nodeCount() <= cap);
  for (size_t i = 0; i < m.nodeCount(); i++) CHECK(std::memchr(m.node(i)->name, 0, sizeof(m.node(i)->name)) != nullptr);
  for (size_t i = 0; i < m.feedCount(); i++) {
    CHECK(m.feed(i) && std::memchr(m.feed(i)->label, 0, sizeof(m.feed(i)->label)) != nullptr);
    CHECK(m.feed(i) && m.feed(i)->via.size <= 3);
  }
}

static void model_ignores_garbage_packets() {
  MeshModel m(16, 32);
  // Truncated headers and malformed packets: never a node.
  uint8_t pub[32]; key(pub, 1, 2, 3);
  Bytes adv = advertPayload(pub, ADV_TYPE_REPEATER, "Ok", true, 1000000, 2000000);
  std::vector<Bytes> bad;
  bad.push_back(Bytes());
  bad.push_back(Bytes(1, 0x11));
  bad.push_back(Bytes(1, 0x10));                                  // transport flood, no codes
  { Bytes b(4, 0); b[0] = 0x10; bad.push_back(b); }                 // transport codes cut
  bad.push_back(packet(PAYLOAD_TYPE_ADVERT, ROUTE_TYPE_FLOOD, 5, Bytes(2, 1), Bytes()));         // path longer than packet
  bad.push_back(packet(PAYLOAD_TYPE_ADVERT, ROUTE_TYPE_FLOOD, 0xC1, Bytes(4, 1), adv));          // reserved 4-byte hashes
  { Bytes b = packet(PAYLOAD_TYPE_ADVERT, ROUTE_TYPE_FLOOD, 0, Bytes(), adv); b[0] |= 0x40; bad.push_back(b); }   // payload ver 2
  bad.push_back(packet(PAYLOAD_TYPE_ADVERT, ROUTE_TYPE_FLOOD, 0, Bytes(), Bytes(adv.begin(), adv.begin() + 99)));   // short advert
  bad.push_back(packet(PAYLOAD_TYPE_ADVERT, ROUTE_TYPE_FLOOD, 0, Bytes(), Bytes(adv.begin(), adv.begin() + 104)));  // cut lat/lon
  { Bytes a = adv; a.insert(a.end(), 40, 'x'); bad.push_back(packet(PAYLOAD_TYPE_ADVERT, ROUTE_TYPE_FLOOD, 0, Bytes(), a)); }   // oversized
  { Bytes a = advertPayload(pub, 0, "Ok", false, 0, 0); bad.push_back(packet(PAYLOAD_TYPE_ADVERT, ROUTE_TYPE_FLOOD, 0, Bytes(), a)); }   // type NONE
  { Bytes a = advertPayload(pub, ADV_TYPE_REPEATER, nullptr, false, 0, 0); bad.push_back(packet(PAYLOAD_TYPE_ADVERT, ROUTE_TYPE_FLOOD, 0, Bytes(), a)); }   // no name
  { Bytes a = advertPayload(pub, ADV_TYPE_REPEATER, "x\x01y", false, 0, 0); bad.push_back(packet(PAYLOAD_TYPE_ADVERT, ROUTE_TYPE_FLOOD, 0, Bytes(), a)); }   // control char
  { Bytes a = advertPayload(pub, ADV_TYPE_REPEATER, "\xFF\xFE", false, 0, 0); bad.push_back(packet(PAYLOAD_TYPE_ADVERT, ROUTE_TYPE_FLOOD, 0, Bytes(), a)); }   // bad UTF-8
  { Bytes a = advertPayload(pub, ADV_TYPE_REPEATER, "Ok", true, 91000000, 0); bad.push_back(packet(PAYLOAD_TYPE_ADVERT, ROUTE_TYPE_FLOOD, 0, Bytes(), a)); }   // lat out of range
  for (size_t i = 0; i < bad.size(); i++) {
    m.onBridgedPacket(bad[i].empty() ? nullptr : bad[i].data(), bad[i].size(), 10);
    if (!bad[i].empty() && bad[i].size() <= observer::MAX_RAW) obs(m, bad[i], 10, 1, -1);
    if (m.nodeCount() != 0) std::printf("bad case %u made a node\n", (unsigned)i);
    CHECK(m.nodeCount() == 0);
  }
  m.onBridgedPacket(nullptr, 5, 10);
  CHECK(m.nodeCount() == 0);
  // An advert whose record was truncated is not parsed (its app data may be cut).
  { Bytes a = packet(PAYLOAD_TYPE_ADVERT, ROUTE_TYPE_FLOOD, 0x3F, Bytes(63, 1), adv);
    a.resize(240, 0); observer::PacketRecord r = rec(a, 10, 0, 0); CHECK(r.truncated);
    m.onObserverPacket(r, 10); CHECK(m.nodeCount() == 0); }
  // Random non-advert packets of every length: never a node, no crash.
  for (int it = 0; it < 20000; it++) {
    observer::PacketRecord r; std::memset(&r, 0, sizeof(r));
    r.len = (uint8_t)(rnd() % (observer::MAX_RAW + 1));
    for (size_t i = 0; i < r.len; i++) r.raw[i] = (uint8_t)rnd();
    if (r.len && ((r.raw[0] >> PH_TYPE_SHIFT) & PH_TYPE_MASK) == PAYLOAD_TYPE_ADVERT) r.raw[0] ^= (uint8_t)(1 << PH_TYPE_SHIFT);
    r.truncated = r.len == observer::MAX_RAW && (rnd() & 1);
    r.rx_ms = rnd(); r.snr_q4 = (int8_t)rnd(); r.rssi = (int16_t)rnd();
    m.onObserverPacket(r, rnd());
    m.onBridgedPacket(r.raw, r.len, rnd());
  }
  CHECK(m.nodeCount() == 0);
  checkInvariants(m, 16);
  // Fully random bytes, adverts included: no crash, invariants hold (no signature check, so a
  // random advert-shaped packet with a valid name could legitimately add a node).
  MeshModel f(16, 32);
  for (int it = 0; it < 50000; it++) {
    uint8_t buf[300];
    size_t len = rnd() % sizeof(buf);
    for (size_t i = 0; i < len; i++) buf[i] = (uint8_t)rnd();
    if (rnd() & 1) { buf[0] = (uint8_t)((PAYLOAD_TYPE_ADVERT << PH_TYPE_SHIFT) | (rnd() & 3)); if (len > 1) buf[1] &= 0xBF; }
    f.onBridgedPacket(len ? buf : nullptr, len, rnd());
    observer::PacketRecord r; std::memset(&r, 0, sizeof(r));
    if (len && observer::setPacketBytes(r, buf, len)) f.onObserverPacket(r, rnd());
  }
  checkInvariants(f, 16);
  PathHash lp[70]; CHECK(f.lastPath(lp, 70) <= 65); CHECK(f.lastPath(nullptr, 0) == 0);
  for (size_t i = 0; i < f.lastPath(lp, 70); i++) CHECK(lp[i].valid());
  uint16_t rates[60]; f.ratesPerMinute(rates);
}

static void geo_missing_repeater_position() {
  MeshModel m;
  int32_t lat = 7, lon = 7;
  CHECK(!m.repeaterPosition(lat, lon));
  CHECK(m.vitals() == nullptr);
  observer::Vitals v; std::memset(&v, 0, sizeof(v));
  v.has_pos = false; v.lat_e6 = 47606209; v.lon_e6 = -122332069;
  m.onVitals(v, 100);
  CHECK(m.vitals() != nullptr && m.vitalsMs() == 100);
  CHECK(!m.repeaterPosition(lat, lon));
  v.has_pos = true; v.lat_e6 = 0; v.lon_e6 = 0;          // 0,0 is "no fix"
  m.onVitals(v, 200); CHECK(!m.repeaterPosition(lat, lon));
  v.lat_e6 = 95000000; v.lon_e6 = 1;                      // out of range
  m.onVitals(v, 300); CHECK(!m.repeaterPosition(lat, lon));
  v.lat_e6 = 47606209; v.lon_e6 = -122332069;
  m.onVitals(v, 400);
  CHECK(m.repeaterPosition(lat, lon) && lat == 47606209 && lon == -122332069);
  v.has_pos = false; m.onVitals(v, 500);                  // fix lost
  CHECK(!m.repeaterPosition(lat, lon));
}

static void node_without_gps_not_plotted() {
  MeshModel m;
  obs(m, repeaterAdvert(0x21, 0x22, 0, "NoGps"), 100, 0, -100);
  const NodeInfo* n = byName(m, "NoGps");
  CHECK(n && !n->has_pos);
  uint8_t pub[32]; key(pub, 0x31, 0x32, 0);
  obs(m, packet(PAYLOAD_TYPE_ADVERT, ROUTE_TYPE_FLOOD, 0, Bytes(), advertPayload(pub, ADV_TYPE_REPEATER, "Zero", true, 0, 0, 1)), 200, 0, -100);
  n = byName(m, "Zero");
  CHECK(n && !n->has_pos);
  // A node that had a fix and re-adverts without one is no longer plotted.
  obs(m, packet(PAYLOAD_TYPE_ADVERT, ROUTE_TYPE_FLOOD, 0, Bytes(), advertPayload(pub, ADV_TYPE_REPEATER, "Zero", true, 45000000, -122000000, 2)), 300, 0, -100);
  CHECK(n && n->has_pos);
  obs(m, packet(PAYLOAD_TYPE_ADVERT, ROUTE_TYPE_FLOOD, 0, Bytes(), advertPayload(pub, ADV_TYPE_REPEATER, "Zero", false, 0, 0, 3)), 400, 0, -100);
  CHECK(n && !n->has_pos);
}

static Bytes ack(uint32_t i) { Bytes p(4, 0); std::memcpy(p.data(), &i, 4); return packet(PAYLOAD_TYPE_ACK, ROUTE_TYPE_FLOOD, 0, Bytes(), p); }
static uint32_t ackSeq = 0;
static void obsAck(MeshModel& m, uint32_t now, uint32_t m5_ms) { m.onObserverPacket(rec(ack(ackSeq++), m5_ms, 0, 0), now); }
static int total(const uint16_t r[60]) { int t = 0; for (int i = 0; i < 60; i++) t += r[i]; return t; }

static void rates_per_minute_window() {
  MeshModel m;
  uint16_t r[60];
  m.ratesPerMinute(r);
  CHECK(total(r) == 0);
  const uint32_t MIN = 60000;
  for (int i = 0; i < 3; i++) obsAck(m, 10 * MIN + 1000 * i, 5);   // minute 10: 3
  for (int i = 0; i < 2; i++) obsAck(m, 12 * MIN + 59999, 5);      // minute 12: 2
  Bytes b = ack(999999);
  m.onBridgedPacket(b.data(), b.size(), 12 * MIN + 59999);         // bridged: not counted
  m.ratesPerMinute(r);
  CHECK(r[59] == 2 && r[58] == 0 && r[57] == 3);
  m.tick(13 * MIN);
  m.ratesPerMinute(r);
  CHECK(r[59] == 0 && r[58] == 2 && r[56] == 3);
  m.tick(69 * MIN);                       // minute 10 is exactly 59 minutes old: still in window
  m.ratesPerMinute(r);
  CHECK(r[0] == 3 && r[2] == 2 && r[59] == 0);
  m.tick(70 * MIN);                       // 60 minutes: rolled out
  m.ratesPerMinute(r);
  CHECK(r[1] == 2 && total(r) == 2);
  obsAck(m, 70 * MIN + 5, 5);             // same slot index as minute 10: old count gone
  m.ratesPerMinute(r);
  CHECK(r[59] == 1);
  m.tick(500 * MIN);                      // long silence clears everything
  m.ratesPerMinute(r);
  CHECK(total(r) == 0);
}

static void rates_survive_m5_reboot() {
  MeshModel m;
  const uint32_t MIN = 60000;
  uint16_t r[60];
  obsAck(m, 5 * MIN, 900000000u);          // M5 up for 10 days
  obsAck(m, 6 * MIN, 900060000u);
  obsAck(m, 7 * MIN, 1000u);               // M5 rebooted: rx_ms jumps backwards
  obsAck(m, 7 * MIN + 10, 2000u);
  obsAck(m, 8 * MIN, 0xFFFFFFF0u);         // and nonsense after that
  m.ratesPerMinute(r);
  CHECK(r[59] == 1 && r[58] == 2 && r[57] == 1 && r[56] == 1 && total(r) == 5);
  CHECK(m.feed(0) && m.feed(0)->ms == 8 * MIN && m.feed(0)->m5_rx_ms == 0xFFFFFFF0u);
}

static void rates_millis_wrap() {
  MeshModel m;
  const uint32_t MIN = 60000;
  uint16_t r[60];
  const uint32_t base = 0xFFFFFFFFu - 90000u;  // 1.5 minutes before the head's millis wraps
  obsAck(m, base, 1);
  obsAck(m, base + 60000u, 2);
  obsAck(m, base + 120000u, 3);               // wrapped: 0x0000752F-ish
  obsAck(m, base + 120000u + 10u, 4);
  CHECK(base + 120000u < base);
  m.ratesPerMinute(r);
  CHECK(r[59] == 2 && r[58] == 1 && r[57] == 1 && total(r) == 4);
  m.tick(base + 120000u + 59u * MIN);         // window keeps rolling after the wrap
  m.ratesPerMinute(r);
  CHECK(r[0] == 2 && total(r) == 2);
  // last_heard stays in raw head millis (wrapped value).
  obs(m, repeaterAdvert(0x10, 1, 0, "W"), 5u, 0, -100);
  CHECK(byName(m, "W") && byName(m, "W")->last_heard_ms == 5u);
}

static void distance_bearing() {
  // Seattle (47.6062, -122.3321) -> Portland (45.5152, -122.6784)
  double d = distanceKm(47606200, -122332100, 45515200, -122678400);
  double b = bearingDeg(47606200, -122332100, 45515200, -122678400);
  std::printf("seattle->portland %.2f km %.2f deg\n", d, b);
  CHECK(std::fabs(d - 233.7) <= 0.5);
  // The task brief said ~191 deg; the initial great-circle bearing for these coordinates is
  // 186.6 deg (Portland is 0.35 deg of longitude west of Seattle: atan(0.24/2.09) = 6.6 deg
  // west of due south), so that is what is pinned.
  CHECK(std::fabs(b - 186.6) <= 1.0);
  CHECK(distanceKm(1, 2, 1, 2) == 0.0);
  double n = bearingDeg(0, 0, 1000000, 0), e = bearingDeg(0, 0, 0, 1000000);
  CHECK(std::fabs(n) < 1e-9 && std::fabs(e - 90.0) < 1e-9);
  CHECK(bearingDeg(0, 0, -1000000, 0) >= 0 && bearingDeg(0, 0, 0, -1000000) < 360.0);
  CHECK(std::fabs(distanceKm(0, 0, 0, 180000000) - 20015.1) < 1.0);   // half circumference
  CHECK(std::fabs(distanceKm(0, 179900000, 0, -179900000) - 22.24) < 0.1);   // across the date line
}

int main(int argc, char** argv) {
  std::string s = argc > 1 ? argv[1] : "";
  if (s == "advert_creates_node_with_gps") advert_creates_node_with_gps();
  else if (s == "snr_attributed_to_last_hop") snr_attributed_to_last_hop();
  else if (s == "advert_via_recorded") advert_via_recorded();
  else if (s == "one_byte_hash_resolves") one_byte_hash_resolves();
  else if (s == "three_byte_hash_resolves_and_no_false_match") three_byte_hash_resolves_and_no_false_match();
  else if (s == "ambiguous_hash_stays_hash") ambiguous_hash_stays_hash();
  else if (s == "bridged_duplicates_not_double_counted") bridged_duplicates_not_double_counted();
  else if (s == "node_table_evicts_lru") node_table_evicts_lru();
  else if (s == "model_ignores_garbage_packets") model_ignores_garbage_packets();
  else if (s == "geo_missing_repeater_position") geo_missing_repeater_position();
  else if (s == "node_without_gps_not_plotted") node_without_gps_not_plotted();
  else if (s == "rates_per_minute_window") rates_per_minute_window();
  else if (s == "rates_survive_m5_reboot") rates_survive_m5_reboot();
  else if (s == "rates_millis_wrap") rates_millis_wrap();
  else if (s == "distance_bearing") distance_bearing();
  else { std::printf("unknown scenario %s\n", s.c_str()); return 2; }
  if (failures) return 1;
  std::printf("PASS %s\n", s.c_str());
  return 0;
}
'''

# A custom allocator is used and released (PSRAM injection path).
ALLOC_HARNESS = r'''
#include <cstdio>
#include <cstdlib>
#include "MeshModel.h"
static int allocs = 0, frees = 0;
static void* a(size_t n, void* ctx) { (*(int*)ctx)++; allocs++; return std::malloc(n); }
static void r(void* p, void*) { if (p) frees++; std::free(p); }
static void* fail(size_t, void*) { return nullptr; }
int main() {
  int ctx = 0;
  { mesh::head::ModelAllocator al = {a, r, &ctx};
    mesh::head::MeshModel m(8, 8, &al);
    if (!m.ok()) return 1; }
  if (allocs == 0 || allocs != frees || ctx != allocs) return 2;
  { mesh::head::ModelAllocator al = {fail, r, nullptr};
    mesh::head::MeshModel m(8, 8, &al);
    if (m.ok() || m.nodeCount() != 0 || m.feed(0) != nullptr) return 3;
    m.onBridgedPacket((const unsigned char*)"\x11\x00", 2, 1); }
  std::printf("PASS alloc\n");
  return 0;
}
'''


class MeshModelTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('g++') or shutil.which('clang++')
        if compiler is None:
            raise unittest.SkipTest('a host C++17 compiler is required')
        cls.compiler = compiler
        cls.sanitize = [] if os.name == 'nt' else [
            '-fsanitize=address,undefined', '-fno-sanitize-recover=all']
        cls.temp = tempfile.TemporaryDirectory(prefix='mesh-model-')
        work = Path(cls.temp.name)
        cls.exe = cls._build(work, 'model', HARNESS, with_sources=True)
        cls.alloc_exe = cls._build(work, 'alloc', ALLOC_HARNESS, with_sources=True)

    @classmethod
    def _build(cls, work, name, source, with_sources):
        cpp = work / (name + '.cpp')
        cpp.write_text(source, encoding='ascii')
        binary = work / (name + ('.exe' if os.name == 'nt' else ''))
        # Repo headers (MeshCore.h etc.) and host mocks are system includes so -Werror
        # applies to the model and harness, not to pre-existing headers.
        sources = [str(UI / 'MeshModel.cpp'), str(ROOT / 'src' / 'Packet.cpp'),
                   str(ROOT / 'src' / 'helpers' / 'AdvertDataHelpers.cpp')] if with_sources else []
        built = subprocess.run([
            cls.compiler, '-std=c++17', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
            '-Wno-unused-parameter', *cls.sanitize,
            '-I', str(UI), '-isystem', str(ROOT / 'src'), '-isystem', str(ROOT / 'test' / 'mocks'),
            str(cpp), *sources, '-o', str(binary),
        ], capture_output=True, text=True, timeout=180, cwd=str(ROOT / 'test'))
        if built.returncode != 0:
            raise AssertionError(built.stdout + built.stderr)
        return binary

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def run_scenario(self, name):
        ran = subprocess.run([str(self.exe), name], capture_output=True, text=True, timeout=60)
        self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)
        self.assertIn('PASS ' + name, ran.stdout)

    def test_advert_creates_node_with_gps(self):
        self.run_scenario('advert_creates_node_with_gps')

    def test_snr_attributed_to_last_hop(self):
        self.run_scenario('snr_attributed_to_last_hop')

    def test_advert_via_recorded(self):
        self.run_scenario('advert_via_recorded')

    def test_ambiguous_hash_stays_hash(self):
        self.run_scenario('ambiguous_hash_stays_hash')

    def test_one_byte_hash_resolves(self):
        self.run_scenario('one_byte_hash_resolves')

    def test_three_byte_hash_resolves_and_no_false_match(self):
        self.run_scenario('three_byte_hash_resolves_and_no_false_match')

    def test_bridged_duplicates_not_double_counted(self):
        self.run_scenario('bridged_duplicates_not_double_counted')

    def test_rates_survive_m5_reboot(self):
        self.run_scenario('rates_survive_m5_reboot')

    def test_rates_millis_wrap(self):
        self.run_scenario('rates_millis_wrap')

    def test_node_table_evicts_lru(self):
        self.run_scenario('node_table_evicts_lru')

    def test_model_ignores_garbage_packets(self):
        self.run_scenario('model_ignores_garbage_packets')

    def test_geo_missing_repeater_position(self):
        self.run_scenario('geo_missing_repeater_position')

    def test_node_without_gps_not_plotted(self):
        self.run_scenario('node_without_gps_not_plotted')

    def test_rates_per_minute_window(self):
        self.run_scenario('rates_per_minute_window')

    def test_distance_bearing(self):
        self.run_scenario('distance_bearing')

    def test_allocator_injection(self):
        ran = subprocess.run([str(self.alloc_exe)], capture_output=True, text=True, timeout=20)
        self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)
        self.assertIn('PASS alloc', ran.stdout)

    def test_model_is_pure_cxx11(self):
        # The head firmware may build as gnu++11; the model must not need Arduino.
        source = (UI / 'MeshModel.cpp').read_text(encoding='utf-8') + (UI / 'MeshModel.h').read_text(encoding='utf-8')
        self.assertNotIn('Arduino.h', source)
        self.assertNotIn('esp_', source)
        checked = subprocess.run([
            self.compiler, '-std=c++11', '-fsyntax-only', '-Wall', '-Wextra', '-Werror',
            '-I', str(UI), '-isystem', str(ROOT / 'src'), '-isystem', str(ROOT / 'test' / 'mocks'),
            str(UI / 'MeshModel.cpp'),
        ], capture_output=True, text=True, timeout=60)
        self.assertEqual(checked.returncode, 0, checked.stdout + checked.stderr)
        geo_tu = Path(self.temp.name) / 'geo11.cpp'
        geo_tu.write_text('#include "GeoMath.h"\n', encoding='ascii')
        geo = subprocess.run([
            self.compiler, '-std=c++11', '-fsyntax-only', '-Wall', '-Wextra', '-Werror',
            '-I', str(UI), str(geo_tu),
        ], capture_output=True, text=True, timeout=60)
        self.assertEqual(geo.returncode, 0, geo.stdout + geo.stderr)


if __name__ == '__main__':
    unittest.main()
