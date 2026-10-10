/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The FM-1 front panel, drawn into the machine's display at the size the
 * display asks for, so it stays sharp in any window. Geometry and colours
 * follow the manufacturer's product photo: coordinates below are its pixels.
 * Nothing is copied from a bitmap. Controls emit ordinary input events into
 * the board, never guest commands. */
#include "qemu/osdep.h"
#include <math.h>
#include "fm1-panel.h"
#include "ui/fm1-leds.h"

/* The photo's panel and a margin around it. */
#define ORIGIN_X 280
#define ORIGIN_Y 118
#define DESIGN_W 1390
#define DESIGN_H 847
#define LCD_X 695
#define LCD_Y 245
#define LCD_SIZE 230
/* The window before the display reports its own size. */
#define DEFAULT_SCALE .8f

/* QEMU's input handlers carry no opaque pointer; there is one board. */
static FM1PocPanel *panel_instance;

typedef struct Color {
    float r, g, b;
} Color;

static Color gray(float value)
{
    return (Color) { value, value, value };
}

static Color rgb(uint32_t hex)
{
    return (Color) { (hex >> 16) / 255.f, ((hex >> 8) & 255) / 255.f, (hex & 255) / 255.f };
}

static Color mix(Color a, Color b, float t)
{
    return (Color) { a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t };
}

static void blend(uint32_t *pixel, Color c, float alpha)
{
    if (alpha <= 0) {
        return;
    }
    alpha = MIN(alpha, 1);
    Color under = rgb(*pixel);
    Color out = mix(under, c, alpha);
    *pixel = (uint32_t)(out.r * 255 + .5f) << 16 | (uint32_t)(out.g * 255 + .5f) << 8 |
             (uint32_t)(out.b * 255 + .5f);
}

/* Where to draw, and how photo pixels map onto display pixels. */
typedef struct Canvas {
    uint32_t *pixels;
    int stride, width, height;
    float scale, x0, y0;
} Canvas;

/* Fill (stroke 0) or outline an anti-aliased rounded rectangle, shaded from
 * top to bottom; blur softens its edge into a shadow. */
static void box(Canvas *c, float x, float y, float w, float h, float radius,
                Color top, Color bottom, float alpha, float stroke, float blur)
{
    float s = c->scale;
    x = c->x0 + x * s;
    y = c->y0 + y * s;
    w *= s;
    h *= s;
    radius = MIN(radius * s, MIN(w, h) / 2);
    stroke *= s;
    float edge = MAX(blur * s, 1);
    float cx = x + w / 2, cy = y + h / 2, hx = w / 2 - radius, hy = h / 2 - radius;
    int x0 = MAX(0, (int)floorf(x - stroke - edge)), y0 = MAX(0, (int)floorf(y - stroke - edge));
    int x1 = MIN(c->width, (int)ceilf(x + w + stroke + edge));
    int y1 = MIN(c->height, (int)ceilf(y + h + stroke + edge));

    for (int py = y0; py < y1; py++) {
        Color color = mix(top, bottom, CLAMP((py + .5f - y) / h, 0, 1));
        for (int px = x0; px < x1; px++) {
            float qx = fabsf(px + .5f - cx) - hx, qy = fabsf(py + .5f - cy) - hy;
            float d = hypotf(MAX(qx, 0), MAX(qy, 0)) + MIN(MAX(qx, qy), 0) - radius;
            float cover = stroke ? (stroke / 2 - fabsf(d)) / edge + .5f : .5f - d / edge;
            blend(&c->pixels[py * c->stride + px], color, MIN(cover, 1) * alpha);
        }
    }
}

static void fill(Canvas *c, float x, float y, float w, float h, float radius, Color color)
{
    box(c, x, y, w, h, radius, color, color, 1, 0, 0);
}

static void outline(Canvas *c, float x, float y, float w, float h, float radius,
                    Color color, float alpha, float stroke)
{
    box(c, x, y, w, h, radius, color, color, alpha, stroke, 0);
}

static void dot(Canvas *c, float cx, float cy, float r, Color color)
{
    fill(c, cx - r, cy - r, 2 * r, 2 * r, r, color);
}

/* Monoline capitals on a 4 by 6 grid, in the panel's squared lettering.
 * Points are "xy" grid digits, separated by spaces; '|' starts a new
 * stroke. Only the letters the panel prints are drawn. */
static const char *const glyphs[128] = {
    ['A'] = "06 01 10 30 41 46|03 43",
    ['B'] = "00 30 41 42 33 03|33 44 45 36 06 00",
    ['C'] = "41 30 10 01 05 16 36 45",
    ['D'] = "00 30 41 45 36 06 00",
    ['E'] = "40 00 06 46|03 33",
    ['F'] = "40 00 06|03 33",
    ['G'] = "41 30 10 01 05 16 36 45 43 23",
    ['H'] = "00 06|40 46|03 43",
    ['I'] = "10 30|20 26|16 36",
    ['K'] = "00 06|40 03 46",
    ['L'] = "00 06 46",
    ['M'] = "06 00 23 40 46",
    ['N'] = "06 00 46 40",
    ['O'] = "10 30 41 45 36 16 05 01 10",
    ['P'] = "06 00 30 41 42 33 03",
    ['Q'] = "10 30 41 45 36 16 05 01 10|34 46",
    ['R'] = "06 00 30 41 42 33 03|33 46",
    ['S'] = "41 30 10 01 02 13 33 44 45 36 16 05",
    ['T'] = "00 40|20 26",
    ['V'] = "00 03 26 43 40",
    ['X'] = "00 46|40 06",
    ['Y'] = "00 23 40|23 26",
    ['1'] = "11 20 26|16 36",
    ['2'] = "01 10 30 41 42 06 46",
    ['3'] = "01 10 30 41 42 33 13|33 44 45 36 16 05",
    ['4'] = "30 03 43|30 36",
    ['5'] = "40 00 03 33 44 45 36 06",
    ['6'] = "40 10 01 05 16 36 45 44 33 03",
    ['-'] = "13 33",
    ['+'] = "03 43|21 25",
};

typedef struct Segment {
    float ax, ay, bx, by;
} Segment;

/* Text centred on (cx, cy): cap is the capital height, wide stretches the
 * letters, advance is the distance between letters and weight the stroke
 * width, all in photo pixels. */
static void text(Canvas *c, const char *str, float cx, float cy, float cap, float wide,
                 float advance, float weight, Color color, float alpha, float blur)
{
    float s = c->scale, unit = cap / 6 * s, xunit = unit * wide;
    size_t length = strlen(str);
    float left = c->x0 + cx * s - ((length - 1) * advance * s + 4 * xunit) / 2;
    float top = c->y0 + cy * s - 3 * unit;
    Segment segments[256];
    unsigned count = 0;

    if (!length) {
        return;
    }
    for (size_t i = 0; i < length; i++) {
        const char *glyph = glyphs[str[i] & 127];
        float gx = left + i * advance * s;
        for (const char *p = glyph; p && *p; p++) {
            if (p[0] == ' ' || p[0] == '|') {
                continue;
            }
            /* Each point after the first in a stroke ends a segment. */
            if (p[2] == ' ' && count < ARRAY_SIZE(segments)) {
                segments[count++] = (Segment) {
                    gx + (p[0] - '0') * xunit, top + (p[1] - '0') * unit,
                    gx + (p[3] - '0') * xunit, top + (p[4] - '0') * unit,
                };
            }
            p++;
        }
    }
    float r = weight * s / 2, edge = MAX(blur * s, 1), margin = r + edge + 1;
    int x0 = MAX(0, (int)(left - margin)), y0 = MAX(0, (int)(top - margin));
    int x1 = MIN(c->width, (int)(left + (length - 1) * advance * s + 4 * xunit + margin));
    int y1 = MIN(c->height, (int)(top + 6 * unit + margin));
    for (int py = y0; py < y1; py++) {
        for (int px = x0; px < x1; px++) {
            float best = INFINITY, fx = px + .5f, fy = py + .5f;
            for (unsigned i = 0; i < count; i++) {
                const Segment *g = &segments[i];
                float dx = g->bx - g->ax, dy = g->by - g->ay;
                float u = CLAMP(((fx - g->ax) * dx + (fy - g->ay) * dy) /
                                MAX(dx * dx + dy * dy, 1e-6f), 0, 1);
                best = MIN(best, hypotf(fx - g->ax - u * dx, fy - g->ay - u * dy));
            }
            blend(&c->pixels[py * c->stride + px], color, MIN(.5f - (best - r) / edge, 1) * alpha);
        }
    }
}

/* LED brightness is the fraction of the time its column was selected that
 * its line was driven; the eye is roughly logarithmic, so a dim glow of a few
 * percent reads as clearly on and a full-duty LED as full. */
static float led_brightness(float level)
{
    return powf(MIN(level / 242.f, 1), .4f);
}

/* Function and octave buttons: backlit labels, no LED windows. */
static void draw_button(Canvas *c, const FM1PanelControl *b)
{
    float x = b->x, y = b->y, w = b->w, h = b->h, press = b->pressed ? 1.5f : 0;
    fill(c, x, y, w, h, 10, rgb(0x050505));
    box(c, x + 2, y + 2 + press, w - 4, h - 4 - press, 8,
        rgb(b->pressed ? 0x202020 : 0x2c2c2c), rgb(b->pressed ? 0x1a1a1a : 0x222222), 1, 0, 0);
    outline(c, x + 2.5f, y + 2.5f + press, w - 5, h - 5 - press, 7.5f, gray(1), .07f, 1);

    float lit = led_brightness(b->led_level[0]);
    bool rec = !strcmp(b->caption, "REC"), play = !strcmp(b->caption, "PLAY/STOP");
    Color color = mix(gray(.5f), rec ? rgb(0xff4433) : gray(1), lit);
    if (play) {
        color = mix(color, rgb(0x55ff77), led_brightness(b->led_level[1]));
    }
    float glow = MAX(lit, play ? led_brightness(b->led_level[1]) : 0);
    float cx = x + w / 2, cy = y + h / 2 + press;
    if (play) {
        text(c, "PLAY", cx, cy - 11, 10, 1.3f, 12.5f, 2.6f, color, .35f * glow, 5);
        text(c, "STOP", cx, cy + 11, 10, 1.3f, 12.5f, 2.6f, color, .35f * glow, 5);
        text(c, "PLAY", cx, cy - 11, 10, 1.3f, 12.5f, 2.4f, color, 1, 0);
        text(c, "STOP", cx, cy + 11, 10, 1.3f, 12.5f, 2.4f, color, 1, 0);
        fill(c, cx - 22, cy - 1, 44, 2, 1, color);
    } else {
        text(c, b->caption, cx, cy, 11, 1.35f, 14, 2.8f, color, .35f * glow, 5);
        text(c, b->caption, cx, cy, 11, 1.35f, 14, 2.6f, color, 1, 0);
    }
}

/* Silicone keys: the white bar is the key's LED window. */
static void draw_key(Canvas *c, const FM1PanelControl *k)
{
    float x = k->x, y = k->y, w = k->w, h = k->h, press = k->pressed ? 1.5f : 0;
    fill(c, x, y, w, h, w / 2, rgb(0x0c0c0c));
    box(c, x + 2.5f, y + 2.5f + press, w - 5, h - 5 - press, w / 2 - 2.5f,
        rgb(k->pressed ? 0x252525 : 0x303030), rgb(k->pressed ? 0x202020 : 0x272727), 1, 0, 0);
    outline(c, x + 3, y + 3 + press, w - 6, h - 6 - press, w / 2 - 3, gray(1), .06f, 1);

    float lit = led_brightness(k->led_level[0]), cx = x + w / 2;
    bool black = k->caption != NULL, label = black && *k->caption;
    float bar_w = black ? 7 : 8, bar_y = y + (black ? 29 : 30) + press;
    float bar_h = black ? (label ? 39 : 68) : 64;
    Color color = mix(rgb(0x3a3a3a), gray(1), lit);
    box(c, cx - bar_w / 2 - 3, bar_y - 3, bar_w + 6, bar_h + 6, bar_w / 2 + 3,
        color, color, .3f * lit, 0, 5);
    fill(c, cx - bar_w / 2, bar_y, bar_w, bar_h, bar_w / 2, color);
    if (label) {
        text(c, k->caption, cx, y + 84 + press, 9, 1, 10.5f, 1.4f, rgb(0xd0d0d0), 1, 0);
    }
}

/* Knurled knobs: the ridges turn with the knob; MASTER has a white dot. */
static void draw_knob(Canvas *c, const FM1PanelControl *k)
{
    float cx = k->x + k->w / 2, cy = k->y + k->h / 2;
    box(c, cx - 36, cy - 33, 72, 72, 36, gray(0), gray(0), .45f, 0, 6);

    float s = c->scale, px_cx = c->x0 + cx * s, px_cy = c->y0 + cy * s;
    float outer = 31 * s, inner = 24 * s, turn = k->angle * (float)G_PI / 180;
    int x0 = MAX(0, (int)(px_cx - outer - 1)), x1 = MIN(c->width, (int)(px_cx + outer + 2));
    int y0 = MAX(0, (int)(px_cy - outer - 1)), y1 = MIN(c->height, (int)(px_cy + outer + 2));
    for (int py = y0; py < y1; py++) {
        for (int px = x0; px < x1; px++) {
            float dx = px + .5f - px_cx, dy = py + .5f - px_cy, r = hypotf(dx, dy);
            float light = -(dx + dy) / (outer * 2.8f);
            float ridge = .5f + .5f * cosf(32 * (atan2f(dx, -dy) - turn));
            Color knurl = gray(.10f + .08f * ridge + .06f * light);
            Color face = gray(.24f + .08f * light);
            float rim = CLAMP(inner + .5f - r, 0, 1);
            blend(&c->pixels[py * c->stride + px], mix(knurl, face, rim),
                  CLAMP(outer + .5f - r, 0, 1));
        }
    }
    outline(c, cx - 24, cy - 24, 48, 48, 24, gray(0), .5f, 1.2f);
    if (k->encoder < 0) {
        dot(c, cx + sinf(turn) * 17, cy - cosf(turn) * 17, 3.5f, gray(.95f));
    }
}

static void draw_background(Canvas *c)
{
    static const struct { const char *text; float x, y; } labels[] = {
        { "MASTER", 419, 206 }, { "SELECT", 560, 206 },
        { "PRESETS", 419, 347 }, { "ALGORITHM", 561, 347 },
        { "KNOB1", 1062, 206 }, { "KNOB2", 1211, 206 },
        { "KNOB3", 1361, 206 }, { "KNOB4", 1511, 206 },
    };

    for (int i = 0; i < c->height; i++) {
        for (int j = 0; j < c->width; j++) {
            c->pixels[i * c->stride + j] = 0x1c1c1e;
        }
    }
    box(c, 305, 152, 1340, 797, 70, gray(0), gray(0), .55f, 0, 18);
    box(c, 305, 143, 1340, 797, 70, rgb(0x4c4c4c), rgb(0x5f5f5f), 1, 0, 0);
    outline(c, 305, 143, 1340, 797, 70, rgb(0x1e1e1e), .9f, 2);
    outline(c, 307, 145, 1336, 793, 68, gray(1), .05f, 1);
    for (unsigned i = 0; i < ARRAY_SIZE(labels); i++) {
        text(c, labels[i].text, labels[i].x, labels[i].y, 14, 1, 12.7f, 1.6f,
             rgb(0xa8a8a8), 1, 0);
    }
    /* Recesses: octave buttons, function buttons, keyboard. */
    box(c, 378, 485, 214, 66, 12, rgb(0x2e2e2e), rgb(0x383838), 1, 0, 0);
    outline(c, 378, 485, 214, 66, 12, rgb(0x1a1a1a), 1, 1.5f);
    box(c, 1018, 362, 542, 189, 20, rgb(0x343434), rgb(0x3d3d3d), 1, 0, 0);
    outline(c, 1018, 362, 542, 189, 20, rgb(0x1e1e1e), 1, 1.5f);
    dot(c, 1289, 458, 2.5f, gray(0));
    box(c, 350, 598, 1250, 294, 46, rgb(0x2a2b2b), rgb(0x323333), 1, 0, 0);
    outline(c, 350, 598, 1250, 294, 46, rgb(0x161616), 1, 2);
    for (unsigned i = 0; i < 4; i++) {
        static const float gaps[] = { 669, 899, 1203, 1431 };
        dot(c, gaps[i], 746, 2.5f, rgb(0x0d0d0d));
    }
    /* The LCD's bezel; the screen itself is drawn with the controls. */
    fill(c, 650, 201, 319, 318, 40, rgb(0x0b0c0c));
    outline(c, 651, 202, 317, 316, 39, gray(1), .06f, 1);
}

/* Each display pixel averages the LCD pixels it covers, so the screen keeps
 * crisp pixel edges at any size without uneven pixel widths. */
static void lcd_layout(FM1PocPanel *p)
{
    float s = p->scale;
    p->lcd_x = lroundf(p->x0 + LCD_X * s);
    p->lcd_y = lroundf(p->y0 + LCD_Y * s);
    p->lcd_size = MAX(1, lroundf(LCD_SIZE * s));
    g_free(p->lcd_taps);
    p->lcd_taps = g_new0(FM1PanelTap, p->lcd_size);
    float step = (float)FM1_LCD_WIDTH / p->lcd_size;
    for (int i = 0; i < p->lcd_size; i++) {
        FM1PanelTap *tap = &p->lcd_taps[i];
        float a = i * step, b = (i + 1) * step;
        tap->first = MIN((int)a, FM1_LCD_WIDTH - 1);
        for (int src = tap->first; src < b && tap->count < 8 && src < FM1_LCD_WIDTH; src++) {
            tap->weight[tap->count++] = (MIN(b, src + 1) - MAX(a, src)) / step;
        }
    }
}

static void draw_lcd(FM1PocPanel *p, Canvas *c)
{
    bool visible = fm1_lcd_visible(p->lcd);
    for (int y = 0; y < p->lcd_size; y++) {
        const FM1PanelTap *ty = &p->lcd_taps[y];
        uint32_t *row = &c->pixels[(p->lcd_y + y) * c->stride + p->lcd_x];
        for (int x = 0; x < p->lcd_size; x++) {
            const FM1PanelTap *tx = &p->lcd_taps[x];
            float r = 0, g = 0, b = 0;
            for (int j = 0; visible && j < ty->count; j++) {
                for (int i = 0; i < tx->count; i++) {
                    uint32_t v = fm1_lcd_rgb(p->lcd, tx->first + i, ty->first + j);
                    float w = tx->weight[i] * ty->weight[j];
                    r += (v >> 16) * w;
                    g += ((v >> 8) & 255) * w;
                    b += (v & 255) * w;
                }
            }
            row[x] = visible ? (uint32_t)MIN(r + .5f, 255) << 16 |
                               (uint32_t)MIN(g + .5f, 255) << 8 | (uint32_t)MIN(b + .5f, 255)
                             : 0x050605;
        }
    }
    p->lcd->redraw = false;
}

/* A control's footprint in display pixels, shadows included. */
static void control_rect(FM1PocPanel *p, const FM1PanelControl *k, int *x, int *y, int *w, int *h)
{
    float margin = k->knob ? 12 : 1;
    *x = MAX(0, (int)floorf(p->x0 + (k->x - margin) * p->scale));
    *y = MAX(0, (int)floorf(p->y0 + (k->y - margin) * p->scale));
    *w = MIN(p->width, (int)ceilf(p->x0 + (k->x + k->w + margin) * p->scale)) - *x;
    *h = MIN(p->height, (int)ceilf(p->y0 + (k->y + k->h + margin) * p->scale)) - *y;
}

static void panel_render(FM1PocPanel *p)
{
    DisplaySurface *surface = qemu_console_surface(p->console);
    /* qemu_console_resize creates QEMU's native 32-bit RGB surface. */
    g_assert(surface_format(surface) == PIXMAN_x8r8g8b8);
    Canvas c = {
        surface_data(surface), surface_stride(surface) / 4, p->width, p->height,
        p->scale, p->x0, p->y0,
    };
    bool all = p->dirty;

    if (all) {
        for (int y = 0; y < p->height; y++) {
            memcpy(&c.pixels[y * c.stride], &p->background[y * p->width], p->width * 4);
        }
    }
    for (unsigned i = 0; i < FM1_PANEL_CONTROLS; i++) {
        FM1PanelControl *k = &p->controls[i];
        if (!all && !k->dirty) {
            continue;
        }
        int x, y, w, h;
        control_rect(p, k, &x, &y, &w, &h);
        for (int row = y; !all && row < y + h; row++) {
            memcpy(&c.pixels[row * c.stride + x], &p->background[row * p->width + x], w * 4);
        }
        if (k->knob) {
            draw_knob(&c, k);
        } else if (k->piano) {
            draw_key(&c, k);
        } else {
            draw_button(&c, k);
        }
        k->dirty = false;
        if (!all) {
            qemu_console_update(p->console, x, y, w, h);
        }
    }
    if (all || p->lcd->redraw) {
        draw_lcd(p, &c);
        if (!all) {
            qemu_console_update(p->console, p->lcd_x, p->lcd_y, p->lcd_size, p->lcd_size);
        }
    }
    if (all) {
        qemu_console_update(p->console, 0, 0, p->width, p->height);
    }
    p->dirty = false;
}

static void panel_resize(FM1PocPanel *p, int width, int height)
{
    p->width = CLAMP(width, DESIGN_W / 3, 8192);
    p->height = CLAMP(height, DESIGN_H / 3, 8192);
    p->scale = MIN((float)p->width / DESIGN_W, (float)p->height / DESIGN_H);
    p->x0 = (p->width - DESIGN_W * p->scale) / 2 - ORIGIN_X * p->scale;
    p->y0 = (p->height - DESIGN_H * p->scale) / 2 - ORIGIN_Y * p->scale;
    qemu_console_resize(p->console, p->width, p->height);
    g_free(p->background);
    p->background = g_new(uint32_t, p->width * p->height);
    Canvas c = { p->background, p->width, p->width, p->height, p->scale, p->x0, p->y0 };
    draw_background(&c);
    lcd_layout(p);
    p->dirty = true;
}

static bool panel_update(void *opaque)
{
    FM1PocPanel *p = opaque;
    uint8_t levels[FM1_LED_COLUMNS][FM1_LED_ROWS];

    fm1_leds_read(levels);
    for (unsigned i = 0; i < FM1_PANEL_CONTROLS; i++) {
        FM1PanelControl *c = &p->controls[i];
        for (unsigned j = 0; j < c->leds; j++) {
            float target = levels[c->led_column[j]][c->led_row[j] - 1];
            float before = c->led_level[j];
            /* Ease towards the firmware's level, like the eye averaging its
             * frame-to-frame dimming jitter; redraw only on a visible step. */
            c->led_level[j] += (target - before) * .5f;
            if (fabsf(target - c->led_level[j]) < 1) {
                c->led_level[j] = target;
            }
            if ((int)(led_brightness(c->led_level[j]) * 24) !=
                (int)(led_brightness(before) * 24)) {
                c->dirty = true;
            }
        }
    }
    panel_render(p);
    return true;
}

static void panel_invalidate(void *opaque)
{
    FM1PocPanel *p = opaque;
    p->dirty = true;
}

static void panel_ui_info(void *opaque, uint32_t head, QemuUIInfo *info)
{
    FM1PocPanel *p = opaque;
    if (info->width && info->height &&
        (info->width != p->width || info->height != p->height)) {
        panel_resize(p, info->width, info->height);
    }
}

static const GraphicHwOps panel_ops = {
    .invalidate = panel_invalidate,
    .gfx_update = panel_update,
    .ui_info = panel_ui_info,
};

/* shortcut: a contact held by both a key and the pointer is released by
 * whichever lets go first; track both if that matters to a player. */
static void contact(QKeyCode code, bool down)
{
    qemu_input_event_send_key_linux(NULL, qemu_input_map_qcode_to_linux[code], down);
}

static void panel_release(FM1PocPanel *p)
{
    timer_del(p->release_timer);
    if (p->held) {
        contact(p->held->contact, false);
        p->held->pressed = false;
        p->held->dirty = true;
        p->held = NULL;
    }
}

static void release_timeout(void *opaque)
{
    panel_release(opaque);
}

static void panel_press(FM1PocPanel *p, FM1PanelControl *c)
{
    if (!runstate_is_running()) {
        return;
    }
    panel_release(p);
    p->held = c;
    c->pressed = true;
    c->dirty = true;
    p->pressed_at = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
    contact(c->contact, true);
}

static void knob_phase(void *opaque)
{
    FM1PanelControl *c = opaque;

    if (!c->pending && !c->phase) {
        return;
    }
    if (!c->phase) {
        c->direction = c->pending > 0 ? 1 : -1;
        c->pending -= c->direction;
    }
    const FM1PanelEncoder *binding = &fm1_panel_encoders[c->encoder];
    QKeyCode first = c->direction > 0 ? binding->phase_b : binding->phase_a;
    QKeyCode second = c->direction > 0 ? binding->phase_a : binding->phase_b;
    contact(c->phase % 2 ? second : first, c->phase < 2);
    c->phase = (c->phase + 1) % 4;
    /* Each quadrature phase must outlast a guest matrix scan; Felucca
     * samples the encoders once per 1.1 ms frame. 4 ms phases play about
     * 60 detents a second, so a fast turn does not queue up. */
    timer_mod(c->phase_timer, qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + 4);
}

static void panel_master(FM1PocPanel *p, int raw)
{
    if (!runstate_is_running()) {
        return;
    }
    FM1PanelControl *knob = &p->controls[FM1_PANEL_CONTROLS - 1];
    p->master = CLAMP(raw, 0, FM1_PANEL_MASTER_MAX);
    knob->angle = -150 + 300. * p->master / FM1_PANEL_MASTER_MAX;
    knob->dirty = true;
    fm1_input_master(p->input, p->master);
}

static void knob_turn(FM1PocPanel *p, FM1PanelControl *c, int steps)
{
    if (!steps || !runstate_is_running()) {
        return;
    }
    if (c->encoder < 0) {
        panel_master(p, p->master + steps * 24);
        return;
    }
    c->angle += steps * 15;
    c->dirty = true;
    c->pending = CLAMP(c->pending + steps, -32, 32);
    if (!timer_pending(c->phase_timer)) {
        knob_phase(c);
    }
}

static FM1PanelControl *panel_hit(FM1PocPanel *p)
{
    float x = (p->x - p->x0) / p->scale, y = (p->y - p->y0) / p->scale;
    for (unsigned i = 0; i < FM1_PANEL_CONTROLS; i++) {
        FM1PanelControl *c = &p->controls[i];
        if (x >= c->x && x < c->x + c->w && y >= c->y && y < c->y + c->h) {
            return c;
        }
    }
    return NULL;
}

static void panel_event(DeviceState *dev, QemuConsole *src, QemuInputEvent *event)
{
    FM1PocPanel *p = panel_instance;

    if (event->type == INPUT_EVENT_KIND_ABS) {
        int value = CLAMP(event->abs.value, INPUT_EVENT_ABS_MIN, INPUT_EVENT_ABS_MAX);
        if (!src) {
            /* QMP without a console turns MASTER, as it always has. */
            if (event->abs.axis == FM1_PANEL_MASTER_AXIS) {
                panel_master(p, qemu_input_scale_axis(value, INPUT_EVENT_ABS_MIN,
                             INPUT_EVENT_ABS_MAX, 0, FM1_PANEL_MASTER_MAX));
            }
        } else if (event->abs.axis == INPUT_AXIS_X) {
            p->x = qemu_input_scale_axis(value, INPUT_EVENT_ABS_MIN, INPUT_EVENT_ABS_MAX,
                                         0, p->width);
        } else {
            p->y = qemu_input_scale_axis(value, INPUT_EVENT_ABS_MIN, INPUT_EVENT_ABS_MAX,
                                         0, p->height);
        }
        return;
    }
    if (event->type != INPUT_EVENT_KIND_BTN || !src) {
        return;
    }
    FM1PanelControl *c = panel_hit(p);
    switch (event->btn.button) {
    case INPUT_BUTTON_LEFT:
        if (event->btn.down && c && c->knob) {
            p->dragged = c;
            p->drag_x = p->x;
            p->drag_y = p->y;
            p->drag_remainder = 0;
        } else if (event->btn.down && c) {
            panel_press(p, c);
        } else if (!event->btn.down) {
            p->dragged = NULL;
            /* A short click must last long enough for the matrix scan. */
            int64_t release = p->pressed_at + 80;
            if (p->held && release > qemu_clock_get_ms(QEMU_CLOCK_REALTIME)) {
                timer_mod(p->release_timer, release);
            } else {
                panel_release(p);
            }
        }
        break;
    case INPUT_BUTTON_WHEEL_UP:
    case INPUT_BUTTON_WHEEL_DOWN:
        if (event->btn.down && c && c->knob) {
            knob_turn(p, c, event->btn.button == INPUT_BUTTON_WHEEL_UP ? 1 : -1);
        }
        break;
    default:
        break;
    }
}

static void panel_sync(DeviceState *dev)
{
    FM1PocPanel *p = panel_instance;

    if (!p->dragged) {
        return;
    }
    /* Up or right turns clockwise, a step per 7 photo pixels. */
    p->drag_remainder += ((p->drag_y - p->y) + (p->x - p->drag_x)) / p->scale;
    int steps = p->drag_remainder / 7;
    p->drag_remainder -= steps * 7;
    p->drag_x = p->x;
    p->drag_y = p->y;
    knob_turn(p, p->dragged, steps);
}

static const QemuInputHandler panel_handler = {
    .name = "FM-1 panel",
    .mask = INPUT_EVENT_MASK_BTN | INPUT_EVENT_MASK_ABS,
    .event = panel_event,
    .sync = panel_sync,
};

static void panel_runstate(void *opaque, bool running, RunState state)
{
    FM1PocPanel *p = opaque;

    if (running) {
        return;
    }
    /* The board input releases its contacts on a stop; forget ours. */
    panel_release(p);
    p->dragged = NULL;
    for (unsigned i = 0; i < FM1_PANEL_CONTROLS; i++) {
        FM1PanelControl *c = &p->controls[i];
        if (c->phase_timer) {
            timer_del(c->phase_timer);
            c->pending = c->phase = 0;
        }
    }
}

static FM1PanelControl *add_contact(FM1PocPanel *p, unsigned *count, float x, float y,
                                    float w, float h, const FM1PanelKey *key)
{
    FM1PanelControl *c = &p->controls[(*count)++];
    *c = (FM1PanelControl) {
        .x = x, .y = y, .w = w, .h = h, .caption = key->label, .contact = key->qcode,
        .leds = 1, .led_column = { key->column }, .led_row = { key->row },
    };
    return c;
}

void fm1_panel_init(FM1PocPanel *p, FM1PocLCD *lcd, FM1PocInput *input)
{
    /* Black keys follow these white keys (1-based), as in the photo; key
     * contacts take the keys from left to right. */
    static const bool black_after[17] = {
        [1] = 1, [2] = 1, [3] = 1, [5] = 1, [6] = 1, [8] = 1, [9] = 1, [10] = 1,
        [12] = 1, [13] = 1, [15] = 1,
    };
    static const char *const black_labels[] = {
        "OP1", "OP2", "OP3", "OP4", "OP5", "OP6", "PIT", "GLO", "MONO", "POLY", "",
    };
    static const float knobs[][2] = {
        { 556, 268 }, { 413, 408 }, { 556, 408 },
        { 1061, 268 }, { 1212, 268 }, { 1362, 268 }, { 1514, 268 }, { 413, 268 },
    };
    unsigned count = 0;

    g_assert(!panel_instance);
    panel_instance = p;
    p->lcd = lcd;
    p->input = input;
    for (unsigned i = 0; i < FM1_PANEL_BUTTONS; i++) {
        FM1PanelControl *c = i < 12 ?
            add_contact(p, &count, 1044 + (i % 6) * 84, 383 + (i / 6) * 84, 70, 66,
                        &fm1_panel_buttons[i]) :
            add_contact(p, &count, 394 + (i - 12) * 104, 497, 82, 44, &fm1_panel_buttons[i]);
        if (!strcmp(c->caption, "PLAY/STOP")) {
            /* PLAY's green LED. */
            c->leds = 2;
            c->led_column[1] = 8;
            c->led_row[1] = 1;
        }
    }
    unsigned key = 0, black = 0;
    for (unsigned white = 1; white <= 16; white++) {
        add_contact(p, &count, 372 + (white - 1) * 76, 748, 66, 124,
                    &fm1_panel_keys[key++])->caption = NULL;
        if (black_after[white]) {
            add_contact(p, &count, 410 + (white - 1) * 76, 620, 66, 122,
                        &fm1_panel_keys[key++])->caption = black_labels[black++];
        }
    }
    g_assert(key == FM1_PANEL_KEYS);
    for (unsigned i = FM1_PANEL_BUTTONS; i < count; i++) {
        p->controls[i].piano = true;
    }
    /* The encoders, then MASTER last. */
    for (int i = 0; i <= FM1_PANEL_ENCODERS; i++) {
        int encoder = i < FM1_PANEL_ENCODERS ? i : -1;
        FM1PanelControl *c = &p->controls[count++];
        *c = (FM1PanelControl) {
            .x = knobs[i][0] - 31, .y = knobs[i][1] - 31, .w = 62, .h = 62,
            .knob = true, .encoder = encoder,
        };
        if (encoder >= 0) {
            c->caption = fm1_panel_encoders[encoder].label;
            c->phase_timer = timer_new_ms(QEMU_CLOCK_REALTIME, knob_phase, c);
        }
    }
    g_assert(count == FM1_PANEL_CONTROLS);
    p->master = FM1_PANEL_MASTER_DEFAULT;
    p->controls[FM1_PANEL_CONTROLS - 1].angle =
        -150 + 300. * p->master / FM1_PANEL_MASTER_MAX;
    p->release_timer = timer_new_ms(QEMU_CLOCK_REALTIME, release_timeout, p);
    /* This private panel is machine state rather than a qdev device. */
    p->console = qemu_graphic_console_create(NULL, 0, &panel_ops, p);
    panel_resize(p, DESIGN_W * DEFAULT_SCALE, DESIGN_H * DEFAULT_SCALE);
    p->handler = qemu_input_handler_register(NULL, &panel_handler);
    p->runstate = qemu_add_vm_change_state_handler(panel_runstate, p);
}
