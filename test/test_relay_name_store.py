"""Host tests for the head's persistent relay-name store
(examples/companion_radio/ui-freenove/RelayNameStore.h): bounds, LRU,
write throttle, unique resolution of short hashes, and the blob that is
kept in NVS and loaded at boot. Plus source contracts for how the visual
head wires it up (model learns, page task loads at boot and saves
throttled).
"""
from pathlib import Path
import os
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
UI = ROOT / 'examples' / 'companion_radio' / 'ui-freenove'
STORE = (UI / 'RelayNameStore.h').as_posix()

HARNESS = r'''
#include "@STORE@"
#include <cstdio>
#include <cstring>
#include <vector>
using mesh::head::RelayNameStore;
static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); failures++; } } while (0)

static void K(uint8_t k[4], uint8_t a, uint8_t b, uint8_t c, uint8_t d) { k[0] = a; k[1] = b; k[2] = c; k[3] = d; }
static const char* nameOf(const RelayNameStore& s, uint8_t a, uint8_t b = 0, uint8_t c = 0, uint8_t size = 1) {
  const uint8_t h[3] = {a, b, c};
  const char* n = nullptr;
  return s.matches(h, size, nullptr, &n) == 1 ? n : nullptr;
}

int main() {
  RelayNameStore s;
  s.begin(0);
  uint8_t k[4];
  CHECK(s.count() == 0);
  CHECK(!s.saveDue(1000000));   // nothing learned: never writes

  // learn: new and renamed are changes; the same name again is not.
  K(k, 0x7E, 0x76, 0x62, 0x01);
  CHECK(s.learn(k, "Downtown"));
  CHECK(!s.learn(k, "Downtown"));
  CHECK(s.learn(k, "Downtown 2"));
  CHECK(s.count() == 1);
  CHECK(std::strcmp(s.nameFor(k), "Downtown 2") == 0);
  CHECK(!s.learn(k, ""));        // empty names are never stored
  CHECK(!s.learn(k, nullptr));
  // Long names are cut to the entry size, NUL terminated.
  K(k, 0x10, 0, 0, 1);
  CHECK(s.learn(k, "A very long repeater name that goes on"));
  CHECK(std::strlen(s.nameFor(k)) == RelayNameStore::kNameLen - 1);

  // Resolution on 1..3-byte hashes; unknown -> none.
  CHECK(nameOf(s, 0x7E) != nullptr && std::strcmp(nameOf(s, 0x7E), "Downtown 2") == 0);
  CHECK(std::strcmp(nameOf(s, 0x7E, 0x76, 0, 2), "Downtown 2") == 0);
  CHECK(std::strcmp(nameOf(s, 0x7E, 0x76, 0x62, 3), "Downtown 2") == 0);
  CHECK(nameOf(s, 0x7E, 0x76, 0x63, 3) == nullptr);
  CHECK(nameOf(s, 0x99) == nullptr);
  // Ambiguous: two stored relays share the 1-byte hash -> no name; the
  // longer hash still separates them.
  K(k, 0x7E, 0x11, 0x00, 0x02);
  CHECK(s.learn(k, "Other7E"));
  CHECK(nameOf(s, 0x7E) == nullptr);
  {
    const uint8_t h[1] = {0x7E};
    const char* n = nullptr;
    CHECK(s.matches(h, 1, nullptr, &n) == 2);
    // A key known elsewhere (live model / the M5) is not counted twice.
    uint8_t live[4]; K(live, 0x7E, 0x11, 0x00, 0x02);
    CHECK(s.matches(h, 1, live, &n) == 1 && std::strcmp(n, "Downtown 2") == 0);
  }
  CHECK(std::strcmp(nameOf(s, 0x7E, 0x76, 0, 2), "Downtown 2") == 0);
  CHECK(std::strcmp(nameOf(s, 0x7E, 0x11, 0, 2), "Other7E") == 0);
  // Invalid sizes never match.
  {
    const uint8_t h[4] = {0x7E, 0x76, 0x62, 0x01};
    CHECK(s.matches(h, 0, nullptr, nullptr) == 0);
    CHECK(s.matches(h, 4, nullptr, nullptr) == 0);
    CHECK(s.matches(nullptr, 1, nullptr, nullptr) == 0);
  }

  // Bounds + LRU: capacity 64; the least recently learned/heard goes.
  RelayNameStore b;
  b.begin(0);
  char nm[16];
  for (int i = 0; i < 64; i++) {
    K(k, (uint8_t)i, 0xA0, 0, 0);
    std::snprintf(nm, sizeof(nm), "R%d", i);
    CHECK(b.learn(k, nm));
  }
  CHECK(b.count() == RelayNameStore::kCapacity && RelayNameStore::kCapacity == 64);
  K(k, 0, 0xA0, 0, 0);
  CHECK(!b.learn(k, "R0"));       // heard again: now most recent (no change)
  K(k, 200, 0xA0, 0, 0);
  CHECK(b.learn(k, "R200"));      // full: evicts R1 (oldest), not R0
  CHECK(b.count() == 64);
  K(k, 1, 0xA0, 0, 0);
  CHECK(b.nameFor(k) == nullptr);
  K(k, 0, 0xA0, 0, 0);
  CHECK(b.nameFor(k) != nullptr);
  K(k, 200, 0xA0, 0, 0);
  CHECK(b.nameFor(k) != nullptr);
  for (int i = 0; i < 300; i++) {   // many more: never above capacity
    K(k, (uint8_t)i, 0xB0, (uint8_t)(i >> 8), 0);
    b.learn(k, "x");
  }
  CHECK(b.count() == 64);

  // Throttle: dirty only on change; at most one write per kSaveIntervalMs,
  // measured from begin() (boot) and from the last save.
  RelayNameStore t;
  t.begin(5000);
  K(k, 1, 2, 3, 4);
  t.learn(k, "One");
  CHECK(!t.saveDue(5000));
  CHECK(!t.saveDue(5000 + 59999));
  CHECK(t.saveDue(5000 + 60000));
  t.markSaved(65000);
  CHECK(!t.saveDue(500000));      // nothing new since: no write
  CHECK(!t.learn(k, "One"));
  CHECK(!t.saveDue(500000));      // same name heard again: still no write
  K(k, 9, 9, 9, 9);
  t.learn(k, "Two");
  CHECK(!t.saveDue(65000 + 1000));
  CHECK(t.saveDue(65000 + 60000));
  // A failed write keeps the change but waits a whole interval to retry.
  t.markSaved(125000, false);
  CHECK(!t.saveDue(125000 + 59999));
  CHECK(t.saveDue(125000 + 60000));
  t.markSaved(185000);
  CHECK(!t.saveDue(185000 + 600000));
  // Millis wrap: the interval is taken modulo 2^32.
  RelayNameStore w;
  w.begin(0xFFFFF000u);
  w.learn(k, "Wrap");
  CHECK(!w.saveDue(0x00001000u));
  CHECK(w.saveDue(0xFFFFF000u + 60000u));

  // Blob round trip = load at boot: same names, same LRU order.
  RelayNameStore a;
  a.begin(0);
  K(k, 0x7E, 0x76, 0x62, 0x01); a.learn(k, "Downtown");
  K(k, 0x22, 0x00, 0x00, 0x01); a.learn(k, "Second");
  K(k, 0x7E, 0x76, 0x62, 0x01); a.learn(k, "Downtown");   // most recent again
  std::vector<uint8_t> blob(RelayNameStore::kBlobMax);
  const size_t len = a.serialize(blob.data(), blob.size());
  CHECK(len == RelayNameStore::kBlobHeader + 2 * RelayNameStore::kBlobEntry);
  CHECK(a.serialize(blob.data(), len - 1) == 0);   // too small: nothing
  RelayNameStore boot;
  CHECK(boot.load(blob.data(), len));
  boot.begin(0);
  CHECK(boot.count() == 2);
  CHECK(!boot.saveDue(1000000));   // loading is not a change
  CHECK(std::strcmp(nameOf(boot, 0x7E, 0x76, 0x62, 3), "Downtown") == 0);
  CHECK(std::strcmp(nameOf(boot, 0x22), "Second") == 0);
  // LRU order survived: fill up, "Second" (older) goes before "Downtown".
  for (int i = 0; i < 63; i++) { K(k, (uint8_t)(0x80 + i), 0xC0, 0, 0); boot.learn(k, "f"); }
  CHECK(boot.count() == 64);
  CHECK(nameOf(boot, 0x22) == nullptr);
  CHECK(nameOf(boot, 0x7E, 0x76, 0, 2) != nullptr);

  // Bad blobs load nothing (and leave the store empty).
  RelayNameStore bad;
  K(k, 5, 5, 5, 5); bad.learn(k, "keep?");
  std::vector<uint8_t> junk(blob.begin(), blob.begin() + len);
  junk[0] ^= 0xFF;   // magic
  CHECK(!bad.load(junk.data(), junk.size()));
  CHECK(bad.count() == 0);
  junk.assign(blob.begin(), blob.begin() + len);
  CHECK(!bad.load(junk.data(), junk.size() - 1));   // length mismatch
  junk[3] = 200;     // count above capacity
  CHECK(!bad.load(junk.data(), junk.size()));
  CHECK(!bad.load(nullptr, 0));
  // An unterminated / empty name inside a good blob is dropped, not trusted.
  junk.assign(blob.begin(), blob.begin() + len);
  std::memset(&junk[RelayNameStore::kBlobHeader + 4], 'Z', RelayNameStore::kNameLen);
  CHECK(bad.load(junk.data(), junk.size()));
  CHECK(bad.count() == 2);
  {
    uint8_t k2[4]; K(k2, 0x22, 0, 0, 1);
    CHECK(std::strlen(bad.nameFor(k2)) == RelayNameStore::kNameLen - 1);
  }
  junk.assign(blob.begin(), blob.begin() + len);
  junk[RelayNameStore::kBlobHeader + 4] = 0;   // empty name
  CHECK(bad.load(junk.data(), junk.size()));
  CHECK(bad.count() == 1);
  // Duplicate keys in a blob keep one entry.
  junk.assign(blob.begin(), blob.begin() + len);
  std::memcpy(&junk[RelayNameStore::kBlobHeader + RelayNameStore::kBlobEntry], &junk[RelayNameStore::kBlobHeader], 4);
  CHECK(bad.load(junk.data(), junk.size()));
  CHECK(bad.count() == 1);
  // Empty store: header only, loads back empty.
  RelayNameStore e;
  const size_t elen = e.serialize(blob.data(), blob.size());
  CHECK(elen == RelayNameStore::kBlobHeader);
  CHECK(e.load(blob.data(), elen) && e.count() == 0);
  // Full store fits the blob bound.
  CHECK(b.serialize(blob.data(), blob.size()) == RelayNameStore::kBlobMax);

  if (failures) return 1;
  std::printf("PASS store\n");
  return 0;
}
'''.replace('@STORE@', STORE)


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


class RelayNameStoreTest(unittest.TestCase):
    def test_store_logic(self):
        compiler = shutil.which('g++') or shutil.which('clang++')
        if compiler is None:
            self.skipTest('a host C++17 compiler is required')
        sanitize = [] if os.name == 'nt' else [
            '-fsanitize=address,undefined', '-fno-sanitize-recover=all']
        with tempfile.TemporaryDirectory(prefix='relay-names-') as directory:
            work = Path(directory)
            cpp = work / 'store.cpp'
            cpp.write_text(HARNESS, encoding='ascii')
            binary = work / ('store' + ('.exe' if os.name == 'nt' else ''))
            built = subprocess.run([
                compiler, '-std=c++17', '-O1', '-g', '-Wall', '-Wextra', '-Werror', *sanitize,
                str(cpp), '-o', str(binary)], capture_output=True, text=True, timeout=120)
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
            ran = subprocess.run([str(binary)], capture_output=True, text=True, timeout=20)
            self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)
            self.assertIn('PASS store', ran.stdout)

    def test_head_wires_store(self):
        # The model feeds every repeater advert name into the store.
        model = strip_comments(read('examples/companion_radio/ui-freenove/MeshModel.cpp'))
        self.assertIn('_names->learn(', model)
        # Names resolve through the store, after the live model.
        widgets = strip_comments(read('examples/companion_radio/ui-freenove/Widgets.h'))
        self.assertIn('nameStore()', function_body(widgets, r'inline void relayName\('))
        # The page task loads the store at boot and writes it only when due.
        task = strip_comments(read('examples/companion_radio/ui-freenove/FreenoveUITask.cpp'))
        begin = function_body(task, r'bool FreenoveUITask::begin\(')
        self.assertIn('loadRelayNames(', begin)
        self.assertIn('setNameStore(', begin)
        self.assertLess(begin.index('loadRelayNames('), begin.index('setNameStore('))
        save = function_body(task, r'void FreenoveUITask::saveRelayNames\(')
        self.assertIn('saveDue(', save)
        self.assertIn('markSaved(', save)
        self.assertIn('putBytes(', save)
        run = function_body(task, r'void FreenoveUITask::run\(')
        self.assertIn('saveRelayNames(', run)


if __name__ == '__main__':
    unittest.main()
