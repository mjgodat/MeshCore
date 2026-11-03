"""Host tests for the repeater head's contact policy and admin list order
(examples/companion_radio/HeadContactPolicy.h), the heard-companion cache
(HeadHeardPeers.h: DMs from companions that are not contacts), plus source
contracts for how MyMesh / BaseChatMesh / HeadAdmin / HeadCompose use them
(home repeater only auto-added, contacts saved on the user's first DM,
login entry, clear action).
"""
from pathlib import Path
import os
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
POLICY = (ROOT / 'examples/companion_radio/HeadContactPolicy.h').as_posix()
HEARD = (ROOT / 'examples/companion_radio/HeadHeardPeers.h').as_posix()

HARNESS = r'''
#include "@POLICY@"
#include "@HEARD@"
#include <cstdio>
#include <cstring>
using namespace head_policy;
static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); failures++; } } while (0)
int main() {
  // Home = a repeater advert at 0 hops that is not a local import.
  CHECK(isHomeAdvert(kTypeRepeater, 0, false));
  CHECK(!isHomeAdvert(kTypeRepeater, 1, false));
  CHECK(!isHomeAdvert(kTypeRepeater, 0, true));
  CHECK(!isHomeAdvert(kTypeChat, 0, false));
  CHECK(!isHomeAdvert(kTypeRoom, 0, false));

  // Auto-add: only the home repeater. Chat companions, other repeaters,
  // rooms and sensors are never added from an advert (a chat contact is
  // saved when the user first sends it a DM).
  CHECK(shouldAutoAdd(kTypeRepeater, true));
  CHECK(!shouldAutoAdd(kTypeRepeater, false));
  CHECK(!shouldAutoAdd(kTypeRoom, false));
  CHECK(!shouldAutoAdd(kTypeSensor, false));
  CHECK(!shouldAutoAdd(kTypeChat, false));
  CHECK(!shouldAutoAdd(kTypeChat, true));   // home is a repeater advert only

  // Heard companions (not contacts): bounded, LRU, replay-safe; DM senders
  // outlive plain advertisers.
  {
    HeardPeers<4> hp;
    uint8_t k[32];
    auto K = [&](uint8_t a) { for (int i = 0; i < 32; i++) k[i] = (uint8_t)(0x40 + i); k[0] = a; return k; };
    CHECK(hp.count() == 0);
    CHECK(hp.learn(K(1), "Alice", 100));
    CHECK(!hp.learn(K(1), "Mallory", 100));   // same/older advert: replay, ignored
    CHECK(!hp.learn(K(1), "Mallory", 99));
    CHECK(std::strcmp(hp.find(K(1), 32)->name, "Alice") == 0);
    CHECK(hp.learn(K(1), "Alice2", 101));     // newer advert renames
    CHECK(std::strcmp(hp.find(K(1), 6)->name, "Alice2") == 0);
    CHECK(!hp.learn(K(9), "", 5));             // no name: not kept
    CHECK(!hp.learn(nullptr, "x", 5));
    hp.learn(K(2), "Bob", 1);
    hp.learn(K(3), "Cat", 1);
    hp.learn(K(4), "Dan", 1);
    CHECK(hp.count() == 4);
    // Bob sent a DM: kept over plain advertisers when full.
    CHECK(hp.markDm(K(2)));
    CHECK(!hp.markDm(K(7)));                   // unknown sender: nothing
    hp.learn(K(5), "Eve", 1);                  // full: evicts the oldest non-DM (Alice2)
    CHECK(hp.count() == 4);
    CHECK(hp.find(K(1), 32) == nullptr);
    CHECK(hp.find(K(2), 32) != nullptr && hp.find(K(2), 32)->dm);
    hp.learn(K(3), "Cat", 2);                  // heard again: most recent
    hp.learn(K(6), "Fay", 1);                  // evicts Dan (oldest non-DM)
    CHECK(hp.find(K(4), 32) == nullptr && hp.find(K(3), 32) != nullptr);
    // All DM senders: the oldest of them goes.
    HeardPeers<2> d;
    d.learn(K(1), "A", 1); d.markDm(K(1));
    d.learn(K(2), "B", 1); d.markDm(K(2));
    d.learn(K(3), "C", 1);
    CHECK(d.find(K(1), 32) == nullptr && d.find(K(2), 32) != nullptr && d.find(K(3), 32) != nullptr);
    // Slots and DM listing; remove (saved as a contact).
    int dms = 0;
    for (size_t i = 0; i < hp.capacity(); i++) { const auto* e = hp.at(i); if (e && e->dm) dms++; }
    CHECK(dms == 1);
    CHECK(hp.remove(K(2)));
    CHECK(!hp.remove(K(2)));
    CHECK(hp.find(K(2), 32) == nullptr && hp.count() == 3);
    // Prefix lookups: 1..32 bytes; 0 never matches.
    CHECK(hp.find(K(3), 1) != nullptr);
    CHECK(hp.find(K(3), 0) == nullptr);
    // Names are cut to the entry, NUL terminated.
    hp.learn(K(8), "A name that is far too long for thirty-one bytes", 1);
    CHECK(std::strlen(hp.find(K(8), 32)->name) == 31);
  }

  // Eviction: heard repeaters first, chats only as a fallback, never home
  // or favourites.
  CHECK(evictRank(kTypeRepeater, false, false) == 2);
  CHECK(evictRank(kTypeRoom, false, false) == 2);
  CHECK(evictRank(kTypeSensor, false, false) == 2);
  CHECK(evictRank(kTypeChat, false, false) == 1);
  CHECK(evictRank(kTypeRepeater, true, false) == 0);
  CHECK(evictRank(kTypeRepeater, false, true) == 0);
  CHECK(evictRank(kTypeChat, true, false) == 0);

  // Clear heard contacts keeps home, favourites and chats.
  CHECK(clearRemoves(kTypeRepeater, false, false));
  CHECK(clearRemoves(kTypeRoom, false, false));
  CHECK(!clearRemoves(kTypeRepeater, false, true));
  CHECK(!clearRemoves(kTypeRepeater, true, false));
  CHECK(!clearRemoves(kTypeChat, false, false));

  // Prefix match.
  const uint8_t home[6] = {0xbc, 0x2d, 0x31, 0xc5, 0x01, 0x02};
  uint8_t key[32] = {0xbc, 0x2d, 0x31, 0xc5, 0x01, 0x02, 0x99};
  CHECK(prefixMatches(key, home, 6));
  key[5] = 0;
  CHECK(!prefixMatches(key, home, 6));
  CHECK(!prefixMatches(key, nullptr, 6));

  // Order: home first even if heard long ago, then newest first; the
  // home repeater sitting at table index 20 (the owner's case) leads.
  struct C { bool home; uint32_t heard; } table[24];
  for (int i = 0; i < 24; i++) table[i] = {false, 1000u + (uint32_t)i * 10u};
  table[20] = {true, 5};
  table[3].heard = 99999;
  uint16_t idx[24];
  for (int i = 0; i < 24; i++) idx[i] = (uint16_t)i;
  sortRepeaters(idx, 24, [&](uint16_t i) { return RepeaterKey{table[i].home, table[i].heard}; });
  CHECK(idx[0] == 20);
  CHECK(idx[1] == 3);
  CHECK(idx[2] == 23);
  CHECK(idx[23] == 0);
  for (int i = 2; i < 24; i++) CHECK(table[idx[i - 1]].heard >= table[idx[i]].heard || i == 1);
  // Ties keep table order.
  uint16_t tie[3] = {2, 0, 1};
  sortRepeaters(tie, 3, [](uint16_t) { return RepeaterKey{false, 7}; });
  CHECK(tie[0] == 0 && tie[1] == 1 && tie[2] == 2);

  // Scrolling keeps the selection visible.
  CHECK(scrollToShow(0, 0, 7, 3) == 0);
  CHECK(scrollToShow(10, 0, 7, 20) == 4);
  CHECK(scrollToShow(2, 5, 7, 20) == 2);
  CHECK(scrollToShow(19, 0, 7, 20) == 13);
  CHECK(scrollToShow(5, 18, 7, 20) == 5);

  // Display names: emoji dropped, emoji-only -> type + key prefix.
  char nm[40];
  const uint8_t pk[4] = {0x57, 0xf7, 0x23, 0x34};
  displayName("\xF0\x9F\xA5\x9E", pk, kTypeRepeater, nm, sizeof(nm));
  CHECK(std::strcmp(nm, "rptr 57f72334") == 0);
  displayName("\xF0\x9F\x90\xB6\xF0\x9F\x90\xB6Woofy Repeater", pk, kTypeRepeater, nm, sizeof(nm));
  CHECK(std::strcmp(nm, "Woofy Repeater") == 0);
  displayName("CID Station \xF0\x9F\x93\xA1", pk, kTypeRepeater, nm, sizeof(nm));
  CHECK(std::strcmp(nm, "CID Station") == 0);
  displayName("CaptCallahan", pk, kTypeRepeater, nm, sizeof(nm));
  CHECK(std::strcmp(nm, "CaptCallahan") == 0);
  displayName("\xF0\x9F\x98\x80 - \xF0\x9F\x98\x80", pk, kTypeChat, nm, sizeof(nm));
  CHECK(std::strcmp(nm, "node 57f72334") == 0);
  displayName("", nullptr, kTypeRoom, nm, sizeof(nm));
  CHECK(std::strcmp(nm, "room") == 0);

  // Login replies.
  CHECK(classifyLoginReply("> login ****\n  Login sent to X (FLOOD, timeout 6000 ms)\n") == LoginReply::Pending);
  CHECK(classifyLoginReply("LOGIN -> CaptCallahan accepted (ACL permissions 0x03, server v2)") == LoginReply::Accepted);
  CHECK(classifyLoginReply("LOGIN -> CaptCallahan rejected") == LoginReply::Failed);
  CHECK(classifyLoginReply("  ERROR: login to X timed out (wrong password or no response).") == LoginReply::Failed);
  CHECK(classifyLoginReply("  ERROR: password must be 1-15 UTF-8 bytes") == LoginReply::Failed);

  if (failures) return 1;
  std::printf("PASS policy\n");
  return 0;
}
'''.replace('@POLICY@', POLICY).replace('@HEARD@', HEARD)


def read(rel):
    return (ROOT / rel).read_bytes().decode('utf-8', 'replace').replace('\r\n', '\n')


def strip_comments(src):
    src = re.sub(r'/\*.*?\*/', '', src, flags=re.S)
    return re.sub(r'//[^\n]*', '', src)


def function_body(src, header_regex):
    m = re.search(header_regex, src)
    assert m, header_regex
    i = src.index('{', m.end() - 1)
    depth = 0
    for j in range(i, len(src)):
        if src[j] == '{':
            depth += 1
        elif src[j] == '}':
            depth -= 1
            if depth == 0:
                return src[i:j + 1]
    raise AssertionError('unbalanced ' + header_regex)


class HeadContactPolicyTest(unittest.TestCase):
    def test_policy_logic(self):
        compiler = shutil.which('g++') or shutil.which('clang++')
        if compiler is None:
            self.skipTest('a host C++17 compiler is required')
        sanitize = [] if os.name == 'nt' else [
            '-fsanitize=address,undefined', '-fno-sanitize-recover=all']
        with tempfile.TemporaryDirectory(prefix='head-policy-') as directory:
            work = Path(directory)
            cpp = work / 'policy.cpp'
            cpp.write_text(HARNESS, encoding='ascii')
            binary = work / ('policy' + ('.exe' if os.name == 'nt' else ''))
            built = subprocess.run([
                compiler, '-std=c++17', '-O1', '-g', '-Wall', '-Wextra', '-Werror', *sanitize,
                str(cpp), '-o', str(binary)], capture_output=True, text=True, timeout=120)
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
            ran = subprocess.run([str(binary)], capture_output=True, text=True, timeout=20)
            self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)
            self.assertIn('PASS policy', ran.stdout)

    def test_mesh_uses_policy_on_heads(self):
        mesh = strip_comments(read('examples/companion_radio/MyMesh.cpp'))
        auto = function_body(mesh, r'bool MyMesh::shouldAutoAddContactType\(')
        self.assertIn('head_policy::shouldAutoAdd', auto)
        over = function_body(mesh, r'bool MyMesh::shouldOverwriteWhenFull\(')
        self.assertIn('_head_advert_home', over)
        recv = function_body(mesh, r'void MyMesh::onAdvertRecv\(')
        self.assertIn('head_policy::isHomeAdvert', recv)
        self.assertIn('getPathHashCount()', recv)
        # The flag never outlives the advert it describes.
        self.assertRegex(recv, r'_head_advert_home = false;\s*#endif\s*\}?\s*$|_head_advert_home = false;')
        rank = function_body(mesh, r'int MyMesh::overwriteRank\(')
        self.assertIn('head_policy::evictRank', rank)
        clear = function_body(mesh, r'int MyMesh::headClearHeardContacts\(')
        self.assertIn('head_policy::clearRemoves', clear)
        self.assertIn('deleteStoredContact', clear)
        base = strip_comments(read('src/helpers/BaseChatMesh.cpp'))
        # (allocateContactSlot has #if'd braces; check the text after it.)
        alloc = base[base.index('ContactInfo* BaseChatMesh::allocateContactSlot('):]
        alloc = alloc[:alloc.index('void BaseChatMesh::populateContactFromAdvert')]
        self.assertIn('overwriteRank(contacts[i])', alloc)

    def test_heads_add_chat_contacts_only_on_user_send(self):
        mesh = strip_comments(read('examples/companion_radio/MyMesh.cpp'))
        auto = function_body(mesh, r'bool MyMesh::shouldAutoAddContactType\(')
        head = auto[:auto.index('#endif')]
        # Heads: only the home repeater; the chat auto-add settings play no part.
        self.assertIn('head_policy::shouldAutoAdd(contact_type, _head_advert_home)', head)
        self.assertNotIn('AUTO_ADD_CHAT', head)
        # A chat advert that was not added is remembered (not a contact) so a
        # DM from it can still be decrypted, shown and answered.
        disc = function_body(mesh, r'void MyMesh::onDiscoveredContact\(')
        self.assertIn('_head_heard.learn(', disc)
        self.assertIn('lookupContactByPubKey(contact.id.pub_key', disc)
        extra = function_body(mesh, r'ContactInfo\* MyMesh::extraPeer\(')
        self.assertIn('_head_heard.at(', extra)
        self.assertIn('OUT_PATH_UNKNOWN', extra)
        # A DM from a heard companion: shown, never persisted as a contact.
        recv = function_body(mesh, r'void MyMesh::onMessageRecv\(')
        self.assertIn('isHeadHeardPeer(from)', recv)
        self.assertIn('_head_heard.markDm(', recv)
        self.assertRegex(recv, r'if \(!isHeadHeardPeer\(from\)\)\s*scheduleContactWrite\(from\)')
        # Saved only by the user's send: added, persisted, dropped from the cache.
        save = function_body(mesh, r'ContactInfo\* MyMesh::headSaveHeardPeer\(')
        self.assertIn('addContact(', save)
        self.assertIn('scheduleContactWrite(', save)
        self.assertIn('_head_heard.remove(', save)
        # BaseChatMesh: extra peers are searched after contacts and accept
        # plain text only.
        base = strip_comments(read('src/helpers/BaseChatMesh.cpp'))
        search = function_body(base, r'int BaseChatMesh::searchPeersByHash\(')
        self.assertIn('extraPeer(', search)
        self.assertIn('kExtraPeerBase + k', search)
        secret = function_body(base, r'void BaseChatMesh::getPeerSharedSecret\(')
        self.assertIn('kExtraPeerBase', secret)
        data = function_body(base, r'void BaseChatMesh::onPeerDataRecv\(')
        self.assertIn('kExtraPeerBase', data)
        self.assertIn('TXT_TYPE_PLAIN', data[:data.index('ContactInfo& from')])
        self.assertIn('PAYLOAD_TYPE_TXT_MSG', data[:data.index('ContactInfo& from')])
        hdr = strip_comments(read('src/helpers/BaseChatMesh.h'))
        self.assertIn('virtual ContactInfo* extraPeer(int k) { return nullptr; }', hdr)
        # Compose (CYD + Freenove): DM senders that are not contacts are
        # destinations; sending to one saves it first.
        compose = strip_comments(read('examples/companion_radio/ui-new/HeadCompose.h'))
        self.assertIn('headHeardDmAt(', function_body(compose, r'void open\(\)'))
        send = function_body(compose, r'void send\(\)')
        self.assertIn('headSaveHeardPeer(', send)
        self.assertIn('headHeardName(', function_body(compose, r'static bool destIsPeer\('))

    def test_clock_floor_never_goes_back_after_reboot(self):
        # The last session may have stamped logins up to one save interval
        # after the saved floor: boot starts past it (else the repeater
        # drops the first login after a reboot as a replay).
        clock = strip_comments(read('examples/companion_radio/ui-new/HeadClock.h'))
        begin = function_body(clock, r'void begin\(\)')
        self.assertIn('setCurrentTime(_saved + kSaveIntervalSecs)', begin)
        self.assertRegex(clock, r'kSaveIntervalSecs = 60;')

    def test_head_names_tiles_and_recent_page(self):
        ui = strip_comments(read('examples/companion_radio/ui-new/UITask.cpp'))
        # Recently-heard page: drawable names, and an empty state (it was blank).
        self.assertIn('head_policy::displayName(a->name, a->pubkey_prefix', ui)
        self.assertIn('if (shown_rows == 0)', ui)
        admin = strip_comments(read('examples/companion_radio/ui-new/HeadAdmin.h'))
        self.assertNotIn('asciiName', admin)
        self.assertIn('head_policy::displayName(c.name, c.id.pub_key, c.type, _sel.name', admin)
        task = strip_comments(read('examples/companion_radio/ui-freenove/FreenoveUITask.cpp'))
        # PRESET tile names persist; ADMIN tile follows the console login.
        self.assertIn('loadPresetNames();', task)
        self.assertIn('prefs.putBytes("names"', task)
        self.assertIn('s_adminLoggedIn.store(ui_task.adminLoggedIn()', task)
        self.assertIn('_ctx.admin_logged_in = loggedIn;', task)

    def test_admin_login_always_opens_entry(self):
        admin = strip_comments(read('examples/companion_radio/ui-new/HeadAdmin.h'))
        # Header Login tap and HOME's ADMIN tile open the entry, prefilled.
        self.assertIn('openLoginEntry()', function_body(admin, r'void loginSaved\('))
        entry = function_body(admin, r'void openLoginEntry\(')
        self.assertIn('loadPassword', entry)
        self.assertIn('_keyboard.reset(', entry)
        self.assertIn('Mode::Password', entry)
        # Saved only on success, under the repeater the login went to.
        poll = function_body(admin, r'void poll\(\) override')
        self.assertIn('classifyLoginReply', poll)
        self.assertIn('savePasswordFor(_login_prefix', poll)
        send = function_body(admin, r'void sendLogin\(')
        self.assertNotIn('savePassword', send)
        # The list is built from the whole contact table, not the first 8.
        self.assertNotIn('kMaxRepeaters = 8', admin)
        self.assertIn('sortRepeaters', admin)
        self.assertIn('isHeadHomeRepeater', admin)
        # Compose "Send as": the home repeater first, not the first 7.
        compose = strip_comments(read('examples/companion_radio/ui-new/HeadCompose.h'))
        self.assertIn('isHeadHomeRepeater', function_body(compose, r'void open\(\)'))
        # The press that closes compose/admin must not tap the home page
        # underneath (on-device: compose X re-opened compose).
        uitask = read('examples/companion_radio/ui-new/UITask.cpp')
        self.assertIn('_raw_touch_hold = true;', uitask)
        self.assertIn('if (_raw_touch_hold) {', uitask)
        # Clear heard contacts asks first.
        self.assertIn('Mode::ClearAsk', admin)
        self.assertIn('headClearHeardContacts', function_body(admin, r'bool touchClearAsk\('))


if __name__ == '__main__':
    unittest.main()
