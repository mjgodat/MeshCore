#pragma once

// Reset history: evidence for the next unexplained stop (repeater, ESP32).
// Pure C++11 logic, host-tested (test/test_reset_history.py); the ESP32 glue
// (ResetHistoryEsp32.h) keeps the record in NVS and an RTC copy.
//
// While running, the repeater checkpoints "still alive" every
// kCheckpointSecs into a small persistent record: uptime, wall-clock time
// (0 when the clock was never set) and the lowest free heap so far. A copy
// in RTC memory is refreshed every second; it survives software, panic and
// watchdog resets but not a power loss. At the next boot onBoot() turns
// that into a summary of the previous run:
//   - how it ended: this boot's reset reason (esp_reset_reason_t numbers,
//     ObserverFrame.h resetReasonName);
//   - how long it ran and when it was last seen alive: exact from the RTC
//     copy when it survived, otherwise the last NVS checkpoint (at most
//     kCheckpointSecs old);
// and counts boots and abnormal ends (panic, watchdogs, brownout). A
// power-on reset after a run that never asked to reboot is a power loss
// or an EN/RST pulse (the two look the same to the chip).
// `powerlog` prints it (format()).

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

namespace mesh {
namespace resethistory {

constexpr uint32_t kMagic = 0x52535431;   // "RST1"
constexpr uint32_t kCheckpointSecs = 600;  // NVS write cadence (about 144 a day)
constexpr uint32_t kFirstCheckpointSecs = 60;

// esp_reset_reason_t values (IDF 4.4 / 5.x).
enum Reason : uint8_t { Unknown = 0, PowerOn = 1, Ext = 2, Sw = 3, Panic = 4, IntWdt = 5, TaskWdt = 6,
                        Wdt = 7, DeepSleep = 8, Brownout = 9, Sdio = 10 };

inline bool abnormal(uint8_t reason) {
  return reason == Panic || reason == IntWdt || reason == TaskWdt || reason == Wdt || reason == Brownout;
}

// Persistent (NVS). Fixed layout, little-endian host == ESP32.
struct Record {
  uint32_t magic;
  uint32_t boots;            // boots since the record was created
  uint32_t abnormal_ends;    // runs ended by panic / watchdog / brownout
  // this run, refreshed at every checkpoint
  uint32_t alive_uptime_s, alive_epoch, alive_min_heap;
  // the previous run, set at boot
  uint32_t prev_uptime_s, prev_epoch, prev_min_heap;
  uint8_t prev_end_reason;   // this boot's reset reason
  uint8_t prev_exact;        // 1: from the RTC copy (exact), 0: last checkpoint
  uint8_t prev_known;        // 0: no previous run recorded
  uint8_t last_abnormal_reason;
  uint32_t last_abnormal_epoch;   // when that run was last alive (0 unknown)
};

// RTC copy (RTC_NOINIT on the device), refreshed every second.
struct RtcCopy {
  uint32_t magic, uptime_s, epoch, min_heap, check;
};

inline uint32_t rtcCheck(const RtcCopy& r) {
  return r.magic ^ (r.uptime_s * 2654435761u) ^ (r.epoch + 0x9E3779B9u) ^ (r.min_heap << 7);
}
inline bool rtcValid(const RtcCopy& r) { return r.magic == kMagic && r.check == rtcCheck(r); }
inline void rtcUpdate(RtcCopy& r, uint32_t uptime_s, uint32_t epoch, uint32_t min_heap) {
  r.magic = kMagic; r.uptime_s = uptime_s; r.epoch = epoch; r.min_heap = min_heap; r.check = rtcCheck(r);
}
inline void rtcClear(RtcCopy& r) { memset(&r, 0, sizeof(r)); }

// Called once at boot. `loaded` = the NVS record was read; the RTC copy is
// used only when valid and the reset kept RTC memory (not power-on).
inline void onBoot(Record& rec, bool loaded, RtcCopy& rtc, uint8_t reason) {
  if (!loaded || rec.magic != kMagic) {
    memset(&rec, 0, sizeof(rec));
    rec.magic = kMagic;
    loaded = false;
  }
  const bool rtc_ok = reason != PowerOn && reason != Unknown && rtcValid(rtc);
  rec.prev_known = (loaded || rtc_ok) ? 1 : 0;
  rec.prev_exact = rtc_ok ? 1 : 0;
  rec.prev_uptime_s = rtc_ok ? rtc.uptime_s : rec.alive_uptime_s;
  rec.prev_epoch = rtc_ok ? rtc.epoch : rec.alive_epoch;
  rec.prev_min_heap = rtc_ok ? rtc.min_heap : rec.alive_min_heap;
  rec.prev_end_reason = reason;
  if (rec.prev_known && abnormal(reason)) {
    rec.abnormal_ends++;
    rec.last_abnormal_reason = reason;
    rec.last_abnormal_epoch = rec.prev_epoch;
  }
  rec.boots++;
  rec.alive_uptime_s = rec.alive_epoch = rec.alive_min_heap = 0;
  rtcClear(rtc);
}

// True when a checkpoint should be written now (first after
// kFirstCheckpointSecs, then every kCheckpointSecs).
inline bool checkpointDue(uint32_t uptime_s, uint32_t last_checkpoint_s, bool any_yet) {
  if (!any_yet) return uptime_s >= kFirstCheckpointSecs;
  return uptime_s - last_checkpoint_s >= kCheckpointSecs;
}

inline void checkpoint(Record& rec, uint32_t uptime_s, uint32_t epoch, uint32_t min_heap) {
  rec.alive_uptime_s = uptime_s; rec.alive_epoch = epoch; rec.alive_min_heap = min_heap;
}

inline void formatDuration(char* out, size_t cap, uint32_t s) {
  if (s >= 86400u) snprintf(out, cap, "%lud%luh", (unsigned long)(s / 86400u), (unsigned long)(s % 86400u / 3600u));
  else if (s >= 3600u) snprintf(out, cap, "%luh%lum", (unsigned long)(s / 3600u), (unsigned long)(s % 3600u / 60u));
  else snprintf(out, cap, "%lum%lus", (unsigned long)(s / 60u), (unsigned long)(s % 60u));
}

// UTC "YYYY-MM-DD HH:MM" for an epoch, or "clock unset".
inline void formatEpoch(char* out, size_t cap, uint32_t epoch) {
  if (epoch < 1577836800u) { snprintf(out, cap, "clock unset"); return; }   // before 2020
  uint32_t days = epoch / 86400u, rem = epoch % 86400u;
  // civil-from-days (H. Hinnant), days since 1970-01-01
  int64_t z = (int64_t)days + 719468;
  const int64_t era = z / 146097;
  const uint32_t doe = (uint32_t)(z - era * 146097);
  const uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  int64_t y = (int64_t)yoe + era * 400;
  const uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const uint32_t mp = (5 * doy + 2) / 153;
  const uint32_t d = doy - (153 * mp + 2) / 5 + 1;
  const uint32_t m = mp < 10 ? mp + 3 : mp - 9;
  if (m <= 2) y++;
  snprintf(out, cap, "%04ld-%02lu-%02lu %02lu:%02luZ", (long)y, (unsigned long)m, (unsigned long)d,
           (unsigned long)(rem / 3600u), (unsigned long)(rem % 3600u / 60u));
}

// The `powerlog` lines after "Last reset reason: ..." (all within the
// 160-byte CLI reply), e.g.
//   "\r\nPrev run 3d4h~, alive 2026-11-04 03:12Z, heap>=81234"
//   "\r\nBoots 12, abnormal 1 (task wdt @2026-11-03 20:01Z)"
// "~": from the last checkpoint (up to kCheckpointSecs early), not exact.
// A run that stopped before its first checkpoint (power-on reset) reads
// "Prev run <1m, not checkpointed".
// `name` maps a reason to text (ObserverFrame.h resetReasonName).
inline size_t format(const Record& rec, char* out, size_t cap, const char* (*name)(uint8_t)) {
  if (out == nullptr || cap == 0) return 0;
  out[0] = 0;
  char up[24], at[40];
  size_t n = 0;
  if (rec.prev_known && !rec.prev_exact && rec.prev_uptime_s == 0) {
    // stopped before its first checkpoint (kFirstCheckpointSecs)
    n += (size_t)snprintf(out + n, cap - n, "\r\nPrev run <1m, not checkpointed");
  } else if (rec.prev_known) {
    formatDuration(up, sizeof(up), rec.prev_uptime_s);
    formatEpoch(at, sizeof(at), rec.prev_epoch);
    n += (size_t)snprintf(out + n, cap - n, "\r\nPrev run %s%s, alive %s, heap>=%lu", up,
                          rec.prev_exact ? "" : "~", at, (unsigned long)rec.prev_min_heap);
  } else {
    n += (size_t)snprintf(out + n, cap - n, "\r\nPrev run not recorded");
  }
  if (n >= cap) return cap - 1;
  n += (size_t)snprintf(out + n, cap - n, "\r\nBoots %lu, abnormal %lu", (unsigned long)rec.boots,
                        (unsigned long)rec.abnormal_ends);
  if (n >= cap) return cap - 1;
  if (rec.abnormal_ends) {
    formatEpoch(at, sizeof(at), rec.last_abnormal_epoch);
    n += (size_t)snprintf(out + n, cap - n, " (%s @%s)", name(rec.last_abnormal_reason), at);
  }
  return n >= cap ? cap - 1 : n;
}

}  // namespace resethistory
}  // namespace mesh
