#include "M5Dial.h"
#include "USB.h"
#include "USBHIDKeyboard.h"

#define KEY_LR_MODE // Use arrow keys for left/right instead of 'a'/'d'

USBHIDKeyboard Keyboard;

static long prev_pos = 0;
static bool shift_lock = false;       // one-shot sticky Shift for next operation
static int16_t last_touch_zone = -1;  // -1: none, 0: ESC, 1: Shift

static uint32_t press_start_ms = 0;
static uint32_t last_delete_ms = 0;
static bool delete_mode_active = false;

// Colors (RGB565)
static constexpr uint16_t COLOR_ESC = 0xF800;   // red
static constexpr uint16_t COLOR_SHIFT = 0x001F; // blue
static constexpr uint16_t COLOR_TEXT = 0xFFFF;  // white

static inline uint16_t darken(uint16_t c, float factor) {
    uint8_t r = (c >> 11) & 0x1F;
    uint8_t g = (c >> 5) & 0x3F;
    uint8_t b = (c) & 0x1F;
    r = (uint8_t)(r * factor);
    g = (uint8_t)(g * factor);
    b = (uint8_t)(b * factor);
    return (uint16_t)((r << 11) | (g << 5) | b);
}

static void draw_keys(int pressed_zone) {
    auto& d = M5Dial.Display;
    int w = d.width();
    int h = d.height();
    int hw = w / 2;

    uint16_t esc_bg = COLOR_ESC;
    uint16_t sh_base = shift_lock ? (uint16_t)0x021F : COLOR_SHIFT;
    uint16_t sh_bg = (pressed_zone == 1) ? darken(sh_base, 0.7f) : sh_base;
    if (pressed_zone == 0) esc_bg = darken(esc_bg, 0.7f);

    d.fillRect(0, 0, hw, h, esc_bg);
    d.fillRect(hw, 0, w - hw, h, sh_bg);

    d.setTextColor(COLOR_TEXT);
    d.setTextDatum(middle_center);
    d.setTextSize(2);
    d.drawString("ESC", hw / 2, h / 2);
    if (shift_lock) {
        int cy = h / 2;
        d.drawString("Shift", hw + (w - hw) / 2, cy - 12);
        d.drawString("Lock",  hw + (w - hw) / 2, cy + 12);
    } else {
        d.drawString("Shift", hw + (w - hw) / 2, h / 2);
    }
}

void setup() {
    auto cfg = M5.config();
    M5Dial.begin(cfg, true, false);

    // Start USB HID keyboard
    USB.begin();
    Keyboard.begin();

    prev_pos = M5Dial.Encoder.read();

    Serial.begin(115200);
    Serial.println("M5Dial -> USB Keyboard ready");

    draw_keys(-1);
}

static inline void sendKey(char c) {
    Keyboard.write((uint8_t)c);
    delay(2);  // brief pause for host to process
}

static inline void sendEsc() {
    Keyboard.press(KEY_ESC);
    delay(2);
    Keyboard.release(KEY_ESC);
    delay(2);
}

static inline void sendAsciiWithOptionalShift(uint8_t c) {
    if (shift_lock) {
        Keyboard.press(KEY_LEFT_SHIFT);
        delay(1);
    }
    Keyboard.write(c);
    delay(2);
    if (shift_lock) {
        Keyboard.release(KEY_LEFT_SHIFT);
        shift_lock = false;
        draw_keys(-1);
    }
}


static inline void sendDelete() {
    Keyboard.press(KEY_DELETE);
    delay(2);
    Keyboard.release(KEY_DELETE);
    delay(2);
}

void loop() {
    M5Dial.update();

    auto t = M5Dial.Touch.getDetail();
    int w = M5Dial.Display.width();
    int zone = -1;
    if (t.state != m5::touch_state_t::none) {
        if (t.x >= 0 && t.x < w) {
            zone = (t.x < (w / 2)) ? 0 : 1; // 0: ESC, 1: Shift
        }
    }

    if (zone != last_touch_zone) {
        last_touch_zone = zone;
        draw_keys(zone);
    }

    if (t.state == m5::touch_state_t::touch_begin) {
        if (zone == 0) {
            sendEsc();
            Serial.println("Touch ESC -> ESC");
        } else if (zone == 1) {
            shift_lock = !shift_lock;
            Serial.printf("Touch Shift -> %s\n", shift_lock ? "lock" : "unlock");
            draw_keys(zone);
        }
    }

    // On touch end, redraw to clear pressed state
    if (t.state == m5::touch_state_t::touch_end) {
        draw_keys(-1);
    }

    long curr = M5Dial.Encoder.read();
    long delta = curr - prev_pos;
    if (delta != 0) {
        if (delta > 0) {
            long steps = delta / 4;
            for (long i = 0; i < steps; ++i) {
                if (shift_lock) {
                    sendAsciiWithOptionalShift('d');
                } else {
#if defined(KEY_LR_MODE)
                    Keyboard.press(KEY_RIGHT_ARROW);
                    delay(2);
                    Keyboard.release(KEY_RIGHT_ARROW);
                    delay(2);
#else
                    sendKey('d');
#endif
                }
            }
            if (steps > 0) Serial.printf("Rotated right: %ld (sent %ld d)\n", delta, steps);
            prev_pos += steps * 4;
        } else {
            long steps = (-delta) / 4;
            for (long i = 0; i < steps; ++i) {
                if (shift_lock) {
                    sendAsciiWithOptionalShift('a');
                } else {
#if defined(KEY_LR_MODE)
                    Keyboard.press(KEY_DOWN_ARROW);
                    delay(2);
                    Keyboard.release(KEY_DOWN_ARROW);
                    delay(2);
#else
                    sendKey('a');
#endif
                }
            }
            if (steps > 0) Serial.printf("Rotated left: %ld (sent %ld a)\n", -delta, steps);
            prev_pos -= steps * 4;
        }
    }

    // Handle button: short press -> Enter, long press (>3s) -> repeat Delete every 0.5s while held
    if (delete_mode_active && M5Dial.BtnA.wasReleased()) {
        delete_mode_active = false;
        last_delete_ms = 0;
    }

    if (M5Dial.BtnA.wasPressed()) {
        press_start_ms = millis();
    }

    if (!delete_mode_active && M5Dial.BtnA.pressedFor(3000)) {
        delete_mode_active = true;
        sendDelete();
        last_delete_ms = millis();
        Serial.println("Long press -> start repeating Delete");
    }

    if (delete_mode_active) {
        if (!M5Dial.BtnA.wasReleased()) {
            uint32_t now = millis();
            if (now - last_delete_ms >= 500) {
                sendDelete();
                last_delete_ms = now;
            }
        }
    }

    if (!delete_mode_active && M5Dial.BtnA.wasReleased()) {
        uint32_t held_ms = press_start_ms ? (millis() - press_start_ms) : 0;
        if (held_ms < 3000) {
            sendAsciiWithOptionalShift('\n');
            Serial.println("Button click -> Enter");
        }
        press_start_ms = 0;
    }

    delay(1);
}
