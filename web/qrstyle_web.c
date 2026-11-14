/*
 * The browser front end's half of the library: qrcodegen.c and qrstyle.c,
 * compiled to WebAssembly unchanged, plus this file to drive them.
 *
 * Nothing here is a port. The page in docs/ hands this a string, this encodes
 * and renders it with exactly the code a firmware build runs, and the page
 * copies the pixels onto a canvas. The only thing added on this side is the
 * RGB565 -> RGBA conversion a canvas needs.
 *
 * The library keeps no state, but a wasm module with no allocator needs its
 * buffers somewhere, so they live here as statics -- one render at a time,
 * which is all a page asks for.
 *
 *   make web     rebuilds docs/qrstyle.wasm with emcc
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "qrcodegen.h"
#include "qrstyle.h"

#include "logo_tori.h"

#define EXPORT __attribute__((used, visibility("default")))

#define MAX_VERSION qrcodegen_VERSION_MAX
#define MAX_PX      1200
#define TEXT_CAP    4096

static char g_text[TEXT_CAP];
static uint8_t g_qr[qrcodegen_BUFFER_LEN_MAX];
static uint8_t g_tmp[qrcodegen_BUFFER_LEN_MAX];
static uint16_t g_fb[MAX_PX * MAX_PX];
static uint8_t g_rgba[MAX_PX * MAX_PX * 4];

static struct QrStyle_Report g_report;
static int g_version, g_ecl, g_ppm, g_px;

static const struct QrStyle_Logo TORI = {
    .rgb565 = tori_logo_rgb565,
    .alpha_bits = tori_logo_alpha,
    .w = TORI_LOGO_W,
    .h = TORI_LOGO_H,
    .rgb565_big_endian = false,
    .size_pct = 22, /* 22 + a 1-module pad = 27 of the 30 HIGH affords */
    .pad_modules = 1,
    .pad_radius_pct = 30,
};

/*---- The gallery's styles, the same ones examples/preview.c checks ----*/

static struct QrStyle_Fill
solid(struct QrStyle_Rgb c)
{
    struct QrStyle_Fill f;

    memset(&f, 0, sizeof(f));
    f.kind = QRSTYLE_FILL_SOLID;
    f.from = c;
    f.to = c;
    return f;
}

static struct QrStyle_Fill
grad(struct QrStyle_Rgb a, struct QrStyle_Rgb b, int deg)
{
    struct QrStyle_Fill f;

    memset(&f, 0, sizeof(f));
    f.kind = QRSTYLE_FILL_LINEAR;
    f.from = a;
    f.to = b;
    f.angle_deg = (int16_t)deg;
    return f;
}

static struct QrStyle_Fill
radial(struct QrStyle_Rgb a, struct QrStyle_Rgb b)
{
    struct QrStyle_Fill f;

    memset(&f, 0, sizeof(f));
    f.kind = QRSTYLE_FILL_RADIAL;
    f.from = a;
    f.to = b;
    return f;
}

#define INK          QRSTYLE_HEX(0x241610)
#define INK_WARM     QRSTYLE_HEX(0x4A2C18)
#define INK_COPPER   QRSTYLE_HEX(0x8A4B26)
#define PAPER        QRSTYLE_HEX(0xFAF3E7)
#define PAPER_WARM   QRSTYLE_HEX(0xF3E4CE)
#define PINE         QRSTYLE_HEX(0x1C4B3C)
#define PINE_LIGHT   QRSTYLE_HEX(0x2F7A5C)
#define NIGHT        QRSTYLE_HEX(0x140F0A)
#define CANDLE       QRSTYLE_HEX(0xF0D9AC)
#define KC_TEAL      QRSTYLE_HEX(0x62C9C8)
#define KC_TEAL_DEEP QRSTYLE_HEX(0x17615E)
#define KC_NAVY      QRSTYLE_HEX(0x16283C)
#define KC_RED       QRSTYLE_HEX(0xD93A44)
#define KC_RED_DEEP  QRSTYLE_HEX(0xA11F2A)
#define KC_WHITE     QRSTYLE_HEX(0xF4F8F8)

/* The order here is the order of STYLES in docs/app.js. */
static void
build_config(int style, struct QrStyle_Config *cfg)
{
    QrStyle_ConfigDefault(cfg);

    switch( style )
    {
    default:
    case 0: /* Liquid */
        QrStyle_ConfigPreset(cfg, QRSTYLE_PRESET_LIQUID);
        cfg->fg = grad(INK_WARM, INK_COPPER, 55);
        cfg->eyes_use_fg = false;
        cfg->eye_frame_fill = solid(INK);
        cfg->eye_pupil_fill = solid(INK_COPPER);
        cfg->bg = solid(PAPER);
        break;

    case 1: /* Dots */
        QrStyle_ConfigPreset(cfg, QRSTYLE_PRESET_DOTS);
        cfg->fg = solid(INK);
        cfg->bg = solid(PAPER);
        break;

    case 2: /* Teardrop */
        QrStyle_ConfigPreset(cfg, QRSTYLE_PRESET_TEARDROP);
        cfg->fg = radial(PINE, PINE_LIGHT);
        cfg->eyes_use_fg = false;
        cfg->eye_frame_fill = solid(PINE);
        cfg->eye_pupil_fill = solid(INK_COPPER);
        cfg->bg = solid(PAPER_WARM);
        break;

    case 3: /* Diamond */
        QrStyle_ConfigPreset(cfg, QRSTYLE_PRESET_DIAMOND);
        cfg->fg = grad(PINE, INK, 90);
        cfg->eyes_use_fg = false;
        cfg->eye_frame_fill = solid(PINE);
        cfg->eye_pupil_fill = solid(PINE);
        cfg->bg = solid(PAPER);
        break;

    case 4: /* Star */
        QrStyle_ConfigPreset(cfg, QRSTYLE_PRESET_STAR);
        cfg->fg = solid(INK_WARM);
        cfg->eyes_use_fg = false;
        cfg->eye_frame_fill = solid(INK);
        cfg->eye_pupil_fill = solid(INK_COPPER);
        cfg->bg = solid(PAPER);
        break;

    case 5: /* Night */
        QrStyle_ConfigPreset(cfg, QRSTYLE_PRESET_LIQUID);
        cfg->fg = grad(CANDLE, QRSTYLE_HEX(0xC9A15E), 45);
        cfg->eyes_use_fg = false;
        cfg->eye_frame_fill = solid(CANDLE);
        cfg->eye_pupil_fill = solid(QRSTYLE_HEX(0xE0B05A));
        cfg->bg = solid(NIGHT);
        break;

    case 6: /* Classic */
        QrStyle_ConfigPreset(cfg, QRSTYLE_PRESET_CLASSIC);
        break;

    case 7: /* KC / Teal */
        QrStyle_ConfigPreset(cfg, QRSTYLE_PRESET_LIQUID);
        cfg->fg = solid(KC_NAVY);
        cfg->eyes_use_fg = false;
        cfg->eye_frame_fill = solid(KC_NAVY);
        cfg->eye_pupil_fill = solid(KC_RED_DEEP);
        cfg->bg = solid(KC_TEAL);
        break;

    case 8: /* KC / Crest */
        QrStyle_ConfigPreset(cfg, QRSTYLE_PRESET_TEARDROP);
        cfg->module_shape = QRSTYLE_MODULE_ROUNDED;
        cfg->module_size_pct = 92;
        cfg->fg = grad(KC_NAVY, KC_TEAL_DEEP, 55);
        cfg->eyes_use_fg = false;
        cfg->eye_frame_fill = solid(KC_NAVY);
        cfg->eye_pupil_fill = solid(KC_RED);
        cfg->bg = solid(KC_WHITE);
        break;

    case 9: /* KC / Match Red */
        QrStyle_ConfigPreset(cfg, QRSTYLE_PRESET_DOTS);
        cfg->fg = solid(KC_RED);
        cfg->eyes_use_fg = false;
        cfg->eye_frame_fill = solid(KC_NAVY);
        cfg->eye_pupil_fill = solid(KC_RED);
        cfg->bg = solid(KC_WHITE);
        break;

    case 10: /* KC / Midnight */
        QrStyle_ConfigPreset(cfg, QRSTYLE_PRESET_LIQUID);
        cfg->fg = solid(KC_TEAL);
        cfg->eyes_use_fg = false;
        cfg->eye_frame_fill = solid(KC_WHITE);
        cfg->eye_pupil_fill = solid(KC_TEAL);
        cfg->bg = solid(KC_NAVY);
        break;
    }

    /* Every style on the page carries Tori; that forces ECC HIGH. */
    cfg->logo = &TORI;
}

/*---- Exports ----*/

/* Where the page writes its UTF-8, NUL-terminated, at most TEXT_CAP bytes. */
EXPORT char *
web_text(void)
{
    return g_text;
}

EXPORT int
web_text_cap(void)
{
    return TEXT_CAP;
}

EXPORT uint8_t *
web_rgba(void)
{
    return g_rgba;
}

/*
 * Encode web_text() and render it in `style`, aiming for about `target_px`
 * pixels across. The result is a whole number of pixels per module -- the
 * renderer snaps to that anyway, and sizing the canvas to it means there is no
 * unused margin -- so the actual size comes back as the return value, and 0
 * means the text did not fit in a version 40 symbol at ECC HIGH.
 */
EXPORT int
web_render(int style, int target_px)
{
    struct QrStyle_Config cfg;
    struct QrStyle_Target tg;
    struct QrStyle_Box box;
    enum qrcodegen_Ecc ecl;
    int size, total, ppm, px, i, n;

    g_text[TEXT_CAP - 1] = '\0';
    memset(&g_report, 0, sizeof(g_report));
    g_version = g_ecl = g_ppm = g_px = 0;

    build_config(style, &cfg);
    if( !QrStyle_EncodeText(g_text, &cfg, g_tmp, g_qr, MAX_VERSION, &ecl) )
        return 0;

    size = qrcodegen_getSize(g_qr);
    total = size + 2 * cfg.quiet_modules;
    ppm = target_px / total;
    if( ppm < 4 )
        ppm = 4;
    if( ppm * total > MAX_PX )
        ppm = MAX_PX / total;
    px = ppm * total;

    (void)QrStyle_Check(g_qr, &cfg, &g_report);

    tg.pixels = g_fb;
    tg.width = px;
    tg.height = px;
    tg.stride = px;
    tg.origin_x = 0;
    tg.origin_y = 0;
    tg.big_endian = false;
    box.x = 0;
    box.y = 0;
    box.size = px;
    QrStyle_Render(g_qr, &cfg, &tg, &box);

    n = px * px;
    for( i = 0; i < n; i++ )
    {
        uint16_t v = g_fb[i];
        unsigned r = (v >> 11) & 0x1F, g = (v >> 5) & 0x3F, b = v & 0x1F;

        g_rgba[i * 4 + 0] = (uint8_t)((r << 3) | (r >> 2));
        g_rgba[i * 4 + 1] = (uint8_t)((g << 2) | (g >> 4));
        g_rgba[i * 4 + 2] = (uint8_t)((b << 3) | (b >> 2));
        g_rgba[i * 4 + 3] = 255;
    }

    g_version = (size - 17) / 4;
    g_ecl = (int)ecl;
    g_ppm = QrStyle_PixelsPerModule(g_qr, &cfg, px);
    g_px = px;
    return px;
}

/* What the last web_render() produced, for the page's readout. */
EXPORT int web_version(void) { return g_version; }
EXPORT int web_ecl(void) { return g_ecl; } /* 0..3 = L M Q H */
EXPORT int web_ppm(void) { return g_ppm; }
EXPORT int web_ok(void) { return g_report.ok ? 1 : 0; }
EXPORT int web_contrast_x10(void) { return g_report.contrast_x10; }
EXPORT int web_logo_pct(void) { return g_report.logo_area_pct; }
EXPORT int web_logo_budget(void) { return g_report.logo_budget_pct; }
EXPORT const char *web_problem(void) { return g_report.problem; }
