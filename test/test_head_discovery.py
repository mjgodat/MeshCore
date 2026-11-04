"""Visual head side of repeater discovery (HeadDiscovery.h, HeadLinkRadio).

- A verified announce teaches the head the repeater's UDP address while
  udp.peer is off; a manual udp.peer wins; the learned address sticks to one
  repeater; replies are rate limited and repeated at once after a repeater
  reboot or an address change; the first announced repeater is offered as
  home once.
- An end-to-end exchange with the real repeater-side codec and the HMAC the
  devices use: the head's reply passes the repeater's tag and freshness
  checks and lands in its auto-peer table.
- `get transport` shows the announce; udp needs no udp.peer any more.
- Source checks of the ESP32 glue (HeadLinkRadio, the Link page, home).
"""
from pathlib import Path
import unittest

from test_espnow_repeat import build_and_run

ROOT = Path(__file__).resolve().parents[1]

CHECKS = r'''
#include <cassert>
#include <cstdio>
#include <cstring>
#include <SHA256.h>
#include <helpers/HeadDiscovery.h>
#include <helpers/bridges/DiscoveryKey.h>
#include <helpers/HeadTransport.h>
using namespace mesh::discovery;
using mesh::headlink::HeadDiscovery;
using mesh::bridge::udp::Peer;
// The devices' tag: HMAC with the PBKDF2-derived key (DiscoveryKey.h).
static KeyCache keys;
static uint8_t* key_for(const char* secret) { return const_cast<uint8_t*>(keys.get(secret)); }
static Mac mac = keyedMac;
static Announce announce(uint8_t id, uint8_t ip_last, uint32_t boot = 0x1234, uint32_t counter = 1) {
  Announce a; a.boot = boot; a.counter = counter;
  for (int i = 0; i < 6; ++i) a.prefix[i] = uint8_t(id + i);
  snprintf(a.name, sizeof(a.name), "Rpt%u", id);
  a.espnow_channel = 6; a.espnow_format = 1; a.caps = CAP_ESPNOW | CAP_UDP | CAP_AUTOPAIR;
  a.ip[0] = 192; a.ip[1] = 168; a.ip[2] = 1; a.ip[3] = ip_last; a.port = 4404;
  return a;
}
int main() {
  HeadDiscovery d; uint32_t now = 1000;
  const uint8_t local[4] = {192, 168, 1, 77};
  assert(!d.seen() && !d.learnedPeer().enabled() && !d.shouldReply(now, local, 4404));
  // learn the address from the first verified announce
  assert(d.noteAnnounce(announce(1, 50), true, now) == HeadDiscovery::PeerChanged);
  assert(d.learnedPeer().enabled() && d.learnedPeer().ip[3] == 50 && d.learnedPeer().port == 4404);
  assert(d.noteAnnounce(announce(1, 50, 0x1234, 2), false, now + 5000) == HeadDiscovery::Kept);   // same: unchanged
  // manual udp.peer wins
  Peer manual; manual.ip[0] = 10; manual.ip[1] = 0; manual.ip[2] = 0; manual.ip[3] = 9; manual.port = 4500;
  assert(HeadDiscovery::effectivePeer(manual, d.learnedPeer()).port == 4500);
  assert(HeadDiscovery::effectivePeer(Peer(), d.learnedPeer()).ip[3] == 50);
  // another repeater does not move it while the first is heard
  assert(d.noteAnnounce(announce(9, 99), true, now + 6000) == HeadDiscovery::Ignored && d.learnedPeer().ip[3] == 50 && d.ignored() == 1);
  // the same repeater moving its address does
  assert(d.noteAnnounce(announce(1, 51, 0x1234, 3), true, now + 7000) == HeadDiscovery::PeerChanged && d.learnedPeer().ip[3] == 51);
  // ... and another one after two minutes of silence
  assert(d.noteAnnounce(announce(9, 99), true, now + 7000 + HeadDiscovery::kSwitchAfterMs) == HeadDiscovery::PeerChanged && d.learnedPeer().ip[3] == 99);
  // an announce without UDP clears the learned address
  Announce no_udp = announce(9, 99, 0x1234, 2); no_udp.caps = CAP_ESPNOW; no_udp.port = 0;
  assert(d.noteAnnounce(no_udp, true, now + 130000) == HeadDiscovery::PeerChanged && !d.learnedPeer().enabled());

  // review F7: replays and stale boots
  {
    HeadDiscovery r; uint32_t t = 1000;
    assert(r.noteAnnounce(announce(1, 50, 0xAAAA, 10), true, t) == HeadDiscovery::PeerChanged);
    assert(r.noteAnnounce(announce(1, 50, 0xAAAA, 10), true, t + 1) == HeadDiscovery::Ignored);   // same counter
    assert(r.noteAnnounce(announce(1, 60, 0xAAAA, 9), true, t + 2) == HeadDiscovery::Ignored);    // older
    assert(r.learnedPeer().ip[3] == 50 && r.replays() == 2);
    // an old boot's announce, replayed while the current boot is heard: refused
    assert(r.noteAnnounce(announce(1, 60, 0x5555, 99), true, t + 1000) == HeadDiscovery::Ignored);
    assert(r.noteAnnounce(announce(1, 50, 0xAAAA, 11), true, t + 5000) == HeadDiscovery::Kept);
    // a real reboot: the old boot falls silent; the new one is taken after 15 s
    assert(r.noteAnnounce(announce(1, 51, 0xBBBB, 1), true, t + 5000 + 14999) == HeadDiscovery::Ignored);
    assert(r.noteAnnounce(announce(1, 51, 0xBBBB, 2), true, t + 5000 + HeadDiscovery::kNewBootAfterMs) == HeadDiscovery::PeerChanged);
    assert(r.last().boot == 0xBBBB && r.learnedPeer().ip[3] == 51);
  }
  // home: the first announced repeater, offered once
  uint8_t home[6];
  assert(d.takeHome(home) && home[0] == 1 && !d.takeHome(home));

  // replies: needs autopair, a usable local address; then at most every 30 s,
  // at once when the repeater rebooted or the head's address changed
  HeadDiscovery h; now = 50000;
  h.noteAnnounce(announce(1, 50), true, now);
  const uint8_t none[4] = {0, 0, 0, 0};
  assert(!h.shouldReply(now, none, 4404) && !h.shouldReply(now, local, 0));
  assert(h.shouldReply(now, local, 4404));
  h.noteReplied(now, local, 4404);
  assert(!h.shouldReply(now + 29999, local, 4404) && h.shouldReply(now + 30000, local, 4404));
  const uint8_t moved[4] = {192, 168, 1, 78};
  assert(h.shouldReply(now + 1000, moved, 4404) && h.shouldReply(now + 1000, local, 4405));
  // review F7: never more than one reply a second, whatever changed
  h.noteReplied(now + 30000, local, 4404);
  assert(!h.shouldReply(now + 30000 + 999, moved, 4404) && h.shouldReply(now + 30000 + 1000, moved, 4404));
  h.noteAnnounce(announce(1, 50, 0x9999, 1), true, now + 30000 + HeadDiscovery::kNewBootAfterMs);   // repeater rebooted
  assert(h.shouldReply(now + 30000 + HeadDiscovery::kNewBootAfterMs + 1, local, 4404));
  Announce no_pair = announce(1, 50); no_pair.caps &= uint8_t(~CAP_AUTOPAIR);
  HeadDiscovery np; np.noteAnnounce(no_pair, true, now); assert(!np.shouldReply(now, local, 4404));

  // end to end: the head's signed reply passes the repeater's checks
  Announce a = announce(1, 50, 0xABCD, 40);
  uint8_t f[MAX_FRAME];
  const size_t an = encodeAnnounce(a, mac, key_for("fleet-test"), f, sizeof(f));
  Announce heard; assert(an && decodeAnnounce(f, an, mac, key_for("fleet-test"), heard));
  HeadDiscovery e; e.noteAnnounce(heard, false, 1000);
  const uint8_t self[6] = {0xC1, 0xC2, 0xC3, 0xC4, 0xC5, 0xC6};
  const Reply r = e.reply(self, "CYD head with a long name", local, 4404);
  const size_t rn = encodeReply(r, mac, key_for("fleet-test"), f, sizeof(f));
  Reply got; assert(rn == REPLY_FRAME && decodeReply(f, rn, mac, key_for("fleet-test"), got));
  assert(freshReply(got, 0xABCD, 40) && freshReply(got, 0xABCD, 51) && !freshReply(got, 0xABCD, 52));
  assert(!strcmp(got.name, "CYD head with a ") && got.flags == 0);   // review F8: reserved
  AutoPeers table; assert(table.offer(got, 1000) == PairResult::Added && table.slot(0).ip[3] == 77);
  Reply bad; assert(!decodeReply(f, rn, mac, key_for("other"), bad));

  // get transport shows the announce (and the learned address while udp.peer is off)
  using namespace mesh::headlink;
  Config c; Status s; s.espnow_channel = 6; char out[200];
  s.announce_seen = true; strcpy(s.announce_name, "Rpt1"); s.announce_channel = 6; s.announce_format = 1;
  s.announce_age_s = 3; s.peer_auto = true; s.peer_ip[0] = 192; s.peer_ip[1] = 168; s.peer_ip[2] = 1; s.peer_ip[3] = 50;
  s.peer_port = 4404;
  formatStatus(c, s, out, sizeof(out));
  assert(!strcmp(out, "> espnow; ch 6 vitals -; repeater Rpt1 ch 6 wrapped 3s, udp.peer auto 192.168.1.50:4404"));
  c.repeater = manual;
  formatStatus(c, s, out, sizeof(out));
  assert(!strcmp(out, "> espnow; ch 6 vitals -; repeater Rpt1 ch 6 wrapped 3s"));
  char tiny[12]; formatStatus(c, s, tiny, sizeof(tiny)); assert(strlen(tiny) < sizeof(tiny));
  // udp no longer needs udp.peer
  Config u; strcpy(u.ssid, "Net"); strcpy(u.secret, "s"); assert(udpConfigured(u));
  char reply[160];
  Config m; strcpy(m.ssid, "Net");
  assert(handleCommand(m, "set transport udp", reply, sizeof(reply), s) == Result::Changed);
  assert(!strcmp(reply, "OK - transport udp; set udp.ssid and udp.secret"));
  return 0;
}
'''


class HeadDiscoveryTests(unittest.TestCase):
    def test_learning_replies_home_and_status(self):
        build_and_run(self, CHECKS, includes=[ROOT / "test/mocks", ROOT / "src/helpers"], strict=False)

    def test_esp32_glue(self):
        radio = (ROOT / "src/helpers/esp32/HeadLinkRadio.cpp").read_text()
        self.assertIn("ESPNOWRadio::setDiscoveryHandler(onEspNowDiscovery, this);", radio)
        service = radio[radio.index("void HeadLinkRadio::serviceDiscovery()"):]
        service = service[:service.index("\n}\n")]
        # verified before anything is learned or answered; the reply goes to the
        # announced repeater address
        self.assertLess(service.index("decodeAnnounce("), service.index("noteAnnounce("))
        self.assertLess(service.index("noteAnnounce("), service.index("encodeReply("))
        self.assertIn("sendDatagram(frame, n, a.ip, a.port)", service)
        self.assertIn("savedEspNowLinkSecret(", service)
        self.assertIn("keyedMac", service)                         # derived key (DiscoveryKey.h)
        self.assertIn("HeadDiscovery::Ignored", service)          # review F7
        queue = radio[radio.index("void HeadLinkRadio::queueDiscovery("):]
        queue = queue[:queue.index("\n}\n")]
        self.assertIn("drop the oldest", queue)
        header = (ROOT / "src/helpers/esp32/HeadLinkRadio.h").read_text()
        self.assertIn("kPendingAnnounces = 4", header)
        self.assertIn("effectivePeer(_cfg.repeater, _disc.learnedPeer())", radio)
        # the UDP link takes announces from any address, mesh only from the repeater
        self.assertIn("isDiscoveryFrame(bytes, (size_t)n)", radio)
        self.assertIn("SO_BROADCAST", radio)
        companion = (ROOT / "examples/companion_radio/MyMesh.cpp").read_text()
        loop = companion[companion.index("void MyMesh::loop() {"):]
        loop = loop[:loop.index("#endif")]
        self.assertIn("takeAnnouncedHome(home)", loop)
        self.assertIn("if (!_head_home_valid) headNoteHome(home);", loop)
        self.assertIn("setIdentity(self_id.pub_key, _prefs.node_name)", loop)
        link = (ROOT / "examples/companion_radio/ui-new/HeadLink.h").read_text()
        self.assertIn('"Repeater: auto %s"', link)


if __name__ == "__main__":
    unittest.main()
