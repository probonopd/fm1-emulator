/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_PI32V2_FM1_PANEL_H
#define HW_PI32V2_FM1_PANEL_H

#include "qemu/timer.h"
#include "system/runstate.h"
#include "ui/console.h"
#include "ui/input.h"
#include "ui/fm1-controls.h"
#include "fm1-input.h"
#include "fm1-lcd.h"

/* Buttons, piano keys, the encoders and MASTER. */
#define FM1_PANEL_CONTROLS (FM1_PANEL_BUTTONS + FM1_PANEL_KEYS + \
                            FM1_PANEL_ENCODERS + 1)

typedef struct FM1PanelControl {
    float x, y, w, h;          /* in the product photo's pixels */
    const char *caption;
    bool piano, knob, dirty;
    QKeyCode contact;
    bool pressed;
    /* Matrix LEDs: [0] the contact's own, [1] PLAY's extra green one. */
    unsigned leds, led_column[2], led_row[2];
    float led_level[2];        /* smoothed, 0 to 255 */
    int encoder;               /* knobs: fm1_panel_encoders index, -1 MASTER */
    double angle;
    int pending, direction, phase;
    QEMUTimer *phase_timer;
} FM1PanelControl;

/* One LCD pixel's share of one display pixel, along one axis. */
typedef struct FM1PanelTap {
    int first, count;
    float weight[8];
} FM1PanelTap;

/* The FM-1 front panel drawn as the machine's one display, at the size the
 * display asks for. Controls close board contacts through QEMU's input
 * layer, as keys and QMP do; any QEMU display (SDL, VNC, ...) shows it and
 * sends it the pointer. */
typedef struct FM1PocPanel {
    QemuConsole *console;
    QemuInputHandlerState *handler;
    VMChangeStateEntry *runstate;
    FM1PocLCD *lcd;
    FM1PocInput *input;
    int width, height;
    float scale, x0, y0;       /* display pixel = x0 + photo pixel * scale */
    uint32_t *background;
    FM1PanelTap *lcd_taps;
    int lcd_x, lcd_y, lcd_size;
    FM1PanelControl controls[FM1_PANEL_CONTROLS];
    FM1PanelControl *held, *dragged;
    QEMUTimer *release_timer;
    int64_t pressed_at;
    int x, y, drag_x, drag_y;
    double drag_remainder;
    int master;
    bool dirty;
} FM1PocPanel;

void fm1_panel_init(FM1PocPanel *panel, FM1PocLCD *lcd, FM1PocInput *input);

#endif
