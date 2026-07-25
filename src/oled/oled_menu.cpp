#include "oled_menu.h"

#if OLED_ENABLE

OledMenu::OledMenu(OledDisplay &display)
    : _display(display), _count(0), _cursor(0), _scroll(0),
      _active(false), _lastActivity(0), _timeoutMs(DEFAULT_TIMEOUT_MS) {}

void OledMenu::clear() {
    _count = 0;
    _cursor = 0;
    _scroll = 0;
}

void OledMenu::addItem(const char* label, GetValueFn getValue,
                       ActionFn onOk, ActionFn onLongOk) {
    if (_count >= MENU_MAX_ITEMS) return;
    _items[_count].label = label;
    _items[_count].getValue = getValue;
    _items[_count].onOk = onOk;
    _items[_count].onLongOk = onLongOk;
    _count++;
}

void OledMenu::show() {
    _active = true;
    _cursor = 0;
    _scroll = 0;
    _lastActivity = millis();
}

void OledMenu::hide() {
    _active = false;
}

void OledMenu::up() {
    if (!_active || _count == 0) return;
    _lastActivity = millis();
    if (_cursor > 0) {
        _cursor--;
    } else {
        _cursor = _count - 1;
    }
    if (_cursor < (uint8_t)_scroll) _scroll = _cursor;
    if (_cursor >= _scroll + VISIBLE) _scroll = _cursor - VISIBLE + 1;
}

void OledMenu::down() {
    if (!_active || _count == 0) return;
    _lastActivity = millis();
    if (_cursor < _count - 1) {
        _cursor++;
    } else {
        _cursor = 0;
    }
    if (_cursor < (uint8_t)_scroll) _scroll = _cursor;
    if (_cursor >= _scroll + VISIBLE) _scroll = _cursor - VISIBLE + 1;
}

void OledMenu::ok(bool longPress) {
    if (!_active || _cursor >= _count) return;
    _lastActivity = millis();
    Item &item = _items[_cursor];
    if (longPress && item.onLongOk) {
        item.onLongOk();
    } else if (item.onOk) {
        item.onOk();
    }
}

void OledMenu::cancel() {
    if (!_active) return;
    hide();
}

void OledMenu::draw() {
    if (!_active) return;

    auto &d = _display.display();
    d.clearDisplay();
    d.setTextColor(SSD1306_WHITE);
    d.setFont(NULL);
    d.setTextSize(1);

    int16_t x1, y1;
    uint16_t w, h;

    // Title
    d.getTextBounds(" SETTINGS ", 0, 0, &x1, &y1, &w, &h);
    d.setCursor((128 - w) / 2, 2);
    d.print(" SETTINGS ");

    for (uint8_t i = 0; i < VISIBLE; i++) {
        int idx = _scroll + i;
        if (idx >= _count) break;

        Item &item = _items[idx];
        int y = 17 + i * 10;

        if (idx == _cursor) {
            d.setCursor(2, y);
            d.print(">");
        }

        char buf[22];
        const char *val = item.getValue ? item.getValue() : "";
        char valTrunc[7];
        strncpy(valTrunc, val, sizeof(valTrunc) - 1);
        valTrunc[sizeof(valTrunc) - 1] = '\0';
        snprintf(buf, sizeof(buf), "%-11s%6s", item.label, valTrunc);

        int x = 12;
        d.getTextBounds(buf, 0, 0, &x1, &y1, &w, &h);
        if (w > 128 - x) {
            int maxW = 128 - x - 1;
            int labelLen = strlen(item.label);
            int maxChars = (maxW - 3) / 6;
            if (maxChars < 1) maxChars = 1;
            int labelChars = labelLen;
            if (labelChars > maxChars) labelChars = maxChars;
            snprintf(buf, sizeof(buf), "%-*s%6s", maxChars, item.label, valTrunc);
        }
        d.setCursor(x, y);
        d.print(buf);
    }

    if (_count > VISIBLE) {
        bool canScrollUp = _scroll > 0;
        bool canScrollDown = (_scroll + VISIBLE) < _count;
        if (canScrollUp && canScrollDown) {
            d.fillTriangle(120, 20, 116, 24, 124, 24, SSD1306_WHITE);
            d.fillTriangle(120, 62, 116, 58, 124, 58, SSD1306_WHITE);
        } else if (canScrollUp) {
            d.fillTriangle(120, 20, 116, 24, 124, 24, SSD1306_WHITE);
        } else if (canScrollDown) {
            d.fillTriangle(120, 62, 116, 58, 124, 58, SSD1306_WHITE);
        }
    }

    d.display();
}

bool OledMenu::tick() {
    if (!_active) return true;
    if (millis() - _lastActivity >= _timeoutMs) {
        hide();
        return false;
    }
    return true;
}

#endif
