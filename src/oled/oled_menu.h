#pragma once
#include <Arduino.h>
#include "config.h"

#if OLED_ENABLE
#include "oled_display.h"

#define MENU_MAX_ITEMS 15

class OledMenu {
public:
    typedef const char* (*GetValueFn)();
    typedef void (*ActionFn)();

    struct Item {
        const char* label;
        GetValueFn getValue;
        ActionFn onOk;
        ActionFn onLongOk;
    };

    OledMenu(OledDisplay &display);

    void clear();
    void addItem(const char* label, GetValueFn getValue = nullptr,
                 ActionFn onOk = nullptr, ActionFn onLongOk = nullptr);

    void show();
    void hide();
    bool active() const { return _active; }

    void up();
    void down();
    void ok(bool longPress = false);
    void cancel();

    void draw();
    bool tick();

    void setTimeout(unsigned long ms) { _timeoutMs = ms; }

private:
    OledDisplay &_display;
    Item _items[MENU_MAX_ITEMS];
    uint8_t _count;
    uint8_t _cursor;
    int8_t _scroll;
    bool _active;
    unsigned long _lastActivity;
    unsigned long _timeoutMs;

    static const uint8_t VISIBLE = 5;
    static const unsigned long DEFAULT_TIMEOUT_MS = 15000;
};
#endif
