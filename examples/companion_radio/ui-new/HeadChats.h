#pragma once

// Chat screen for a repeater head (UI_HEAD_CHAT_LIST, Freenove): a
// conversation list (Public and every channel / DM with its newest message,
// unread count and age) and, on a tap, that conversation's thread: messages
// oldest -> newest with sender and age, newest at the bottom, scrolled by a
// vertical drag or the Up / Down buttons, and a "Write" bar that opens the
// head's compose screen on that conversation.
//
// It reads the companion's message store (CompanionMessageHistory, owned by
// MsgPreviewScreen) in place; the only copy it keeps is the conversation
// rows of the last list it drew (for hit testing). Display-only: it needs a
// DisplayDriver and nothing else, so it is host-testable; the caller supplies
// the configured channels and saved chat contacts and acts on the returned
// Action (home, compose).
//
// Layout in the display's logical coordinates (240 x 148 on the Freenove,
// drawn at 2x); touches arrive raw (press / move / release) like the compose
// and admin screens.

#include <stdio.h>
#include <string.h>

#include <helpers/ui/CompanionMessageHistory.h>
#include <helpers/ui/DisplayDriver.h>
#include <helpers/ui/HeadChatList.h>

template <class History>
class HeadChatsView {
 public:
  using Entry = typename History::Entry;
  using ChatRow = mesh::ui::chats::ChatRow;
  enum class Action { None, Changed, Home, Compose };

  static constexpr int kMaxRows = 24;
  static constexpr int kHeaderH = 18;
  static constexpr int kBarH = 20;
  static constexpr int kSlop = 4;          // logical px before a press becomes a drag
  static constexpr int kNoTarget = -2;     // composeChannel(): no conversation (compose's default)

  // Configured channels and saved chat contacts, read at each render.
  struct Sources {
    const int* channels;
    const char* const* channel_names;
    int channel_count;
    const char* const* dm_names;
    int dm_count;
  };

  void showList() {
    _mode = Mode::List;
    _listScroll = 0;
    _down = _drag = false;
  }

  void openThread(const ChatRow& row) {
    _thread = row;
    _mode = Mode::Thread;
    _pinBottom = true;
    _down = _drag = false;
  }

  // The conversation of a stored message (an incoming one, say).
  void openThreadOf(const Entry& e) {
    ChatRow row;
    memset(&row, 0, sizeof(row));
    row.channel_idx = e.channel_idx >= 0 ? e.channel_idx : mesh::ui::chats::kDirect;
    if (e.channel_idx >= 0) snprintf(row.name, sizeof(row.name), "%s", e.channel_name[0] ? e.channel_name : "Channel");
    else snprintf(row.name, sizeof(row.name), "%.*s", (int)mesh::ui::chats::peerLength(e.origin), e.origin);
    openThread(row);
  }

  bool inThread() const { return _mode == Mode::Thread; }
  const ChatRow& thread() const { return _thread; }
  int scroll() const { return _mode == Mode::Thread ? _scroll : _listScroll; }
  int maxScroll() const { return _content > _view ? _content - _view : 0; }

  // Compose target after Action::Compose: a channel index, kDirect with
  // composePeer(), or kNoTarget (the "New" button).
  int composeChannel() const { return _composeChannel; }
  const char* composePeer() const { return _composePeer; }

  void describe(char* out, size_t cap) const {
    if (_mode == Mode::List) snprintf(out, cap, "list rows=%d scroll=%d", _rowCount, _listScroll);
    else snprintf(out, cap, "thread %s%s scroll=%d/%d", _thread.channel_idx >= 0 ? "#" : "@", _thread.name, _scroll,
                  maxScroll());
  }

  // now_ms: the store's clock (Entry::heard_millis); display_on: reading
  // marks the open thread's messages read.
  int render(DisplayDriver& d, History& h, const Sources& src, uint64_t now_ms, bool display_on,
             bool (*visible)(const Entry&)) {
    _visible = visible;
    d.setTextSize(1);
    if (_mode == Mode::List) renderList(d, h, src, now_ms);
    else renderThread(d, h, now_ms, display_on);
    return 1000;
  }

  // Raw touch (logical px), polled: touched + position while down, then a
  // release. Taps act on release so a drag never also presses a button.
  Action handleTouch(bool touched, int x, int y, int width, int height) {
    if (touched) {
      if (!_down) {
        _down = true;
        _drag = false;
        _x0 = x;
        _y0 = y;
        _scroll0 = scroll();
        return Action::None;
      }
      if (!_drag && (y - _y0 > kSlop || _y0 - y > kSlop) && _y0 >= kHeaderH && _y0 < bodyBottom(height)) _drag = true;
      if (!_drag) return Action::None;
      return setScroll(_scroll0 - (y - _y0)) ? Action::Changed : Action::None;
    }
    if (!_down) return Action::None;
    _down = false;
    if (_drag) { _drag = false; return Action::None; }
    return tap(_x0, _y0, width, height);
  }

  // Buttons: next / previous scroll a page, enter goes back.
  Action handleKey(int dir) {
    if (dir == 0) return back();
    return setScroll(scroll() + dir * pageStep()) ? Action::Changed : Action::None;
  }

 private:
  enum class Mode { List, Thread };

  int bodyBottom(int height) const { return _mode == Mode::Thread ? height - kBarH : height; }
  int pageStep() const { return _view > 40 ? _view * 3 / 4 : 30; }

  bool setScroll(int s) {
    s = mesh::ui::chats::clampScroll(s, _content, _view);
    int& cur = _mode == Mode::Thread ? _scroll : _listScroll;
    if (s == cur) return false;
    cur = s;
    if (_mode == Mode::Thread) _pinBottom = s >= maxScroll();
    return true;
  }

  Action back() {
    if (_mode == Mode::Thread) { showList(); return Action::Changed; }
    return Action::Home;
  }

  Action tap(int x, int y, int width, int height) {
    if (y < kHeaderH) {
      if (x < kBackW) return back();
      if (_mode == Mode::List && x >= width - kNewW) {
        _composeChannel = kNoTarget;
        _composePeer[0] = 0;
        return Action::Compose;
      }
      return Action::None;
    }
    if (_mode == Mode::List) {
      if (_rowH <= 0) return Action::None;
      const int i = (y - kHeaderH - 1 + _listScroll) / _rowH;
      if (i < 0 || i >= _rowCount) return Action::None;
      openThread(_rows[i]);
      return Action::Changed;
    }
    if (y < height - kBarH) return Action::None;
    if (x < kScrollW) return setScroll(_scroll - pageStep()) ? Action::Changed : Action::None;
    if (x >= width - kScrollW) return setScroll(_scroll + pageStep()) ? Action::Changed : Action::None;
    _composeChannel = _thread.channel_idx;
    snprintf(_composePeer, sizeof(_composePeer), "%s", _thread.channel_idx >= 0 ? "" : _thread.name);
    return Action::Compose;
  }

  // "# Public", "#testing" (a hashtag channel is already marked), "@ Bob".
  static void convTitle(char* out, size_t cap, const ChatRow& r) {
    if (r.channel_idx < 0) snprintf(out, cap, "@ %s", r.name);
    else if (r.name[0] == '#') snprintf(out, cap, "%s", r.name);
    else snprintf(out, cap, "# %s", r.name);
  }

  static void ageText(char* out, size_t cap, uint64_t now_ms, uint64_t heard_ms) {
    mesh::ui::formatCompanionMessageAge(out, cap, now_ms >= heard_ms ? now_ms - heard_ms : 0);
    char* sp = strchr(out, ' ');
    if (sp != nullptr) *sp = 0;   // "5m ago" -> "5m"
  }

  static void button(DisplayDriver& d, int x, int y, int w, int h, const char* label, bool lit = false) {
    d.setColor(lit ? UIColor::corp_blue : UIColor::title_bkg);
    d.fillRect(x + 1, y + 1, w - 2, h - 2);
    d.setColor(UIColor::secondary_txt);
    d.drawRect(x, y, w, h);
    d.setColor(UIColor::title_txt);
    const int tw = d.getTextWidth(label);
    d.drawTextEllipsized(x + (tw < w - 6 ? (w - tw) / 2 : 3), y + (h - 8) / 2, w - 6, label);
  }

  void title(DisplayDriver& d, const char* text, int x0, int x1) {
    d.setColor(UIColor::title_bkg);
    d.fillRect(x0, 0, x1 - x0, kHeaderH);
    d.setColor(UIColor::title_txt);
    d.drawTextEllipsized(x0 + 4, (kHeaderH - 8) / 2, x1 - x0 - 8, text);
  }

  // ------------------------------------------------------------ list

  void renderList(DisplayDriver& d, History& h, const Sources& src, uint64_t now_ms) {
    const int W = d.width(), H = d.height();
    const int lh = d.textLineHeight();
    _rowH = 2 * lh + 6;
    _rowCount = mesh::ui::chats::buildRows(h, [this](const Entry& e) { return _visible == nullptr || _visible(e); },
                                           src.channels, src.channel_names, src.channel_count, src.dm_names,
                                           src.dm_count, _rows, kMaxRows);
    _view = H - kHeaderH - 1;
    _content = _rowCount * _rowH;
    _listScroll = mesh::ui::chats::clampScroll(_listScroll, _content, _view);

    button(d, 0, 0, kBackW, kHeaderH, "Home");
    int unread = 0;
    for (int i = 0; i < _rowCount; i++) unread += _rows[i].unread;
    char head[24];
    if (unread > 0) snprintf(head, sizeof(head), "Chats  %d new", unread);
    else snprintf(head, sizeof(head), "Chats");
    title(d, head, kBackW + 1, W - kNewW - 1);
    button(d, W - kNewW, 0, kNewW, kHeaderH, "New");

    if (_rowCount == 0) {
      d.setColor(UIColor::secondary_txt);
      d.drawTextCentered(W / 2, kHeaderH + 30, "No channels or chats yet");
      return;
    }
    const int top = kHeaderH + 1;
    for (int i = 0; i < _rowCount; i++) {
      const int y = top + i * _rowH - _listScroll;
      if (y < top || y + _rowH > H) continue;   // whole rows only
      drawRow(d, h, _rows[i], y, W, lh, now_ms);
    }
    drawScrollMark(d, W, top, _view, _listScroll);
  }

  void drawRow(DisplayDriver& d, const History& h, const ChatRow& r, int y, int W, int lh, uint64_t now_ms) const {
    d.setColor(UIColor::window_bkg);
    d.fillRect(1, y + 1, W - 2, _rowH - 3);
    d.setColor(UIColor::title_bkg);
    d.drawRect(0, y, W, _rowH - 1);
    // Title: "# Public" / "@ Alice"; unread conversations lit.
    char name[40], shown[40];
    convTitle(name, sizeof(name), r);
    d.translateUTF8ToBlocks(shown, name, sizeof(shown));
    const Entry* newest = r.newest_age >= 0 ? h.newest((size_t)r.newest_age) : nullptr;
    char right[28] = "";
    if (newest != nullptr) ageText(right, sizeof(right), now_ms, newest->heard_millis);
    const int rw = right[0] ? d.getTextWidth(right) : 0;
    d.setColor(r.unread > 0 ? UIColor::corp_blue : UIColor::primary_txt);
    d.drawTextEllipsized(5, y + 3, W - rw - 14, shown);
    d.setColor(UIColor::secondary_txt);
    if (right[0]) {
      d.setCursor(W - rw - 5, y + 3);
      d.print(right);
    }
    // Preview: "Sender: text" of the newest message, else a hint.
    char preview[sizeof(((Entry*)0)->message) + 40];
    if (newest == nullptr) {
      snprintf(preview, sizeof(preview), "%s", r.channel_idx >= 0 ? "No messages yet" : "No messages yet - tap to write");
    } else {
      char sender[36];
      const char* body = mesh::ui::chats::splitSender(*newest, sender, sizeof(sender));
      // A DM row is the peer already: only "You:" is said.
      if (sender[0] && (r.channel_idx >= 0 || newest->own_sent)) snprintf(preview, sizeof(preview), "%s: %s", sender, body);
      else snprintf(preview, sizeof(preview), "%s", body);
    }
    char shownPreview[sizeof(preview)];
    d.translateUTF8ToBlocks(shownPreview, preview, sizeof(shownPreview));
    mesh::ui::makeCompanionMessagePreviewSingleLine(shownPreview);
    char badge[24] = "";
    if (r.unread > 0) snprintf(badge, sizeof(badge), "%d new", r.unread);
    const int bw = badge[0] ? d.getTextWidth(badge) : 0;
    d.setColor(UIColor::secondary_txt);
    d.drawTextEllipsized(5, y + 3 + lh + 1, W - bw - 16, shownPreview);
    if (badge[0]) {
      d.setColor(UIColor::corp_blue);
      d.setCursor(W - bw - 5, y + 3 + lh + 1);
      d.print(badge);
    }
  }

  void drawScrollMark(DisplayDriver& d, int W, int top, int view, int s) const {
    if (_content <= view || view <= 0) return;
    int h = view * view / _content;
    if (h < 8) h = 8;
    const int y = top + (view - h) * s / (_content - view);
    d.setColor(UIColor::corp_blue);
    d.fillRect(W - 3, y, 2, h);
  }

  // ------------------------------------------------------------ thread

  struct Measure {
    DisplayDriver* d;
    int operator()(const char* s, size_t n) const {
      char tmp[8];
      if (n >= sizeof(tmp)) n = sizeof(tmp) - 1;
      memcpy(tmp, s, n);
      tmp[n] = 0;
      return d->getTextWidth(tmp);
    }
  };

  void renderThread(DisplayDriver& d, History& h, uint64_t now_ms, bool display_on) {
    const int W = d.width(), H = d.height();
    const int lh = d.textLineHeight();
    const int top = kHeaderH + 1, bottom = H - kBarH;
    const int textW = W - 12;
    const Measure measure{&d};
    _view = bottom - top;

    // Pass 1: the thread's height (oldest first).
    int content = 0, messages = 0;
    for (size_t age = h.count(); age-- > 0;) {
      const Entry* e = h.newest(age);
      if (e == nullptr || !shows(*e)) continue;
      content += messageHeight(d, *e, textW, lh, measure);
      messages++;
    }
    _content = content;
    if (_pinBottom) _scroll = maxScroll();
    _scroll = mesh::ui::chats::clampScroll(_scroll, _content, _view);

    button(d, 0, 0, kBackW, kHeaderH, "Chats");
    char head[48], shownHead[48];
    convTitle(head, sizeof(head), _thread);
    d.translateUTF8ToBlocks(shownHead, head, sizeof(shownHead));
    title(d, shownHead, kBackW + 1, W);

    if (messages == 0) {
      d.setColor(UIColor::secondary_txt);
      d.drawTextCentered(W / 2, top + 30, "No messages yet");
    }
    // Pass 2: draw the lines that are wholly in the body.
    // A short thread sits on the bar (newest at the bottom, as chats do).
    int y = _content < _view ? bottom - _content : top - _scroll;
    for (size_t age = h.count(); age-- > 0;) {
      const Entry* e = h.newest(age);
      if (e == nullptr || !shows(*e)) continue;
      y = drawMessage(d, *e, y, top, bottom, W, textW, lh, now_ms, measure);
      if (display_on && e->unread) h.markRead(e);
    }
    drawScrollMark(d, W, top, _view, _scroll);

    // Bottom bar: Up, Write, Down.
    const bool canUp = _scroll > 0, canDown = _scroll < maxScroll();
    scrollButton(d, 0, bottom, kScrollW, kBarH, "Up", canUp);
    button(d, kScrollW + 2, bottom, W - 2 * kScrollW - 4, kBarH, "Write message", true);
    scrollButton(d, W - kScrollW, bottom, kScrollW, kBarH, "Down", canDown);
  }

  static void scrollButton(DisplayDriver& d, int x, int y, int w, int h, const char* label, bool enabled) {
    d.setColor(enabled ? UIColor::title_bkg : UIColor::window_bkg);
    d.fillRect(x + 1, y + 1, w - 2, h - 2);
    d.setColor(UIColor::secondary_txt);
    d.drawRect(x, y, w, h);
    d.setColor(enabled ? UIColor::title_txt : UIColor::secondary_txt);
    const int tw = d.getTextWidth(label);
    d.setCursor(x + (w - tw) / 2, y + (h - 8) / 2);
    d.print(label);
  }

  bool shows(const Entry& e) const {
    return (_visible == nullptr || _visible(e)) && mesh::ui::chats::inThread(e, _thread);
  }

  static constexpr int kGap = 4;

  int messageHeight(DisplayDriver& d, const Entry& e, int textW, int lh, const Measure& m) const {
    char sender[36];
    char shown[sizeof(e.message)];
    d.translateUTF8ToBlocks(shown, mesh::ui::chats::splitSender(e, sender, sizeof(sender)), sizeof(shown));
    return lh + mesh::ui::chats::countLines(shown, textW, m) * lh + kGap;
  }

  int drawMessage(DisplayDriver& d, const Entry& e, int y, int top, int bottom, int W, int textW, int lh,
                  uint64_t now_ms, const Measure& m) const {
    char sender[36], shownSender[36];
    char shown[sizeof(e.message)];
    d.translateUTF8ToBlocks(shown, mesh::ui::chats::splitSender(e, sender, sizeof(sender)), sizeof(shown));
    d.translateUTF8ToBlocks(shownSender, sender[0] ? sender : "?", sizeof(shownSender));
    if (y >= top && y + lh <= bottom) {
      char age[28];
      ageText(age, sizeof(age), now_ms, e.heard_millis);
      const int aw = d.getTextWidth(age);
      d.setColor(e.own_sent ? UIColor::warning_txt : UIColor::corp_blue);
      d.drawTextEllipsized(4, y, W - aw - 16, shownSender);
      d.setColor(UIColor::secondary_txt);
      d.setCursor(W - aw - 6, y);
      d.print(age);
    }
    y += lh;
    const char* p = shown;
    if (*p == 0) return y + lh + kGap;
    while (*p) {
      const char* next = p;
      const size_t len = mesh::ui::chats::nextLine(p, textW, m, &next);
      if (y >= top && y + lh <= bottom && len > 0) {
        char line[sizeof(e.message)];
        memcpy(line, p, len);
        line[len] = 0;
        d.setColor(UIColor::primary_txt);
        d.setCursor(8, y);
        d.print(line);
      }
      y += lh;
      if (next == p) break;
      p = next;
    }
    return y + kGap;
  }

  static constexpr int kBackW = 52;
  static constexpr int kNewW = 48;
  static constexpr int kScrollW = 44;

  Mode _mode = Mode::List;
  ChatRow _rows[kMaxRows];
  int _rowCount = 0;
  int _rowH = 26;
  ChatRow _thread = {};
  int _scroll = 0, _listScroll = 0;
  int _content = 0, _view = 1;
  bool _pinBottom = true;
  bool _down = false, _drag = false;
  int _x0 = 0, _y0 = 0, _scroll0 = 0;
  int _composeChannel = kNoTarget;
  char _composePeer[34] = "";
  bool (*_visible)(const Entry&) = nullptr;
};
