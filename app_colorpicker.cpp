/*
 * app_colorpicker.cpp — HSV colour picker
 *
 * Portrait 368×448, direct gfx.
 * Full-screen colour fill with HEX + HSV overlay at bottom.
 *
 * Controls:
 *   Touch drag left/right – change hue
 *   Touch drag up/down   – change saturation
 *   BOOT short press     – step value (brightness) +10%
 *   PWR  short press     – reset to defaults
 */

#include "app_colorpicker.h"
#include "app_common.h"
#include <Arduino.h>
#include <math.h>
#include "canvas/Arduino_Canvas.h"
#include "pin_config.h"
#include "HWCDC.h"
#include "TouchDrvFT6X36.hpp"

extern USBCDC USBSerial;
extern Arduino_Canvas *g_canvas;

#define BOOT_BTN    0
#define PWR_POLL_MS 50

static Arduino_Canvas   *canvas     = nullptr;
static TouchDrvFT6X36    s_touch;
static float             s_h        = 0.0f;   // 0-360
static float             s_s        = 1.0f;   // 0-1
static float             s_v        = 1.0f;   // 0-1
static bool              s_bootWas  = false;
static uint32_t          s_lastPwr  = 0;
static int16_t           s_lastTx   = -1;
static int16_t           s_lastTy   = -1;
static uint32_t          s_hintEnd  = 0;
static bool              s_hintActive = false;

// ── HSV → RGB888 → RGB565 ─────────────────────────────────────────────────────
static void hsv2rgb(float h, float sv, float v, uint8_t &r, uint8_t &g, uint8_t &b) {
    float c  = v * sv;
    float x  = c * (1.0f - fabsf(fmodf(h / 60.0f, 2.0f) - 1.0f));
    float m  = v - c;
    float r1, g1, b1;
    int   hi = (int)(h / 60.0f) % 6;
    switch (hi) {
        case 0: r1=c; g1=x; b1=0; break;
        case 1: r1=x; g1=c; b1=0; break;
        case 2: r1=0; g1=c; b1=x; break;
        case 3: r1=0; g1=x; b1=c; break;
        case 4: r1=x; g1=0; b1=c; break;
        default:r1=c; g1=0; b1=x; break;
    }
    r = (uint8_t)((r1 + m) * 255.0f);
    g = (uint8_t)((g1 + m) * 255.0f);
    b = (uint8_t)((b1 + m) * 255.0f);
}

static uint16_t hsv565(float h, float sv, float v) {
    uint8_t r, g, b;
    hsv2rgb(h, sv, v, r, g, b);
    return ((uint16_t)(r >> 3) << 11) | ((uint16_t)(g >> 2) << 5) | (uint16_t)(b >> 3);
}

static void draw() {
    uint8_t r, g, b;
    hsv2rgb(s_h, s_s, s_v, r, g, b);
    uint16_t col = ((uint16_t)(r >> 3) << 11) | ((uint16_t)(g >> 2) << 5) | (uint16_t)(b >> 3);

    // ── Layout zones ─────────────────────────────────────────────────
    // Top 10 % = rainbow strip, edge-to-edge, no rounded card.
    // Middle 70 % = solid colour swatch.
    // Bottom 20 % = pure black info zone, no rounded card.
    const int16_t hueH    = LCD_HEIGHT * 10 / 100;     // 44
    const int16_t infoY   = 318;                       // moved up so watermark stays clear
    const int16_t infoH   = LCD_HEIGHT - infoY;        // 130

    // Solid colour fill across the middle
    canvas->fillRect(0, hueH, LCD_WIDTH, infoY - hueH, col);

    // ── Rainbow strip ─────────────────────────────────────────────────
    for (int x = 0; x < LCD_WIDTH; x++) {
        float hue = (float)x / (float)LCD_WIDTH * 360.0f;
        uint16_t hc = hsv565(hue, 1.0f, 1.0f);
        canvas->drawFastVLine(x, 0, hueH, hc);
    }
    // Hue cursor (small inverted triangle pointing down into the colour zone)
    int16_t hx = (int16_t)(s_h / 360.0f * LCD_WIDTH);
    if (hx < 4)               hx = 4;
    if (hx > LCD_WIDTH - 5)   hx = LCD_WIDTH - 5;
    canvas->drawFastVLine(hx,     0, hueH, 0x0000);
    canvas->drawFastVLine(hx + 1, 0, hueH, 0x0000);
    canvas->fillTriangle(hx - 5, hueH, hx + 5, hueH, hx, hueH + 6, 0x0000);

    // ── Bottom info zone (black, no card) ────────────────────────────
    canvas->fillRect(0, infoY, LCD_WIDTH, infoH, 0x0000);

    // HEX value (size 3, accent in the chosen colour)
    char hexBuf[12];
    snprintf(hexBuf, sizeof(hexBuf), "#%02X%02X%02X", r, g, b);
    canvas->setTextSize(3);
    canvas->setTextColor(col);
    canvas->setCursor((LCD_WIDTH - (int16_t)(strlen(hexBuf) * 18)) / 2, infoY + 6);
    canvas->print(hexBuf);

    // RGB / HSV stacked, two compact rows below the HEX
    char rgbBuf[24];
    snprintf(rgbBuf, sizeof(rgbBuf), "R:%3d G:%3d B:%3d", r, g, b);
    canvas->setTextSize(2);
    canvas->setTextColor(0xFFFF);
    canvas->setCursor((LCD_WIDTH - (int16_t)(strlen(rgbBuf) * 12)) / 2, infoY + 38);
    canvas->print(rgbBuf);

    char hsvBuf[24];
    snprintf(hsvBuf, sizeof(hsvBuf), "H:%3d S:%2d%% V:%2d%%",
             (int)s_h, (int)(s_s * 100), (int)(s_v * 100));
    canvas->setTextColor(0xC618);
    canvas->setCursor((LCD_WIDTH - (int16_t)(strlen(hsvBuf) * 12)) / 2, infoY + 60);
    canvas->print(hsvBuf);

    // Pill labels anchored to hardware buttons
    draw_pill_label(canvas, 0, 0, "V+");
    draw_pill_label(canvas, 0, 1, "V-");

    draw_battery_g(canvas, LCD_WIDTH, LCD_HEIGHT);
    draw_watermark_g(canvas, LCD_WIDTH, LCD_HEIGHT);

    // 3 s touch-control hint on startup
    if (s_hintActive) {
        int16_t cx = LCD_WIDTH / 2;
        int16_t cy = 170;
        int16_t bw = 260, bh = 180;
        int16_t bx = cx - bw / 2, by = cy - bh / 2;
        canvas->fillRoundRect(bx, by, bw, bh, 16, 0x0000);
        canvas->drawRoundRect(bx, by, bw, bh, 16, 0xFFFF);
        canvas->setTextSize(2);
        canvas->setTextColor(0xFFFF);
        const char *t = "DRAG";
        canvas->setCursor(cx - (int16_t)(strlen(t) * 6), by + 12);
        canvas->print(t);
        // Up arrow + SAT
        int16_t uy = cy - 36;
        canvas->fillTriangle(cx, uy, cx - 10, uy + 14, cx + 10, uy + 14, 0xFFFF);
        canvas->setTextSize(1);
        canvas->setTextColor(0xC618);
        canvas->setCursor(cx - 9, uy + 18);
        canvas->print("SAT");
        // Down arrow + SAT
        int16_t dy = cy + 36;
        canvas->fillTriangle(cx, dy + 14, cx - 10, dy, cx + 10, dy, 0xFFFF);
        canvas->setCursor(cx - 9, dy - 10);
        canvas->print("SAT");
        // Left arrow + HUE
        int16_t lx = cx - 80;
        canvas->fillTriangle(lx, cy, lx + 14, cy - 10, lx + 14, cy + 10, 0xFFFF);
        canvas->setCursor(lx + 18, cy - 4);
        canvas->print("HUE");
        // Right arrow + HUE
        int16_t rx = cx + 80;
        canvas->fillTriangle(rx, cy, rx - 14, cy - 10, rx - 14, cy + 10, 0xFFFF);
        canvas->setCursor(rx - 36, cy - 4);
        canvas->print("HUE");
    }

    canvas->flush();
}

void app_colorpicker_setup(Arduino_SH8601 *gfx) {
    (void)gfx;
    canvas    = g_canvas;
    s_h       = 0.0f;
    s_s       = 1.0f;
    s_v       = 1.0f;
    s_bootWas = false;
    s_lastPwr = 0;
    s_lastTx  = -1;
    s_lastTy  = -1;
    pinMode(BOOT_BTN, INPUT_PULLUP);
    // Wire already initialised by launcher / standalone .ino
    s_touch.begin(Wire, 0x38, IIC_SDA, IIC_SCL);
    s_hintActive = true;
    s_hintEnd    = millis() + 3000;
    draw();
}

void app_colorpicker_loop() {
    common_tick();
    uint32_t now = millis();

    // Dismiss the 3 s startup hint
    if (s_hintActive && now >= s_hintEnd) {
        s_hintActive = false;
        draw();
    }

    // Touch: drag for hue (L/R) and saturation (U/D)
    int16_t tx, ty;
    if (s_touch.getPoint(&tx, &ty, 1)) {
        if (s_hintActive) { s_hintActive = false; draw(); }
        if (s_lastTx >= 0) {
            int16_t dx = tx - s_lastTx;
            int16_t dy = ty - s_lastTy;
            if (abs(dx) >= abs(dy)) {
                s_h = fmodf(s_h + (float)dx * 0.2f + 360.0f, 360.0f);
            } else {
                s_s -= (float)dy / (float)(LCD_HEIGHT * 3);
                if (s_s > 1.0f) s_s = 1.0f;
                if (s_s < 0.0f) s_s = 0.0f;
            }
            if (abs(dx) > 3 || abs(dy) > 3) {
                common_activity();
                draw();
            }
        }
        s_lastTx = tx; s_lastTy = ty;
    } else {
        s_lastTx = -1; s_lastTy = -1;
    }

    // BOOT: brightness (value) up +10%
    bool boot = (digitalRead(BOOT_BTN) == LOW);
    if (boot && !s_bootWas) {
        common_activity();
        s_v += 0.1f;
        if (s_v > 1.0f) s_v = 1.0f;
        draw();
    }
    s_bootWas = boot;

    // PWR: brightness (value) down -10%
    if (common_consume_pwr_short()) {
        common_activity();
        s_v -= 0.1f;
        if (s_v < 0.1f) s_v = 0.1f;
        draw();
    }

    delay(10);
}
