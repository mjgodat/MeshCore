// Host harness for the repeater user-button long press.
// The test splices in the real code from examples/simple_repeater/UITask.cpp:
// the LONG_PRESS branch of UITask::loop(), UITask::handleUserButtonHold(),
// UITask::holdFeedbackActive() and millisReached(). Display, mesh and clock are
// host fakes; hardware power-off is only observed via _powering_off_at.
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>

@BUTTON_EVENTS@
#include <helpers/ui/RepeaterHoldAction.h>

namespace mesh {
namespace ui { enum class DisplayWake { Button }; }
struct FakePort {
  int printf(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(last, sizeof(last), fmt, args);
    va_end(args);
    return 0;
  }
  char last[96] = "";
};
static FakePort port;
FakePort& usbConsolePort() { return port; }
}  // namespace mesh

#define POWEROFF_DELAY 3000

static unsigned long g_now = 0;
static unsigned long millis() { return g_now; }

@MILLIS_REACHED@

struct FakeDisplay {
  bool on = true;
  int wakes = 0;
  bool isOn() { return on; }
  bool wake(mesh::ui::DisplayWake) { ++wakes; const bool changed = !on; on = true; return changed; }
};

struct FakeMesh {
  int adverts = 0;
  int last_delay = -1;
  bool last_flood = false;
  void sendSelfAdvertisement(int delay_millis, bool flood) {
    ++adverts;
    last_delay = delay_millis;
    last_flood = flood;
  }
};
static FakeMesh the_mesh;

struct UITask {
  FakeDisplay display;
  FakeDisplay* _display = &display;
  unsigned long _powering_off_at = 0;
  unsigned long _next_refresh = 12345;
  int resets = 0;
  mesh::ui::RepeaterHoldAction page_action = mesh::ui::RepeaterHoldAction::SendAdvert;
#if defined(REPEATER_UI_HOLD_ACTIONS) && REPEATER_UI_HOLD_ACTIONS
  mesh::ui::RepeaterHoldAdvertLimiter _hold_advert_limiter;
  char _hold_feedback[24] = "";
  unsigned long _hold_feedback_until = 0;
  mesh::ui::RepeaterHoldAction currentHoldAction() { return page_action; }
  bool holdFeedbackActive();
  void handleUserButtonHold();
#endif
  void resetRadioProfileDisplayPage() { ++resets; }
  void dispatch(int ev);
};

#if defined(REPEATER_UI_HOLD_ACTIONS) && REPEATER_UI_HOLD_ACTIONS
@FEEDBACK_ACTIVE@

@HOLD@
#endif

void UITask::dispatch(int ev) {
  if (ev == BUTTON_EVENT_CLICK) {
  } else if (ev == BUTTON_EVENT_LONG_PRESS) {
@BRANCH@
  }
}

static int checks = 0;
static int failures = 0;
#define CHECK(cond) do { ++checks; if (!(cond)) { ++failures; \
  std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #cond); } } while (0)

int main() {
  using namespace mesh::ui;
#if !(defined(REPEATER_UI_HOLD_ACTIONS) && REPEATER_UI_HOLD_ACTIONS)
  {
    UITask ui;
    g_now = 1000;
    ui.dispatch(BUTTON_EVENT_LONG_PRESS);
    CHECK(ui._powering_off_at == 1000 + POWEROFF_DELAY);
    CHECK(the_mesh.adverts == 0);
    CHECK(ui.display.wakes == 1);
    CHECK(ui.resets == 1);
    CHECK(ui._next_refresh == 0);
    CHECK(std::strcmp(mesh::port.last, "Powering Off\r\n") == 0);
  }
  std::printf("PASS flag-off: %d checks\n", checks);
#else
  {
    UITask ui;
    // Home page, screen on: exactly one flood advert, same args as CLI `advert`.
    g_now = 1000;
    ui.dispatch(BUTTON_EVENT_LONG_PRESS);
    CHECK(the_mesh.adverts == 1);
    CHECK(the_mesh.last_delay == 1500);
    CHECK(the_mesh.last_flood);
    CHECK(std::strcmp(ui._hold_feedback, "Advert sent") == 0);
    CHECK(ui.holdFeedbackActive());
    CHECK(ui._powering_off_at == 0);
    CHECK(ui._next_refresh == 0);

    // Repeated holds inside 30 s are refused with the remaining wait.
    g_now = 5000;
    ui.dispatch(BUTTON_EVENT_LONG_PRESS);
    CHECK(the_mesh.adverts == 1);
    CHECK(std::strcmp(ui._hold_feedback, "Advert wait 26s") == 0);
    g_now = 1000 + 29999;
    ui.dispatch(BUTTON_EVENT_LONG_PRESS);
    CHECK(the_mesh.adverts == 1);
    CHECK(std::strcmp(ui._hold_feedback, "Advert wait 1s") == 0);
    g_now = 1000 + 30000 + 3000;
    CHECK(!ui.holdFeedbackActive());
    g_now = 1000 + 30000;
    ui.dispatch(BUTTON_EVENT_LONG_PRESS);
    CHECK(the_mesh.adverts == 2);
    CHECK(std::strcmp(ui._hold_feedback, "Advert sent") == 0);

    // Pages without an action: feedback only, no advert, no radio change.
    ui.page_action = RepeaterHoldAction::None;
    g_now = 100000;
    ui.dispatch(BUTTON_EVENT_LONG_PRESS);
    CHECK(the_mesh.adverts == 2);
    CHECK(std::strcmp(ui._hold_feedback, "No hold action") == 0);

    // Dark screen: the hold only wakes it to the home page.
    ui.page_action = RepeaterHoldAction::SendAdvert;
    ui.display.on = false;
    const int resets = ui.resets;
    g_now = 200000;
    ui.dispatch(BUTTON_EVENT_LONG_PRESS);
    CHECK(the_mesh.adverts == 2);
    CHECK(ui.display.on);
    CHECK(ui.resets == resets + 1);
    CHECK(ui._powering_off_at == 0);
  }
  {
    // 32-bit millis rollover keeps the 30 s spacing.
    RepeaterHoldAdvertLimiter limiter;
    CHECK(limiter.tryConsume(0xFFFFF000u));
    CHECK(!limiter.tryConsume(0x00001000u));
    CHECK(limiter.remainingMillis(0x00001000u) == 30000u - 0x2000u);
    CHECK(limiter.tryConsume(0xFFFFF000u + 30000u));
  }
  {
    // Page -> action map: only page 0 (R1 home) sends an advert.
    CHECK(repeaterHoldActionForPage(false, 0, 0) == RepeaterHoldAction::SendAdvert);
    CHECK(repeaterHoldActionForPage(false, 2, 0) == RepeaterHoldAction::SendAdvert);
    CHECK(repeaterHoldActionForPage(false, 2, 1) == RepeaterHoldAction::None);
    CHECK(repeaterHoldActionForPage(false, 2, 2) == RepeaterHoldAction::None);
    CHECK(repeaterHoldActionForPage(false, 2, 3) == RepeaterHoldAction::SendAdvert);  // wraps
    CHECK(repeaterHoldActionForPage(true, 2, 1) == RepeaterHoldAction::None);       // R2 profile
    CHECK(repeaterHoldActionForPage(true, 2, 3) == RepeaterHoldAction::None);
    CHECK(std::strcmp(repeaterHoldHint(RepeaterHoldAction::SendAdvert), "hold: send advert") == 0);
    CHECK(repeaterHoldHint(RepeaterHoldAction::None) == nullptr);
  }
  std::printf("PASS flag-on: %d checks\n", checks);
#endif
  if (failures != 0) return 1;
  return 0;
}
