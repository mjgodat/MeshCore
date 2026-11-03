#include "FreenoveUITask.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <atomic>
#include <new>
#include <string.h>

#include <Preferences.h>
#include <helpers/UsbLogging.h>
#include <helpers/esp32/ESPNOWRadio.h>

#if HMI_CYD
#include "CYDDisplay.h"
#else
#include "FreenoveDisplay.h"
#include "FreenoveGeometry.h"
#endif
#include "HeadRadio.h"
#include "RadioLabel.h"
#include "target.h"

#include "../MyMesh.h"
#include "HeadPresets.h"
#include "UITask.h"
#if defined(FREENOVE_UITEST) && FREENOVE_UITEST
#include "UiTest.h"
#endif

// The companion objects (main.cpp); used only from serviceUiLoop(), on the
// loop that owns them.
extern MyMesh the_mesh;
extern UITask ui_task;

namespace {

constexpr int kW = freenove_ui::profile::kScreenW;
constexpr int kH = freenove_ui::profile::kScreenH;
constexpr uint32_t kTaskStack = 8192;
#ifndef HMI_TASK_PRIORITY
#define HMI_TASK_PRIORITY 1   // same as the Arduino loop task
#endif
constexpr UBaseType_t kTaskPriority = HMI_TASK_PRIORITY;
constexpr uint32_t kStatsMs = 10000;
#if HMI_CYD
// Internal RAM only (no PSRAM): a bounded model.
#ifndef HMI_MODEL_NODES
#define HMI_MODEL_NODES 64
#endif
#ifndef HMI_MODEL_FEED
#define HMI_MODEL_FEED 128
#endif
constexpr size_t kNodeCapacity = HMI_MODEL_NODES;
constexpr size_t kFeedCapacity = HMI_MODEL_FEED;
constexpr uint32_t kBigCaps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
#else
constexpr size_t kFbBytes = (size_t)kW * kH * 2;   // RGB565
constexpr size_t kNodeCapacity = 256;      // spec: node table
constexpr size_t kFeedCapacity = 512;      // spec: packet ring
constexpr uint32_t kBigCaps = MALLOC_CAP_SPIRAM;
#endif

// Model, page tables and relay names: PSRAM on the Freenove, internal RAM
// on the CYD.
void* psramAlloc(size_t bytes, void*) { return heap_caps_malloc(bytes, kBigCaps); }
void psramFree(void* p, void*) { heap_caps_free(p); }
const mesh::head::ModelAllocator kPsramAllocator = {psramAlloc, psramFree, nullptr};

// The repeater's radio for the companion UI (HeadRadio.h): written by the
// page task, read by the UI loop.
portMUX_TYPE s_radioMux = portMUX_INITIALIZER_UNLOCKED;
bool s_radioKnown = false;
mesh::ui::RepeaterRadio s_radio;

// The running visual head, for freenoveHeadReceived. Published once by
// begin() (setup, loop task) after everything it points to is ready; read by
// the companion loop. Release/acquire so a reader never sees a half-built
// head even if begin() moves to another task.
std::atomic<FreenoveUITask*> s_head(nullptr);

// Preset names the head's admin console last read (UI loop -> page task),
// kept in NVS ("hpreset") so the PRESET tile names the slot after a reboot.
portMUX_TYPE s_presetMux = portMUX_INITIALIZER_UNLOCKED;
char s_presetNames[5][13];
uint32_t s_presetGen = 0;
constexpr const char* kPresetNs = "hpreset";

// The ADMIN tile: the console's login state (UI loop -> page task).
std::atomic<bool> s_adminLoggedIn(false);

void onPresetList(const HeadPresetList& list) {
  char names[5][13];
  for (int i = 0; i < 5; i++) {
    if (list.slot[i].used) snprintf(names[i], sizeof(names[i]), "%s", list.slot[i].name);
    else names[i][0] = 0;
  }
  portENTER_CRITICAL(&s_presetMux);
  const bool changed = memcmp(names, s_presetNames, sizeof(names)) != 0;
  memcpy(s_presetNames, names, sizeof(names));
  s_presetGen++;
  portEXIT_CRITICAL(&s_presetMux);
  if (!changed) return;   // NVS only when the list changed
  Preferences prefs;
  if (!prefs.begin(kPresetNs, false)) return;
  prefs.putBytes("names", names, sizeof(names));
  prefs.end();
}

void loadPresetNames() {
  char names[5][13];
  Preferences prefs;
  if (!prefs.begin(kPresetNs, true)) return;
  const size_t n = prefs.getBytes("names", names, sizeof(names));
  prefs.end();
  if (n != sizeof(names)) return;
  for (int i = 0; i < 5; i++) names[i][12] = 0;
  portENTER_CRITICAL(&s_presetMux);
  memcpy(s_presetNames, names, sizeof(names));
  s_presetGen++;
  portEXIT_CRITICAL(&s_presetMux);
}

// The header's repeater name before the M5's own advert is heard this boot:
// the name the model last learned (NVS "hrepname", page task writes) or the
// home repeater's contact name (UI loop writes). Guarded by s_nameMux.
portMUX_TYPE s_nameMux = portMUX_INITIALIZER_UNLOCKED;
char s_learnedName[24];
char s_homeName[24];
uint32_t s_nameGen = 0;
constexpr const char* kRepNameNs = "hrepname";

void loadRepeaterName() {
  Preferences prefs;
  if (!prefs.begin(kRepNameNs, true)) return;
  char name[24] = "";
  prefs.getString("name", name, sizeof(name));
  prefs.end();
  name[sizeof(name) - 1] = 0;
  portENTER_CRITICAL(&s_nameMux);
  memcpy(s_learnedName, name, sizeof(name));
  s_nameGen++;
  portEXIT_CRITICAL(&s_nameMux);
}

// Relay names (RelayNameStore): one NVS blob, read at boot, written by the
// page task at most once a minute and only after a change.
constexpr const char* kRelayNamesNs = "hrelays";
constexpr const char* kRelayNamesKey = "v1";

mesh::head::RelayNameStore* loadRelayNames() {
  void* mem = heap_caps_malloc(sizeof(mesh::head::RelayNameStore), kBigCaps);
  if (mem == nullptr) return nullptr;
  auto* store = new (mem) mesh::head::RelayNameStore();
  uint8_t* blob = (uint8_t*)heap_caps_malloc(mesh::head::RelayNameStore::kBlobMax, kBigCaps);
  Preferences prefs;
  if (blob != nullptr && prefs.begin(kRelayNamesNs, true)) {
    const size_t len = prefs.getBytesLength(kRelayNamesKey);
    if (len > 0 && len <= mesh::head::RelayNameStore::kBlobMax &&
        prefs.getBytes(kRelayNamesKey, blob, len) == len) {
      store->load(blob, len);
    }
    prefs.end();
  }
  heap_caps_free(blob);
  store->begin(millis());
  return store;
}

bool companionRepeaterRadio(mesh::ui::RepeaterRadio& out) {
  portENTER_CRITICAL(&s_radioMux);
  const bool known = s_radioKnown;
  if (known) out = s_radio;
  portEXIT_CRITICAL(&s_radioMux);
  return known;
}

// Text diagnostics only while USB logging is on and no companion app owns
// the USB stream (text would corrupt its binary frames).
template <typename... Args>
void logf(const char* fmt, Args... args) {
  if (mesh::isUsbBinaryStreamOwned() || !mesh::isUsbLoggingEnabled()) return;
  mesh::usbLoggingPort().printf(fmt, args...);
}
void logln(const char* s) { logf("%s\n", s); }

}  // namespace

FreenoveUITask::FreenoveUITask(HeadDisplay& display) : _display(display), _nav(freenove_ui::PageSet::kCount) {
  memset(&_ctx, 0, sizeof(_ctx));
}

bool FreenoveUITask::begin(const uint8_t* self_pub_key) {
  if (_task != nullptr) return true;
#if HMI_CYD
  if (!_display.hmiReady()) {
    logln("cyd: no band buffer; companion UI only");
    return false;
  }
  _model = new (std::nothrow) mesh::head::MeshModel(kNodeCapacity, kFeedCapacity, &kPsramAllocator);
  if (_model == nullptr || !_model->ok()) {
    logln("cyd: visual head allocation failed; companion UI only");
    delete _model;
    _model = nullptr;
    return false;
  }
#else
  _model = new (std::nothrow) mesh::head::MeshModel(kNodeCapacity, kFeedCapacity, &kPsramAllocator);
  _bg = (uint16_t*)heap_caps_malloc(kFbBytes, MALLOC_CAP_SPIRAM);
  _buf[0] = _display.reservedPageBuffer();
  _buf[1] = (uint16_t*)heap_caps_malloc(kFbBytes, MALLOC_CAP_SPIRAM);
  if (_model == nullptr || !_model->ok() || _bg == nullptr || _buf[0] == nullptr || _buf[1] == nullptr) {
    logln("freenove: visual head allocation failed; companion UI only");
    delete _model;
    _model = nullptr;
    heap_caps_free(_bg);
    heap_caps_free(_buf[1]);
    _bg = _buf[0] = _buf[1] = nullptr;
    return false;
  }
  for (int i = 0; i < 2; i++) _fb[i] = FrameBuffer565(_buf[i], kW, kH);
#endif
  updateClock(millis());
  _model->setHeadKey(self_pub_key);
  _names = loadRelayNames();   // before the first advert: "via" names at boot
  _model->setNameStore(_names);
#if defined(FREENOVE_UITEST) && FREENOVE_UITEST
  freenove_uitest::noteRelayNames(_names);
#endif
  _ctx.model = _model;
  _pages.attach(&_ctx);
  _display.setCompanionOverlay(drawHeadOverlay, this);
  // Network/Radio screens show the repeater's radio, never the placeholder.
  mesh::ui::setRepeaterRadioSource(companionRepeaterRadio);
  headPresetListSink() = onPresetList;
  loadPresetNames();
  loadRepeaterName();
  _pages.setAllocator(&kPsramAllocator);
  // From here the page task reads the touch controller (switched here, on
  // the UI loop that read it until now, so the two never overlap).
  _display.setTouchRelayed(true);

  if (xTaskCreatePinnedToCore(taskEntry, "pages", kTaskStack, this, kTaskPriority, &_task, 1) != pdPASS) {
    logln("freenove: page task create failed; companion UI only");
    _task = nullptr;
    _display.setTouchRelayed(false);
    _display.setCompanionOverlay(nullptr, nullptr);
    mesh::ui::setRepeaterRadioSource(nullptr);
    return false;
  }
  // Frames queue up (bounded) until the task drains them.
  ESPNOWRadio::setObserverHandler(onObserverFrame, this);
  s_head.store(this, std::memory_order_release);
  logf("freenove: visual head up (pages, model %u nodes / %u feed in %s); "
                "free internal %u B, free PSRAM %u B\n",
                (unsigned)kNodeCapacity, (unsigned)kFeedCapacity, HMI_CYD ? "RAM" : "PSRAM",
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  return true;
}

// WiFi task: one bounded copy, nothing else.
void FreenoveUITask::onObserverFrame(const uint8_t* frame, size_t len, void* ctx) {
  FreenoveUITask* self = static_cast<FreenoveUITask*>(ctx);
  portENTER_CRITICAL(&self->_ringLock);
  self->_ring.push(frame, len);
  portEXIT_CRITICAL(&self->_ringLock);
}

// Companion loop: one bounded copy, nothing else (the model is the page
// task's). Longer packets than one ESP-NOW frame cannot arrive here.
void freenoveHeadReceived(const uint8_t* raw, int len) {
  FreenoveUITask* self = s_head.load(std::memory_order_acquire);
  if (self == nullptr || raw == nullptr || len <= 0) return;
  portENTER_CRITICAL(&self->_ringLock);
  self->_bridged.push(raw, (size_t)len);
  portEXIT_CRITICAL(&self->_ringLock);
}

// UI loop, before each companion push: the tab bar under the companion
// area (the companion UI never draws there).
void FreenoveUITask::drawHeadOverlay(FrameBuffer565& fb, void* ctx) {
  (void)ctx;
  freenove_ui::drawTabBar(fb, freenove_ui::PageSet::kHead, freenove_ui::PageSet::kCount);
}

// After new observer data: refresh the HUD radio label (a frame when it
// changed) and the companion UI's copy.
void FreenoveUITask::updateRadio(uint32_t now_ms) {
  const mesh::observer::Vitals* vit = _model->vitals();
  char label[sizeof(_radioLabel)];
  const freenove_ui::RadioState state =
      freenove_ui::formatRepeaterRadio(label, sizeof(label), vit, _model->vitalsMs(), now_ms);
  const bool known = state != freenove_ui::kRadioNone;
  if (state != _radioState || strcmp(label, _radioLabel) != 0) {
    memcpy(_radioLabel, label, sizeof(_radioLabel));
    _radioState = state;
    _forceFrame = true;
  }
  portENTER_CRITICAL(&s_radioMux);
  s_radioKnown = known;
  if (known) {
    s_radio.freq = vit->freq;
    s_radio.bw = vit->bw;
    s_radio.sf = vit->sf;
    s_radio.cr = vit->cr;
  }
  portEXIT_CRITICAL(&s_radioMux);
}

// Header clock: sampled once a second; a frame when the text changes (once
// a minute) so pages without motion of their own keep it current.
void FreenoveUITask::updateClock(uint32_t now_ms) {
  const uint32_t epoch = rtc_clock.getCurrentTime();
  _clock.sample(epoch, now_ms);
  char hm[sizeof(_ctx.clock_hm)], date[sizeof(_ctx.clock_date)];
  freenove_ui::hmi::formatClock(hm, sizeof(hm), date, sizeof(date), _clock.valid(), epoch, now_ms);
  if (strcmp(hm, _ctx.clock_hm) != 0 || strcmp(date, _ctx.clock_date) != 0) {
    memcpy(_ctx.clock_hm, hm, sizeof(hm));
    memcpy(_ctx.clock_date, date, sizeof(date));
    _forceFrame = true;
  }
}

// A page's request: device actions go to the UI loop (serviceUiLoop); the
// admin console is the HEAD page.
void FreenoveUITask::handleRequest(freenove_ui::PageRequest req, uint32_t now_ms) {
  if (req == freenove_ui::kReqNone) return;
  uint8_t expected = 0;
  if (!_uiRequest.compare_exchange_strong(expected, (uint8_t)req)) {
    // The UI loop has not taken the previous one yet: say so on the page.
    current()->requestDone(req, false, now_ms);
    return;
  }
  _resultFor = req;
  if (req != freenove_ui::kReqAdvert) {
    showPage(freenove_ui::PageSet::kHead, now_ms);
  }
}

// UI loop context: the companion objects are this loop's.
void FreenoveUITask::serviceUiLoop() {
  s_adminLoggedIn.store(ui_task.adminLoggedIn(), std::memory_order_relaxed);
  // The home repeater's contact name (header fallback), every few seconds.
  const uint32_t now = millis();
  if (_homeCheckMs == 0 || (uint32_t)(now - _homeCheckMs) >= 5000) {
    _homeCheckMs = now == 0 ? 1 : now;
    char name[24] = "";
    const uint8_t* home = the_mesh.headHomeRepeater();
    const ContactInfo* c = home != nullptr ? the_mesh.lookupContactByPubKey(home, 4) : nullptr;
    if (c != nullptr) snprintf(name, sizeof(name), "%s", c->name);
    portENTER_CRITICAL(&s_nameMux);
    if (strcmp(name, s_homeName) != 0) {
      memcpy(s_homeName, name, sizeof(name));
      s_nameGen++;
    }
    portEXIT_CRITICAL(&s_nameMux);
  }
  const uint8_t req = _uiRequest.load();
  if (req == 0) return;
  bool ok = true;
  switch ((freenove_ui::PageRequest)req) {
    case freenove_ui::kReqAdvert:
      ok = the_mesh.advert();
      break;
    case freenove_ui::kReqOpenHeadAdmin:
      ui_task.showAdmin();
      break;
    case freenove_ui::kReqAdminLogin:
      ui_task.showAdminLogin();   // the saved password; asks for one if none
      break;
    case freenove_ui::kReqOpenPresets:
      ui_task.showAdminPresets();   // logs in first (saved password) if needed
      break;
    case freenove_ui::kReqOpenSettings:
      ui_task.showAdminSettings();
      break;
    default:
      break;
  }
  _uiResult.store(ok ? 1 : 2);
  _uiRequest.store(0);
}

void FreenoveUITask::taskEntry(void* arg) { static_cast<FreenoveUITask*>(arg)->run(); }

void FreenoveUITask::drainObserver(uint32_t now_ms) {
  uint8_t frame[mesh::observer::FrameRing<kRingDepth>::MAX_FRAME];
  for (size_t i = 0; i < kRingDepth; i++) {
    size_t len = 0;
    portENTER_CRITICAL(&_ringLock);
    const bool got = _ring.pop(frame, sizeof(frame), len);
    _ctx.obs_frames = _ring.pushed();
    _ctx.obs_dropped = _ring.dropped();
    portEXIT_CRITICAL(&_ringLock);
    if (!got) break;
    _ctx.obs_last_ms = now_ms;
    _ctx.obs_seen = true;
    // Model time is the head's clock; the record's M5 timestamp is display-only.
    if (frame[5] == (uint8_t)mesh::observer::Kind::Packet) {
      mesh::observer::PacketRecord rec;
      if (mesh::observer::decodePacket(frame, len, rec)) _model->onObserverPacket(rec, now_ms);
      else _ctx.obs_bad++;
    } else {
      mesh::observer::Vitals vit;
      if (mesh::observer::decodeVitals(frame, len, vit)) _model->onVitals(vit, now_ms);
      else _ctx.obs_bad++;
    }
    _ctx.data_gen++;
  }
  // Packets the head received itself: no signal; the model counts each
  // packet once whichever path (observer or bridge) delivers it first.
  for (size_t i = 0; i < kBridgedDepth; i++) {
    size_t len = 0;
    portENTER_CRITICAL(&_ringLock);
    const bool got = _bridged.pop(frame, sizeof(frame), len);
    portEXIT_CRITICAL(&_ringLock);
    if (!got) break;
    _ctx.bridge_last_ms = now_ms;
    _ctx.bridge_seen = true;
    _model->onBridgedPacket(frame, len, now_ms);
    _ctx.data_gen++;
  }
}

void FreenoveUITask::pollTouch(uint32_t now_ms) {
  int x = -1, y = -1;
#if defined(FREENOVE_UITEST) && FREENOVE_UITEST
  bool down = _display.touch().read(x, y);
  freenove_uitest::overrideTouch(now_ms, down, x, y);   // an injected gesture wins
#else
  const bool down = _display.touch().read(x, y);
#endif
  const bool pressed = down && !_touchDown;
  _touchDown = down;
  freenove_ui::Page* page = current();
  const bool external = page->external();

  if (external && !_display.isOn()) {
    // The companion UI wakes itself on this touch (and ignores it otherwise).
    _display.relayTouch(down, x, y);
    return;
  }
  const freenove_ui::NavEvent ev = _nav.update(down, x, y, now_ms, external);
  // A press on a visual page or on the HEAD page's tab bar is not seen by
  // the companion UI: tell it about the activity.
  if (pressed && (!external || _nav.tabAt(x, y) >= 0)) {
    const bool wasOn = _display.isOn();
    _display.relayActivity();   // UI loop: wake / restart the auto-off timer
    if (!wasOn) {
      _nav.swallowGesture();    // a waking touch does nothing else
      return;
    }
  }

  switch (ev.kind) {
    case freenove_ui::NavEvent::PageChanged:
      showPage(_nav.current(), now_ms);
      break;
    case freenove_ui::NavEvent::Tap:
      if (external) {
        _display.relayTouch(true, ev.x, ev.y);   // reported once even if released already
        _display.relayTouch(false, -1, -1);
      } else {
        const bool a = page->touch(ev.x, ev.y, true);
        const bool b = page->touch(ev.x, ev.y, false);
        if (a || b) _forceFrame = true;
      }
      break;
    case freenove_ui::NavEvent::Press:
    case freenove_ui::NavEvent::Move:
    case freenove_ui::NavEvent::Release: {
      const bool isDown = ev.kind != freenove_ui::NavEvent::Release;
      if (external) _display.relayTouch(isDown, ev.x, ev.y);
      else if (page->touch(ev.x, ev.y, isDown)) _forceFrame = true;
      break;
    }
    default:
      break;
  }
}

// Page task: new or renamed relay names go to NVS, at most once a minute.
void FreenoveUITask::saveRelayNames(uint32_t now_ms) {
  if (_names == nullptr || !_names->saveDue(now_ms)) return;
  uint8_t* blob = (uint8_t*)heap_caps_malloc(mesh::head::RelayNameStore::kBlobMax, kBigCaps);
  if (blob == nullptr) return;
  const size_t len = _names->serialize(blob, mesh::head::RelayNameStore::kBlobMax);
  Preferences prefs;
  bool ok = false;
  if (len > 0 && prefs.begin(kRelayNamesNs, false)) {
    ok = prefs.putBytes(kRelayNamesKey, blob, len) == len;
    prefs.end();
  }
  heap_caps_free(blob);
  // A failed write retries after the next interval, not every frame.
  _names->markSaved(now_ms, ok);
  logf("freenove: relay names %s (%u)\n", ok ? "saved" : "save failed", (unsigned)_names->count());
}

// Page task: a name the model learned from the M5's own advert is kept in
// NVS (written only when it changes); the fallback goes into _ctx.
void FreenoveUITask::updateRepeaterName() {
  const char* learned = _model->repeaterName();
  bool save = false;
  char copy[24];
  portENTER_CRITICAL(&s_nameMux);
  if (learned[0] && strncmp(learned, s_learnedName, sizeof(s_learnedName) - 1) != 0) {
    snprintf(s_learnedName, sizeof(s_learnedName), "%s", learned);
    memcpy(copy, s_learnedName, sizeof(copy));
    s_nameGen++;
    save = true;
  }
  if (s_nameGen != _nameGen) {
    _nameGen = s_nameGen;
    memcpy(_ctx.repeater_name, s_learnedName[0] ? s_learnedName : s_homeName, sizeof(_ctx.repeater_name));
    _forceFrame = true;
  }
  portEXIT_CRITICAL(&s_nameMux);
  if (save) {
    Preferences prefs;
    if (prefs.begin(kRepNameNs, false)) {
      prefs.putString("name", copy);
      prefs.end();
    }
  }
}

void FreenoveUITask::showPage(int index, uint32_t now_ms) {
  freenove_ui::Page* next = _pages.page(index);
  if (next == nullptr) return;
  if (_shown != nullptr) _shown->onHide();
  _nav.setCurrent(index);
  _display.relayTouch(false, -1, -1);
  if (next->external()) {
    _display.setPanelOwner(HeadDisplay::PanelOwner::Companion);
  } else {
    _display.setPanelOwner(HeadDisplay::PanelOwner::Pages);
    _bgDirty = true;
    _forceFrame = true;
  }
  next->onShow(now_ms);
  _shown = next;
#if defined(FREENOVE_UITEST) && FREENOVE_UITEST
  freenove_uitest::notePage(index, next->title());
#endif
  logf("freenove: page %s\n", next->title());
}

#if !HMI_CYD
void FreenoveUITask::renderFrame(uint32_t now_ms) {
  const int64_t t0 = esp_timer_get_time();
  freenove_ui::Page* page = current();
  if (_bgDirty) {
    // The page's background layer, once per show: backdrop + glow, panels,
    // static labels, tab bar. Frames restore from it (eraseTouched).
    FrameBuffer565 bg(_bg, kW, kH);
    freenove_ui::drawPageChrome(bg, *page, _nav.current(), _nav.count());
    _bgDirty = false;
    _needsCopy[0] = _needsCopy[1] = true;
    _bgUs += (uint32_t)(esp_timer_get_time() - t0);
  }
  FrameBuffer565& fb = _fb[_cur];
  if (_needsCopy[_cur]) {
    fb.copyFrom(_bg);
    _needsCopy[_cur] = false;
  } else {
    fb.eraseTouched(_bg);   // only what this buffer's last frame drew
  }
  page->draw(fb, now_ms);
  freenove_ui::drawHeader(fb, _ctx, now_ms);
  _cur ^= 1;
  _forceFrame = false;
  _renderUs += (uint32_t)(esp_timer_get_time() - t0);
  // Waits for the previous push; the pusher then reads fb while the other
  // buffer is drawn. A dropped submit makes the next push a full resend,
  // and another frame is rendered so a page that never asks for one does
  // not leave stale glass.
  if (!_display.submitPage(fb)) _forceFrame = true;
  _framesRendered++;
}
#else
// One band of the current page's frame (CYDDisplay::renderPage, page task).
void FreenoveUITask::drawBand(FrameBuffer565& fb, int band, void* ctx) {
  FreenoveUITask* self = static_cast<FreenoveUITask*>(ctx);
  self->_ctx.band = band;
  // drawFrame() in three timed steps (stats: chrome / page / header).
  freenove_ui::Page& page = *self->current();
  const int64_t t0 = esp_timer_get_time();
  fb.resetClip();
  freenove_ui::drawPageChrome(fb, page, self->_nav.current(), self->_nav.count());
  const int64_t t1 = esp_timer_get_time();
  fb.resetClip();
  page.draw(fb, self->_ctx.now_ms);
  const int64_t t2 = esp_timer_get_time();
  fb.resetClip();
  freenove_ui::drawHeader(fb, self->_ctx, self->_ctx.now_ms);
  const int64_t t3 = esp_timer_get_time();
  self->_chromeUs += (uint32_t)(t1 - t0);
  self->_pageUs += (uint32_t)(t2 - t1);
  self->_headerUs += (uint32_t)(t3 - t2);
  self->_ctx.band = 0;
}

void FreenoveUITask::renderFrame(uint32_t now_ms) {
  const int64_t t0 = esp_timer_get_time();
  _ctx.now_ms = now_ms;
  _bgDirty = false;
  const bool drawn = _display.renderPage(drawBand, this);
  _forceFrame = false;
  _lastFrameMs = now_ms;
  const uint32_t pushUs = _display.lastPushUs();
  _renderUs += (uint32_t)(esp_timer_get_time() - t0) - pushUs;
  _pushUs += pushUs;
  _bandsPushed += (uint32_t)_display.lastBandsPushed();
  if (drawn) _framesRendered++;
  else _forceFrame = true;   // the companion still owns the panel: retry
}
#endif

void FreenoveUITask::run() {
  showPage(freenove_ui::PageSet::kHome, millis());
  TickType_t wake = xTaskGetTickCount();
  uint32_t lastStats = millis();
  for (;;) {
    const uint32_t now = millis();
    drainObserver(now);
    // New vitals, or once a second so the label goes stale on time.
    if (_ctx.data_gen != _radioGen || (uint32_t)(now - _radioCheckMs) >= 1000) {
      _radioGen = _ctx.data_gen;
      _radioCheckMs = now;
      updateRadio(now);
    }
    _model->tick(now);
    if ((uint32_t)(now - _clockCheckMs) >= 1000) {
      _clockCheckMs = now;
      updateClock(now);
    }
    // The header's link icon follows the observer's freshness.
    const int link = freenove_ui::hmi::linkLevel(_ctx.obs_seen, now - _ctx.obs_last_ms);
    if (link != _linkLevel) {
      _linkLevel = link;
      _forceFrame = true;
    }
    _ctx.now_ms = now;
    portENTER_CRITICAL(&s_presetMux);
    if (s_presetGen != _presetGen) {
      _presetGen = s_presetGen;
      memcpy(_ctx.preset_names, s_presetNames, sizeof(_ctx.preset_names));
      _forceFrame = true;
    }
    portEXIT_CRITICAL(&s_presetMux);
    updateRepeaterName();
    saveRelayNames(now);
    const bool loggedIn = s_adminLoggedIn.load(std::memory_order_relaxed);
    if (loggedIn != _ctx.admin_logged_in) {
      _ctx.admin_logged_in = loggedIn;
      _forceFrame = true;
    }
#if defined(FREENOVE_UITEST) && FREENOVE_UITEST
    const int wanted = freenove_uitest::takePageRequest();
    if (wanted >= 0 && wanted < _nav.count()) showPage(wanted, now);
#endif
    pollTouch(now);
#if HMI_CYD && defined(FREENOVE_UITEST) && FREENOVE_UITEST
    {
      freenove_uitest::ShotSink sink = nullptr;
      void* sinkCtx = nullptr;
      if (freenove_uitest::takeShotRequest(sink, sinkCtx)) {
        _ctx.now_ms = _lastFrameMs;   // the frame on the glass
        if (!current()->external()) _display.dumpPage(drawBand, this, sink, sinkCtx);
        freenove_uitest::shotDone();
      }
    }
#endif
    handleRequest(current()->takeRequest(), now);
    const uint8_t result = _uiResult.exchange(0);
    if (result != 0) {
      _pages.page(freenove_ui::PageSet::kHome)->requestDone(_resultFor, result == 1, now);
      _forceFrame = true;
    }

    freenove_ui::Page* page = current();
    const bool on = _display.isOn();
    if (on && !_wasOn) _forceFrame = true;   // frames were skipped while dark
    _wasOn = on;
#if HMI_CYD
    const bool due = _bgDirty || (uint32_t)(now - _lastFrameMs) >= kMinFrameGapMs;
#else
    const bool due = true;
#endif
    if (on && !page->external() && due && (_forceFrame || _bgDirty || page->wantsFrame(now))) {
      renderFrame(now);
    }

    if ((uint32_t)(now - lastStats) >= kStatsMs) {
#if HMI_CYD
      logf("cyd: pages page=%s frames=%lu (%.1f fps) render_ms=%.1f (chrome %.1f page %.1f header %.1f) "
           "push_ms=%.1f bands=%.1f obs=%lu dropped=%lu bad=%lu nodes=%u heap=%u min=%u largest=%u\n",
           page->title(), (unsigned long)_framesRendered, _framesRendered * 1000.0f / (float)(now - lastStats),
           _framesRendered ? (double)_renderUs / 1000.0 / _framesRendered : 0.0,
           _framesRendered ? (double)_chromeUs / 1000.0 / _framesRendered : 0.0,
           _framesRendered ? (double)_pageUs / 1000.0 / _framesRendered : 0.0,
           _framesRendered ? (double)_headerUs / 1000.0 / _framesRendered : 0.0,
           _framesRendered ? (double)_pushUs / 1000.0 / _framesRendered : 0.0,
           _framesRendered ? (double)_bandsPushed / _framesRendered : 0.0, (unsigned long)_ctx.obs_frames,
           (unsigned long)_ctx.obs_dropped, (unsigned long)_ctx.obs_bad, (unsigned)_model->nodeCount(),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
#if defined(FREENOVE_UITEST) && FREENOVE_UITEST
      freenove_uitest::noteStats(_framesRendered, now - lastStats, _renderUs, _pushUs, _bandsPushed, _chromeUs,
                                 _pageUs, _headerUs);
#endif
      _framesRendered = 0;
      _renderUs = _bgUs = _pushUs = _bandsPushed = 0;
      _chromeUs = _pageUs = _headerUs = 0;
      lastStats = now;
#else
      const FramePusher::Stats s = _display.pusher().takeStats();
      logf("freenove: pages page=%s frames=%lu (%.1f fps) render_ms=%.2f bg_ms=%lu push_kb=%.1f "
                    "push_ms=%.2f diff_ms=%.2f obs=%lu dropped=%lu bad=%lu nodes=%u\n",
                    page->title(), (unsigned long)_framesRendered,
                    _framesRendered * 1000.0f / (float)(now - lastStats),
                    _framesRendered ? (double)(_renderUs - _bgUs) / 1000.0 / _framesRendered : 0.0,
                    (unsigned long)(_bgUs / 1000u), (double)s.bytes / 1024.0,
                    s.pushed ? (double)s.pushUs / 1000.0 / s.pushed : 0.0,
                    s.submitted ? (double)s.diffUs / 1000.0 / s.submitted : 0.0,
                    (unsigned long)_ctx.obs_frames, (unsigned long)_ctx.obs_dropped,
                    (unsigned long)_ctx.obs_bad, (unsigned)_model->nodeCount());
      _framesRendered = 0;
      _renderUs = _bgUs = 0;
      lastStats = now;
#endif
    }

    // Fixed frame budget: never faster than kFramePeriodMs, always yield.
    if (xTaskDelayUntil(&wake, pdMS_TO_TICKS(kFramePeriodMs)) == pdFALSE) {
      wake = xTaskGetTickCount();
      vTaskDelay(1);
    }
  }
}
