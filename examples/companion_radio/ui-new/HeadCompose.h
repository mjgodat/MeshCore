#pragma once

// Touch compose screen for a repeater head (UI_REPEATER_HEAD): pick a channel
// or contact, type on the on-screen keyboard, and send. Channel posts can be
// sent as the head or under a repeater's name ("Send as").
//
// Layout is in the display's logical coordinates (160x120 on the CYD, drawn
// at 2x). Keys react to a touch-down; the next key needs a release.

#include <helpers/ui/UIScreen.h>
#include <helpers/ui/DisplayDriver.h>
#include <helpers/AdvertDataHelpers.h>
#include "../MyMesh.h"
#include "TouchKeyboard.h"

extern MyMesh the_mesh;

class HeadComposeScreen : public UIScreen {
 public:
  using CloseFn = void (*)(void* ctx, const char* result);

  HeadComposeScreen(CloseFn on_close, void* ctx) : _on_close(on_close), _ctx(ctx) {}

  // Rebuild the destination and sender lists from current channels/contacts.
  void open() {
    _status = nullptr;
    _dest_count = 0;
    for (int i = 0; i < MAX_GROUP_CHANNELS && _dest_count < kMaxDest; i++) {
      ChannelDetails ch;
      if (!the_mesh.getChannel(i, ch) || ch.name[0] == 0) continue;
      Dest& d = _dest[_dest_count++];
      d.channel_idx = i;
      strncpy(d.name, ch.name, sizeof(d.name) - 1);
      d.name[sizeof(d.name) - 1] = 0;
    }
    _sender_count = 1;  // index 0 = the head itself
    const int n = the_mesh.getNumContacts();
    for (int i = 0; i < n; i++) {
      ContactInfo c;
      if (!the_mesh.getContactByIdx(MAX_ANON_CONTACTS + i, c)) continue;
      if (c.type == ADV_TYPE_CHAT && _dest_count < kMaxDest) {
        Dest& d = _dest[_dest_count++];
        d.channel_idx = -1;
        memcpy(d.pub_prefix, c.id.pub_key, sizeof(d.pub_prefix));
        snprintf(d.name, sizeof(d.name), "@%s", c.name);
      } else if (c.type == ADV_TYPE_REPEATER && _sender_count < kMaxSenders) {
        strncpy(_senders[_sender_count], c.name, sizeof(_senders[0]) - 1);
        _senders[_sender_count][sizeof(_senders[0]) - 1] = 0;
        _sender_count++;
      }
    }
    if (_dest_idx >= _dest_count) _dest_idx = 0;
    if (_sender_idx >= _sender_count) _sender_idx = 0;
    _keyboard.reset("", maxLen());
    _touch_down = false;
  }

  int render(DisplayDriver& d) override {
    d.setTextSize(1);
    char buf[48];
    const char* dest = _dest_count ? _dest[_dest_idx].name : "(no channels)";
    snprintf(buf, sizeof(buf), "To:%s", dest);
    drawHeaderButton(d, 0, kSplitX, buf);
    snprintf(buf, sizeof(buf), "As:%s", isChannelDest() ? senderName() : the_mesh.headNodeName());
    drawHeaderButton(d, kSplitX, d.width() - kSplitX, buf);

    if (_status) {
      d.setColor(UIColor::warning_txt);
      d.setCursor(2, kHeaderH + 3);
      d.print(_status);
    } else {
      d.setColor(UIColor::primary_txt);
      _keyboard.drawText(d, kHeaderH + 3);
    }
    d.setColor(UIColor::secondary_txt);
    d.drawRect(0, kHeaderH + 1, d.width(), kTextH - 2);
    _keyboard.draw(d, kHeaderH + kTextH, kKeyH, "Send");
    return 1000;
  }

  bool handleInput(char c) override {
    if (c == KEY_CANCEL) { close(nullptr); return true; }
    return true;  // keep button navigation from leaking to other screens
  }

  // Raw touch in logical coordinates. Returns true when the screen changed.
  bool handleTouch(bool touched, int x, int y, int width) {
    if (!touched) { _touch_down = false; return false; }
    if (_touch_down) return false;
    _touch_down = true;
    if (y < kHeaderH) {
      _status = nullptr;
      if (x < kSplitX) {
        if (_dest_count) _dest_idx = (_dest_idx + 1) % _dest_count;
      } else if (!isChannelDest()) {
        _status = "DMs always go as this head";
      } else if (_sender_count < 2) {
        _status = "No repeater heard yet";
      } else {
        _sender_idx = (_sender_idx + 1) % _sender_count;
      }
      _keyboard.setMaxLength(maxLen());
      return true;
    }
    switch (_keyboard.touch(x, y, width, kHeaderH + kTextH, kKeyH)) {
      case TouchKeyboard::Result::Changed: _status = nullptr; return true;
      case TouchKeyboard::Result::Submit: send(); return true;
      case TouchKeyboard::Result::Cancel: close(nullptr); return true;
      case TouchKeyboard::Result::None: break;
    }
    return false;
  }

 private:
  static constexpr int kMaxDest = 24;
  static constexpr int kMaxSenders = 8;
  // Header halves are buttons: "To" (left) and "As" (right). Keep them tall
  // enough to hit on a resistive panel.
  static constexpr int kHeaderH = 18;
  static constexpr int kTextH = 14;
  static constexpr int kKeyH = 22;  // 4 rows fill 120 - 18 - 14 = 88
  static constexpr int kSplitX = 80;

  struct Dest { int channel_idx; uint8_t pub_prefix[6]; char name[34]; };

  bool isChannelDest() const { return _dest_count && _dest[_dest_idx].channel_idx >= 0; }
  const char* senderName() const {
    return _sender_idx == 0 ? the_mesh.headNodeName() : _senders[_sender_idx];
  }

  // Channel text travels as "<sender>: <text>" within MAX_TEXT_LEN.
  size_t maxLen() const {
    if (!isChannelDest()) return MAX_TEXT_LEN;
    const size_t prefix = strlen(senderName()) + 2;
    return prefix < MAX_TEXT_LEN ? MAX_TEXT_LEN - prefix : 0;
  }

  void drawHeaderButton(DisplayDriver& d, int x, int w, const char* label) const {
    d.setColor(UIColor::title_bkg);
    d.fillRect(x + 1, 1, w - 2, kHeaderH - 2);
    d.setColor(UIColor::secondary_txt);
    d.drawRect(x, 0, w, kHeaderH);
    d.setColor(UIColor::title_txt);
    d.drawTextEllipsized(x + 3, (kHeaderH - 8) / 2, w - 6, label);
  }

  void send() {
    if (_keyboard.length() == 0 || _dest_count == 0) return;
    const Dest& d = _dest[_dest_idx];
    bool ok;
    if (d.channel_idx >= 0) {
      ok = the_mesh.headSendChannel(d.channel_idx, senderName(), _keyboard.text());
    } else {
      ContactInfo* c = the_mesh.lookupContactByPubKey(d.pub_prefix, sizeof(d.pub_prefix));
      ok = c != nullptr && the_mesh.headSendDirect(*c, _keyboard.text());
    }
    if (ok) {
      close("Sent");
    } else {
      _status = _keyboard.length() > maxLen() ? "Too long for this name" : "Send failed";
    }
  }

  void close(const char* result) {
    if (_on_close) _on_close(_ctx, result);
  }

  CloseFn _on_close;
  void* _ctx;
  TouchKeyboard _keyboard;
  bool _touch_down = false;
  const char* _status = nullptr;
  Dest _dest[kMaxDest];
  int _dest_count = 0;
  int _dest_idx = 0;
  char _senders[kMaxSenders][32] = {};
  int _sender_count = 1;
  int _sender_idx = 0;
};
