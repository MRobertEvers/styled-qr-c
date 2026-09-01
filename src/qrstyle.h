/*
 * qrstyle -- artistic rendering for QR codes produced by qrcodegen.
 *
 * qrcodegen.c answers "which modules are dark". This answers "what does a dark
 * module look like", and nothing here can change the first answer -- the
 * styling layer only ever READS that matrix. Every style below is therefore a
 * rendering choice over a matrix that is already correct, and a style that will
 * not scan is a style that drew the right matrix badly: a bug in this file, and
 * never a question about the standard.
 *
 * THE ONE INVARIANT. A decoder samples the CENTRE of each module, and finds
 * the code at all by the 1:1:3:1:1 dark/light run through the middle of a
 * finder pattern. Every shape here is centred on its module, and every eye
 * keeps that run intact along its centre row and column -- that is the whole
 * reason the shapes are the shapes, and examples/preview.c measures it on
 * the rendered pixels rather than taking this paragraph's word for it.
 *
 * Colour and the logo are the ways a caller can still break a code that this
 * file drew correctly -- too little contrast, an eye shade that binarises to
 * the wrong side of the one threshold a decoder picks, or a logo bigger than
 * the error correction can repair. QrStyle_Check() measures all three.
 *
 * See the repository README for the pictures.
 */
#ifndef QRSTYLE_H
#define QRSTYLE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "qrcodegen.h"

#ifdef __cplusplus
extern "C" {
#endif

/*---- Colour ----*/

struct QrStyle_Rgb
{
    uint8_t r, g, b;
};

/* QRSTYLE_HEX(0x1B4D3E) -- a C99 compound literal, so it works anywhere a
 * struct value is expected, including inside a designated initialiser. */
#define QRSTYLE_HEX(hex)                                                      \
    ((struct QrStyle_Rgb){ (uint8_t)(((hex) >> 16) & 0xFF),                   \
                           (uint8_t)(((hex) >> 8) & 0xFF),                    \
                           (uint8_t)((hex) & 0xFF) })

enum QrStyle_FillKind
{
    QRSTYLE_FILL_SOLID = 0, /* `from` only */
    QRSTYLE_FILL_LINEAR,    /* `from` -> `to` along `angle_deg` */
    QRSTYLE_FILL_RADIAL,    /* `from` at the centre -> `to` at the corners */
};

/*
 * A gradient is evaluated over the CODE's bounding box, not per module, so the
 * ramp crosses the whole symbol and neighbouring modules differ by a shade
 * instead of each carrying its own private copy of the ramp.
 */
struct QrStyle_Fill
{
    enum QrStyle_FillKind kind;
    struct QrStyle_Rgb from;
    struct QrStyle_Rgb to;
    int16_t angle_deg; /* linear only: 0 = left to right, 90 = top to bottom */
};

/*---- Shapes ----*/

/*
 * The data modules.
 *
 * All of these cover the module's centre, which is the sample point a decoder
 * uses. SQUARE at 100% is the standard; the rest trade coverage for looks and
 * `module_size_pct` decides how much.
 */
enum QrStyle_ModuleShape
{
    QRSTYLE_MODULE_SQUARE = 0,
    QRSTYLE_MODULE_ROUNDED, /* square with softened corners */
    QRSTYLE_MODULE_CIRCLE,
    QRSTYLE_MODULE_DIAMOND,
    QRSTYLE_MODULE_STAR,  /* four-pointed; wants >= 6 px per module */
    QRSTYLE_MODULE_CROSS, /* plus sign */
    /*
     * LIQUID fuses neighbours: a module fills its whole cell, and a corner is
     * rounded only where both neighbours meeting at it are light. Runs of dark
     * modules join into one continuous form and a lone module becomes a
     * circle. It keeps every dark module's full cell, which makes the most
     * decorative style here also the one with the most margin.
     */
    QRSTYLE_MODULE_LIQUID,
};

/*
 * The outer 7x7 ring of a finder pattern.
 *
 * TEARDROP and LEAF are oriented per eye -- a teardrop's sharp corner points
 * away from the centre of the code, so the three eyes mirror each other rather
 * than all pointing the same way.
 */
enum QrStyle_EyeFrame
{
    QRSTYLE_EYE_SQUARE = 0,
    QRSTYLE_EYE_ROUNDED,
    QRSTYLE_EYE_CIRCLE,
    QRSTYLE_EYE_TEARDROP, /* one sharp corner, pointing outward */
    QRSTYLE_EYE_LEAF,     /* two opposite sharp corners */
};

/* The 3x3 centre of a finder pattern. */
enum QrStyle_PupilShape
{
    QRSTYLE_PUPIL_SQUARE = 0,
    QRSTYLE_PUPIL_ROUNDED,
    QRSTYLE_PUPIL_CIRCLE,
    QRSTYLE_PUPIL_DIAMOND,
    QRSTYLE_PUPIL_STAR,
};

/*---- Logo ----*/

/*
 * An image dropped into the middle of the code.
 *
 * The modules underneath are not drawn at all, and that is what makes a logo
 * affordable: a half-covered module is one a decoder reads as the wrong
 * colour, while an absent one is an erasure, and Reed-Solomon exists to repair
 * erasures. `size_pct` is the logo box's width as a percentage of the code's
 * width; QrStyle_LogoBudgetPct() is the number it has to stay under.
 *
 * `rgb565` is row-major, w*h entries. `alpha_bits` is an optional 1bpp mask,
 * MSB first, each row padded to a whole byte, a set bit meaning opaque. Both
 * may live in flash.
 */
struct QrStyle_Logo
{
    const uint16_t *rgb565;
    const uint8_t *alpha_bits; /* NULL when the image is fully opaque */
    uint16_t w, h;
    bool rgb565_big_endian; /* byte order of the SOURCE array */

    uint8_t size_pct;       /* logo box width, % of the code width */
    uint8_t pad_modules;    /* background margin around it, in modules */
    uint8_t pad_radius_pct; /* pad corner radius, % of half the pad's width */
};

/*---- Configuration ----*/

struct QrStyle_Config
{
    enum QrStyle_ModuleShape module_shape;
    /*
     * How much of its cell a module fills, 55..100. LIQUID ignores it and is
     * always 100. Below 55 a phone camera's blur closes the gaps between
     * modules faster than it resolves the modules, so it is clamped.
     */
    uint8_t module_size_pct;

    enum QrStyle_EyeFrame eye_frame;
    enum QrStyle_PupilShape eye_pupil;

    struct QrStyle_Fill fg;             /* data modules */
    struct QrStyle_Fill eye_frame_fill; /* used when eyes_use_fg is false */
    struct QrStyle_Fill eye_pupil_fill;
    bool eyes_use_fg;

    struct QrStyle_Fill bg;
    /*
     * Draw no background and blend onto whatever is already in the target.
     * The caller owns the contrast in that case, and QrStyle_Check() reports
     * that it could not check rather than passing it.
     */
    bool bg_transparent;

    uint8_t quiet_modules; /* light margin; the standard is 4 */
    uint8_t supersample;   /* 1..4 samples per axis per pixel; 3 is the
                            * default and 1 turns anti-aliasing off */

    const struct QrStyle_Logo *logo; /* NULL for none */
};

/* A square region of the caller's coordinate space, in pixels. */
struct QrStyle_Box
{
    int x, y, size;
};

/*
 * Where the pixels go.
 *
 * `origin_x`/`origin_y` are the LOGICAL coordinates of pixels[0], which is
 * what lets a 240x240 frame be drawn as two 240x120 strips: pass the same box
 * both times, with origin_y 0 and then 120. The renderer clips to the target,
 * so a strip the code does not reach costs a bounds test and nothing else.
 *
 * `big_endian` is the byte order the DISPLAY wants. An SPI panel such as the
 * ST7789 is usually fed RGB565 big-endian, so firmware sets this; a host tool
 * writing a PNG does not.
 */
struct QrStyle_Target
{
    uint16_t *pixels;
    int width, height, stride;
    int origin_x, origin_y;
    bool big_endian;
};

/*---- Presets ----*/

enum QrStyle_Preset
{
    QRSTYLE_PRESET_CLASSIC = 0, /* square modules and eyes, black on white */
    QRSTYLE_PRESET_DOTS,        /* circles, circular eyes */
    QRSTYLE_PRESET_LIQUID,      /* fused modules, rounded eyes */
    QRSTYLE_PRESET_TEARDROP,    /* rounded modules, teardrop eyes, gradient */
    QRSTYLE_PRESET_DIAMOND,
    QRSTYLE_PRESET_STAR,
    QRSTYLE_PRESET_COUNT,
};

void QrStyle_ConfigDefault(struct QrStyle_Config *cfg);
void QrStyle_ConfigPreset(struct QrStyle_Config *cfg, enum QrStyle_Preset p);

/*---- Encoding ----*/

/*
 * Encode `text` at an error correction level chosen for the style.
 *
 * A logo needs HIGH, because the modules it hides have to come back out of the
 * redundancy and nothing else pays for them. Without a logo this asks for
 * MEDIUM and lets qrcodegen raise it for free -- that boost only applies when
 * a higher level fits the same version, so it never costs a module.
 *
 * `tmp` and `qrcode` must each hold qrcodegen_BUFFER_LEN_FOR_VERSION(max_version)
 * bytes. Returns false if the text does not fit, exactly as qrcodegen does.
 * `out_ecl`, when non-NULL, receives the level that was used.
 */
bool QrStyle_EncodeText(const char *text, const struct QrStyle_Config *cfg,
                        uint8_t tmp[], uint8_t qrcode[], int max_version,
                        enum qrcodegen_Ecc *out_ecl);

/* The error correction level of an encoded symbol, read back out of its
 * format bits. */
enum qrcodegen_Ecc QrStyle_Ecl(const uint8_t qrcode[]);

/*---- Checking ----*/

/*
 * What a decoder needs, measured rather than assumed.
 *
 * `contrast_x10` is the WCAG contrast ratio between the worst foreground shade
 * and the background, times ten -- 45 means 4.5:1. Below
 * QRSTYLE_MIN_CONTRAST_X10 a phone camera in ordinary light starts losing the
 * threshold between a dark and a light module.
 *
 * `logo_area_pct` is the share of the symbol's width the logo and its pad
 * erase; `logo_budget_pct` is what the symbol's error correction level can
 * repair. The first has to be under the second.
 */
#define QRSTYLE_MIN_CONTRAST_X10 30

struct QrStyle_Report
{
    bool ok;
    int contrast_x10; /* -1 when the background is transparent */
    int logo_area_pct;
    int logo_budget_pct;
    const char *problem; /* NULL when ok */
};

/*
 * How far an ink shade may sit from the data ink, as a percentage of the
 * distance between the data ink and the background.
 *
 * A decoder does not compare each module against the background; it picks ONE
 * threshold for the image and applies it everywhere, and that threshold lands
 * near the midpoint of the two populations it can see -- the data modules and
 * the paper. An eye drawn in a colour that has good contrast against the
 * background but sits on the far side of that midpoint therefore binarises to
 * PAPER, and the finder pattern disappears while every colour in the design
 * still passes a contrast check.
 *
 * That is not hypothetical: it is what examples/preview.c caught on a mint
 * green eye against a near-black background, where the ring simply stopped
 * existing for the binariser. So an eye colour has to stay on the ink side of
 * the midpoint by this margin.
 */
#define QRSTYLE_INK_MARGIN_PCT 10

bool QrStyle_Check(const uint8_t qrcode[], const struct QrStyle_Config *cfg,
                   struct QrStyle_Report *out);

/* The largest logo `size_pct` this error correction level can carry, as a
 * percentage of the code's width. */
int QrStyle_LogoBudgetPct(enum qrcodegen_Ecc ecl);

/*---- Rendering ----*/

/*
 * Draw `qrcode` into `box`, clipped to `target`.
 *
 * `box.size` is the whole symbol INCLUDING the quiet zone, so the code is
 * drawn at box.size/(modules + 2*quiet) pixels per module and a caller can
 * hand over the whole panel without doing that arithmetic itself. Nothing is
 * allocated and nothing is kept between calls.
 */
void QrStyle_Render(const uint8_t qrcode[], const struct QrStyle_Config *cfg,
                    const struct QrStyle_Target *target,
                    const struct QrStyle_Box *box);

/*
 * Pixels per module for a given box -- what a caller checks before drawing.
 * Below 3 no style survives; the shaped ones want 5 or more.
 */
int QrStyle_PixelsPerModule(const uint8_t qrcode[],
                            const struct QrStyle_Config *cfg, int box_size);

#ifdef __cplusplus
}
#endif
#endif /* QRSTYLE_H */
