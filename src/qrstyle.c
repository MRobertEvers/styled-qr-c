/*
 * qrstyle -- see qrstyle.h for what this is and what it may not break.
 *
 * FIXED POINT, AND WHY THERE IS NO FLOAT HERE. Every shape is evaluated per
 * sub-sample -- a 200 px code at supersample 3 is 360,000 shape tests -- and
 * this part draws that beside a software 3D raster on a 240 MHz core with a
 * frame budget already spoken for. One module is 256 units (Q8) and every
 * predicate below is integer compares and one multiply.
 *
 * WHY MODULES SNAP TO WHOLE PIXELS. The renderer picks an integer pixels-
 * per-module and centres the result in the caller's box rather than scaling to
 * fill it. A fractional module size makes some modules a pixel wider than
 * their neighbours, and the decoder's grid estimate then walks off the true
 * centres across the width of the symbol -- which shows up as a code that
 * scans at one distance and not another. A few pixels of unused box is the
 * cheaper mistake.
 */

#include <string.h>

#include "qrstyle.h"

/*---- Fixed point ----*/

#define Q       8
#define ONE     (1 << Q)  /* one module */
#define HALFONE (ONE / 2)

/* A finder pattern is 7x7 modules: the ring, the separator, then the pupil. */
#define EYE_HALF   (7 * ONE / 2) /* 896 */
#define EYE_INNER  (5 * ONE / 2) /* 640 */
#define EYE_PUPIL  (3 * ONE / 2) /* 384 */

#define CLS_NONE      0
#define CLS_FG        1
#define CLS_EYE_FRAME 2
#define CLS_EYE_PUPIL 3
#define CLS_COUNT     3

static int32_t
iabs32(int32_t v)
{
    return v < 0 ? -v : v;
}

static int32_t
imin32(int32_t a, int32_t b)
{
    return a < b ? a : b;
}

static int32_t
imax32(int32_t a, int32_t b)
{
    return a > b ? a : b;
}

static uint32_t
isqrt32(uint32_t x)
{
    uint32_t r = 0, b = 1u << 30;

    while( b > x )
        b >>= 2;
    while( b )
    {
        if( x >= r + b )
        {
            x -= r + b;
            r = (r >> 1) + b;
        }
        else
            r >>= 1;
        b >>= 2;
    }
    return r;
}

/*---- Shape predicates ----*/

/*
 * A rounded rectangle with an independent radius per corner, which is the one
 * primitive the square, rounded, circular, teardrop and leaf styles are all
 * made of -- a circle is a rectangle with a radius of half its width, and a
 * teardrop is one with three of its four corners rounded. Corner order is
 * top-left, top-right, bottom-right, bottom-left.
 */
static bool
rrect_in(
    int32_t dx,
    int32_t dy,
    int32_t hw,
    int32_t hh,
    int32_t rtl,
    int32_t rtr,
    int32_t rbr,
    int32_t rbl)
{
    int32_t r, adx, ady, cx, cy, ex, ey;

    if( dx < -hw || dx > hw || dy < -hh || dy > hh )
        return false;

    r = (dy < 0) ? ((dx < 0) ? rtl : rtr) : ((dx < 0) ? rbl : rbr);
    if( r <= 0 )
        return true;

    adx = iabs32(dx);
    ady = iabs32(dy);
    cx = hw - r;
    cy = hh - r;
    if( adx <= cx || ady <= cy )
        return true;

    ex = adx - cx;
    ey = ady - cy;
    return ex * ex + ey * ey <= r * r;
}

static bool
diamond_in(int32_t dx, int32_t dy, int32_t r)
{
    return iabs32(dx) + iabs32(dy) <= r;
}

static bool
cross_in(int32_t dx, int32_t dy, int32_t r)
{
    int32_t w = r * 2 / 5;

    return (iabs32(dx) <= w && iabs32(dy) <= r) ||
           (iabs32(dy) <= w && iabs32(dx) <= r);
}

/*
 * A four-pointed star, as the superellipse sqrt(|x|) + sqrt(|y|) <= k, with
 * sqrt(256) = 16 as the unit.
 *
 * k = 16 is the true astroid and it is too thin twice over: its arms taper to
 * nothing, so a module a handful of pixels across keeps a visible centre and
 * no arms, and the shape covers barely a third of what a circle would, which
 * is coverage a camera's blur eats first. Every step of k trades a little of
 * the concavity that makes it read as a star for area. 20 is where it still
 * reads as one; the bounding test keeps the wider threshold inside the cell.
 */
#define QRSTYLE_STAR_K 20

static bool
star_in(int32_t dx, int32_t dy, int32_t r)
{
    uint32_t a, b;

    if( r <= 0 )
        return false;
    dx = iabs32(dx);
    dy = iabs32(dy);
    if( dx > r || dy > r )
        return false;

    a = isqrt32((uint32_t)(dx * 256 / r));
    b = isqrt32((uint32_t)(dy * 256 / r));
    return a + b <= QRSTYLE_STAR_K;
}

/*---- Colour ----*/

/*
 * sRGB channel values linearised to Q16, for the WCAG relative luminance in
 * QrStyle_Check. A table because the alternative is powf(x, 2.4) on a part
 * whose libm call costs more than this whole table does in flash.
 */
static const uint16_t SRGB_LINEAR[256] = {
        0,    20,    40,    60,    80,    99,   119,   139,
      159,   179,   199,   219,   241,   264,   288,   313,
      340,   367,   396,   427,   458,   491,   526,   562,
      599,   637,   677,   718,   761,   805,   851,   898,
      947,   997,  1048,  1101,  1156,  1212,  1270,  1330,
     1391,  1453,  1517,  1583,  1651,  1720,  1790,  1863,
     1937,  2013,  2090,  2170,  2250,  2333,  2418,  2504,
     2592,  2681,  2773,  2866,  2961,  3058,  3157,  3258,
     3360,  3464,  3570,  3678,  3788,  3900,  4014,  4129,
     4247,  4366,  4488,  4611,  4736,  4864,  4993,  5124,
     5257,  5392,  5530,  5669,  5810,  5953,  6099,  6246,
     6395,  6547,  6700,  6856,  7014,  7174,  7335,  7500,
     7666,  7834,  8004,  8177,  8352,  8528,  8708,  8889,
     9072,  9258,  9445,  9635,  9828, 10022, 10219, 10417,
    10619, 10822, 11028, 11235, 11446, 11658, 11873, 12090,
    12309, 12530, 12754, 12980, 13209, 13440, 13673, 13909,
    14146, 14387, 14629, 14874, 15122, 15371, 15623, 15878,
    16135, 16394, 16656, 16920, 17187, 17456, 17727, 18001,
    18277, 18556, 18837, 19121, 19407, 19696, 19987, 20281,
    20577, 20876, 21177, 21481, 21787, 22096, 22407, 22721,
    23038, 23357, 23678, 24002, 24329, 24658, 24990, 25325,
    25662, 26001, 26344, 26688, 27036, 27386, 27739, 28094,
    28452, 28813, 29176, 29542, 29911, 30282, 30656, 31033,
    31412, 31794, 32179, 32567, 32957, 33350, 33745, 34143,
    34544, 34948, 35355, 35764, 36176, 36591, 37008, 37429,
    37852, 38278, 38706, 39138, 39572, 40009, 40449, 40891,
    41337, 41785, 42236, 42690, 43147, 43606, 44069, 44534,
    45002, 45473, 45947, 46423, 46903, 47385, 47871, 48359,
    48850, 49344, 49841, 50341, 50844, 51349, 51858, 52369,
    52884, 53401, 53921, 54445, 54971, 55500, 56032, 56567,
    57105, 57646, 58190, 58737, 59287, 59840, 60396, 60955,
    61517, 62082, 62650, 63221, 63795, 64372, 64952, 65535,
};

/* sin(deg) in Q10, 0..90; everything else is a reflection of these. */
static const int16_t SIN_Q10[91] = {
       0,   18,   36,   54,   71,   89,  107,  125,  143,  160,
     178,  195,  213,  230,  248,  265,  282,  299,  316,  333,
     350,  367,  384,  400,  416,  433,  449,  465,  481,  496,
     512,  527,  543,  558,  573,  587,  602,  616,  630,  644,
     658,  672,  685,  698,  711,  724,  737,  749,  761,  773,
     784,  796,  807,  818,  828,  839,  849,  859,  868,  878,
     887,  896,  904,  912,  920,  928,  935,  943,  949,  956,
     962,  968,  974,  979,  984,  989,  993,  997, 1001, 1005,
    1008, 1010, 1013, 1015, 1017, 1019, 1020, 1021, 1022, 1023,
    1024,
};

static int32_t
sin_q10(int deg)
{
    deg %= 360;
    if( deg < 0 )
        deg += 360;
    if( deg <= 90 )
        return SIN_Q10[deg];
    if( deg <= 180 )
        return SIN_Q10[180 - deg];
    if( deg <= 270 )
        return -SIN_Q10[deg - 180];
    return -SIN_Q10[360 - deg];
}

static int32_t
cos_q10(int deg)
{
    return sin_q10(deg + 90);
}

static uint16_t
pack565(struct QrStyle_Rgb c, bool big_endian)
{
    uint16_t v = (uint16_t)(((c.r & 0xF8) << 8) | ((c.g & 0xFC) << 3) | (c.b >> 3));

    if( big_endian )
        v = (uint16_t)((v >> 8) | (v << 8));
    return v;
}

static struct QrStyle_Rgb
unpack565(uint16_t v, bool big_endian)
{
    struct QrStyle_Rgb c;
    unsigned r, g, b;

    if( big_endian )
        v = (uint16_t)((v >> 8) | (v << 8));

    r = (v >> 11) & 0x1F;
    g = (v >> 5) & 0x3F;
    b = v & 0x1F;
    /* Replicate the high bits into the low ones so 0x1F comes back as 255 and
     * a round trip through the panel's format is idempotent. */
    c.r = (uint8_t)((r << 3) | (r >> 2));
    c.g = (uint8_t)((g << 2) | (g >> 4));
    c.b = (uint8_t)((b << 3) | (b >> 2));
    return c;
}

static void
blend_rgb(struct QrStyle_Rgb *dst, struct QrStyle_Rgb src, int cov, int total)
{
    if( cov <= 0 )
        return;
    if( cov >= total )
    {
        *dst = src;
        return;
    }
    dst->r = (uint8_t)(dst->r + ((int)src.r - (int)dst->r) * cov / total);
    dst->g = (uint8_t)(dst->g + ((int)src.g - (int)dst->g) * cov / total);
    dst->b = (uint8_t)(dst->b + ((int)src.b - (int)dst->b) * cov / total);
}

static struct QrStyle_Rgb
lerp_rgb(struct QrStyle_Rgb a, struct QrStyle_Rgb b, int t)
{
    struct QrStyle_Rgb c;

    c.r = (uint8_t)(a.r + ((int)b.r - (int)a.r) * t / 255);
    c.g = (uint8_t)(a.g + ((int)b.g - (int)a.g) * t / 255);
    c.b = (uint8_t)(a.b + ((int)b.b - (int)a.b) * t / 255);
    return c;
}

/*---- Fills ----*/

struct FillCtx
{
    enum QrStyle_FillKind kind;
    struct QrStyle_Rgb from, to;
    int32_t ax, ay;  /* linear: Q10 direction */
    int32_t umin;    /* linear: projection of the nearest corner, whole pixels */
    int32_t uspan;   /* linear: projection range, whole pixels, >= 1 */
    int32_t cx, cy;  /* radial: centre, whole pixels */
    int32_t rmax;    /* radial: distance at which `to` is reached, >= 1 */
};

/*
 * A gradient is measured over the code's own box, so the ramp is a property of
 * the symbol rather than of wherever on the panel it was placed.
 */
static void
fill_prepare(struct FillCtx *fc, const struct QrStyle_Fill *f, int x0, int y0, int span)
{
    int32_t u[4];
    int i;

    fc->kind = f->kind;
    fc->from = f->from;
    fc->to = f->to;
    fc->ax = fc->ay = 0;
    fc->umin = 0;
    fc->uspan = 1;
    fc->cx = x0 + span / 2;
    fc->cy = y0 + span / 2;
    fc->rmax = 1;

    if( f->kind == QRSTYLE_FILL_LINEAR )
    {
        fc->ax = cos_q10(f->angle_deg);
        fc->ay = sin_q10(f->angle_deg);
        u[0] = (x0 * fc->ax + y0 * fc->ay) >> 10;
        u[1] = ((x0 + span) * fc->ax + y0 * fc->ay) >> 10;
        u[2] = (x0 * fc->ax + (y0 + span) * fc->ay) >> 10;
        u[3] = ((x0 + span) * fc->ax + (y0 + span) * fc->ay) >> 10;
        fc->umin = u[0];
        for( i = 1; i < 4; i++ )
            fc->umin = imin32(fc->umin, u[i]);
        fc->uspan = u[0];
        for( i = 1; i < 4; i++ )
            fc->uspan = imax32(fc->uspan, u[i]);
        fc->uspan = imax32(1, fc->uspan - fc->umin);
    }
    else if( f->kind == QRSTYLE_FILL_RADIAL )
    {
        /* Half the diagonal, so `to` lands on the corners rather than being
         * clipped away before the box even ends. */
        fc->rmax = imax32(1, (int32_t)isqrt32((uint32_t)(span * span / 2)));
    }
}

static struct QrStyle_Rgb
fill_at(const struct FillCtx *fc, int x, int y)
{
    int32_t t, dx, dy;

    switch( fc->kind )
    {
    case QRSTYLE_FILL_LINEAR:
        t = (((x * fc->ax + y * fc->ay) >> 10) - fc->umin) * 255 / fc->uspan;
        break;
    case QRSTYLE_FILL_RADIAL:
        dx = x - fc->cx;
        dy = y - fc->cy;
        t = (int32_t)isqrt32((uint32_t)(dx * dx + dy * dy)) * 255 / fc->rmax;
        break;
    default:
        return fc->from;
    }

    if( t < 0 )
        t = 0;
    if( t > 255 )
        t = 255;
    return lerp_rgb(fc->from, fc->to, t);
}

/*---- Presets ----*/

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

void
QrStyle_ConfigDefault(struct QrStyle_Config *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->module_shape = QRSTYLE_MODULE_SQUARE;
    cfg->module_size_pct = 100;
    cfg->eye_frame = QRSTYLE_EYE_SQUARE;
    cfg->eye_pupil = QRSTYLE_PUPIL_SQUARE;
    cfg->fg = solid(QRSTYLE_HEX(0x000000));
    cfg->eye_frame_fill = cfg->fg;
    cfg->eye_pupil_fill = cfg->fg;
    cfg->eyes_use_fg = true;
    cfg->bg = solid(QRSTYLE_HEX(0xFFFFFF));
    cfg->quiet_modules = 4;
    cfg->supersample = 3;
}

void
QrStyle_ConfigPreset(struct QrStyle_Config *cfg, enum QrStyle_Preset p)
{
    QrStyle_ConfigDefault(cfg);

    switch( p )
    {
    case QRSTYLE_PRESET_DOTS:
        cfg->module_shape = QRSTYLE_MODULE_CIRCLE;
        cfg->module_size_pct = 88;
        cfg->eye_frame = QRSTYLE_EYE_CIRCLE;
        cfg->eye_pupil = QRSTYLE_PUPIL_CIRCLE;
        break;

    case QRSTYLE_PRESET_LIQUID:
        cfg->module_shape = QRSTYLE_MODULE_LIQUID;
        cfg->eye_frame = QRSTYLE_EYE_ROUNDED;
        cfg->eye_pupil = QRSTYLE_PUPIL_ROUNDED;
        break;

    case QRSTYLE_PRESET_TEARDROP:
        cfg->module_shape = QRSTYLE_MODULE_ROUNDED;
        cfg->module_size_pct = 92;
        cfg->eye_frame = QRSTYLE_EYE_TEARDROP;
        cfg->eye_pupil = QRSTYLE_PUPIL_CIRCLE;
        break;

    case QRSTYLE_PRESET_DIAMOND:
        cfg->module_shape = QRSTYLE_MODULE_DIAMOND;
        cfg->module_size_pct = 100;
        cfg->eye_frame = QRSTYLE_EYE_LEAF;
        cfg->eye_pupil = QRSTYLE_PUPIL_DIAMOND;
        break;

    case QRSTYLE_PRESET_STAR:
        cfg->module_shape = QRSTYLE_MODULE_STAR;
        cfg->module_size_pct = 100;
        cfg->eye_frame = QRSTYLE_EYE_ROUNDED;
        cfg->eye_pupil = QRSTYLE_PUPIL_STAR;
        break;

    case QRSTYLE_PRESET_CLASSIC:
    default:
        break;
    }
}

/*---- Encoding ----*/

enum qrcodegen_Ecc
QrStyle_Ecl(const uint8_t qrcode[])
{
    /*
     * The two error correction bits are the top of the 15-bit format field,
     * which qrcodegen writes XORed with 0x5412 -- bit 14 at module (0,8) and
     * bit 13 at (1,8), see drawFormatBits. {1,0,3,2} maps the level to those
     * bits and, being its own inverse, maps them back.
     */
    static const enum qrcodegen_Ecc table[4] = {
        qrcodegen_Ecc_MEDIUM,
        qrcodegen_Ecc_LOW,
        qrcodegen_Ecc_HIGH,
        qrcodegen_Ecc_QUARTILE,
    };
    int hi = qrcodegen_getModule(qrcode, 0, 8) ? 0 : 1; /* ^ bit 14 of 0x5412 */
    int lo = qrcodegen_getModule(qrcode, 1, 8) ? 1 : 0; /* ^ bit 13, which is 0 */

    return table[(hi << 1) | lo];
}

bool
QrStyle_EncodeText(
    const char *text,
    const struct QrStyle_Config *cfg,
    uint8_t tmp[],
    uint8_t qrcode[],
    int max_version,
    enum qrcodegen_Ecc *out_ecl)
{
    enum qrcodegen_Ecc ecl =
        (cfg && cfg->logo) ? qrcodegen_Ecc_HIGH : qrcodegen_Ecc_MEDIUM;
    bool ok;

    if( max_version < qrcodegen_VERSION_MIN )
        max_version = qrcodegen_VERSION_MIN;
    if( max_version > qrcodegen_VERSION_MAX )
        max_version = qrcodegen_VERSION_MAX;

    ok = qrcodegen_encodeText(
        text,
        tmp,
        qrcode,
        ecl,
        qrcodegen_VERSION_MIN,
        max_version,
        qrcodegen_Mask_AUTO,
        /* boostEcl: raise the level for free when the chosen version has room
         * for it, which it often does. It never costs a module. */
        true);

    if( ok && out_ecl )
        *out_ecl = QrStyle_Ecl(qrcode);
    return ok;
}

/*---- Checking ----*/

int
QrStyle_LogoBudgetPct(enum qrcodegen_Ecc ecl)
{
    /*
     * Half of what the level can restore, spent on AREA, then expressed as a
     * width -- and capped at 30 regardless.
     *
     * The halving and the cap are both about the difference between the
     * standard's arithmetic and a scanner's day. Reed-Solomon restores 30% of
     * the codewords at HIGH, but a patch in the middle of the symbol destroys
     * them in a burst that interleaving only partly spreads across blocks, so
     * spending the whole budget on one contiguous hole is not the same bet as
     * spending it on scattered damage. The cap also keeps the timing patterns
     * and the centre alignment pattern clear, which no amount of error
     * correction can replace: they are how the decoder builds the grid it
     * then reads the codewords out of.
     *
     * LOW is 0. Seven percent is consumed by ordinary print and camera noise
     * before a logo is even considered.
     */
    static const uint8_t pct[4] = { 0, 22, 26, 30 };
    int i = (int)ecl;

    return (i >= 0 && i < 4) ? pct[i] : 0;
}

/* Gamma-encoded grey, which is what a binariser thresholds -- as opposed to
 * the linear luminance the contrast ratio is defined on. */
static int
grey_of(struct QrStyle_Rgb c)
{
    return ((int)c.r * 299 + (int)c.g * 587 + (int)c.b * 114) / 1000;
}

static int32_t
luminance_q16(struct QrStyle_Rgb c)
{
    return ((int32_t)SRGB_LINEAR[c.r] * 2126 + (int32_t)SRGB_LINEAR[c.g] * 7152 +
            (int32_t)SRGB_LINEAR[c.b] * 722) /
           10000;
}

static int
contrast_x10(struct QrStyle_Rgb a, struct QrStyle_Rgb b)
{
    /* 0.05 of full scale, the WCAG ambient-light term. */
    const int32_t amb = 65535 / 20;
    int32_t la = luminance_q16(a) + amb;
    int32_t lb = luminance_q16(b) + amb;
    int32_t hi = imax32(la, lb), lo = imin32(la, lb);

    return (int)((hi * 10 + lo / 2) / lo);
}

/* The worst contrast any shade of `fg` reaches against any shade of `bg`. */
static int
fill_contrast_x10(const struct QrStyle_Fill *fg, const struct QrStyle_Fill *bg)
{
    struct QrStyle_Rgb f[2] = { fg->from, fg->to };
    struct QrStyle_Rgb b[2] = { bg->from, bg->to };
    int nf = (fg->kind == QRSTYLE_FILL_SOLID) ? 1 : 2;
    int nb = (bg->kind == QRSTYLE_FILL_SOLID) ? 1 : 2;
    int worst = 1000, i, j, c;

    for( i = 0; i < nf; i++ )
        for( j = 0; j < nb; j++ )
        {
            c = contrast_x10(f[i], b[j]);
            if( c < worst )
                worst = c;
        }
    return worst;
}

bool
QrStyle_Check(
    const uint8_t qrcode[], const struct QrStyle_Config *cfg, struct QrStyle_Report *out)
{
    struct QrStyle_Report r;
    enum qrcodegen_Ecc ecl = QrStyle_Ecl(qrcode);
    int size = qrcodegen_getSize(qrcode);

    memset(&r, 0, sizeof(r));
    r.ok = true;
    r.contrast_x10 = -1;
    r.logo_budget_pct = QrStyle_LogoBudgetPct(ecl);

    if( !cfg->bg_transparent )
    {
        int cf = fill_contrast_x10(&cfg->fg, &cfg->bg);

        if( !cfg->eyes_use_fg )
        {
            int ce = fill_contrast_x10(&cfg->eye_frame_fill, &cfg->bg);
            int cp = fill_contrast_x10(&cfg->eye_pupil_fill, &cfg->bg);

            cf = imin32(cf, imin32(ce, cp));
        }
        r.contrast_x10 = cf;
        if( cf < QRSTYLE_MIN_CONTRAST_X10 )
        {
            r.ok = false;
            r.problem = "foreground and background are too close in luminance";
        }
    }

    /*
     * Every ink shade has to survive the one threshold a decoder will pick.
     * See QRSTYLE_INK_MARGIN_PCT.
     */
    if( !cfg->bg_transparent && !cfg->eyes_use_fg && r.ok )
    {
        int bg = (grey_of(cfg->bg.from) + grey_of(cfg->bg.to)) / 2;
        int fg_near = grey_of(cfg->fg.from);
        int fg_far = grey_of(cfg->fg.to);
        int fg, thr, margin, shade[4], i;

        if( cfg->fg.kind == QRSTYLE_FILL_SOLID )
            fg_far = fg_near;
        /* The endpoint NEAREST the background is the one that sets where the
         * threshold falls, so it is the one to reason from. */
        fg = (iabs32(fg_near - bg) < iabs32(fg_far - bg)) ? fg_near : fg_far;

        thr = (fg + bg) / 2;
        margin = iabs32(fg - bg) * QRSTYLE_INK_MARGIN_PCT / 100;

        shade[0] = grey_of(cfg->eye_frame_fill.from);
        shade[1] = grey_of(cfg->eye_frame_fill.to);
        shade[2] = grey_of(cfg->eye_pupil_fill.from);
        shade[3] = grey_of(cfg->eye_pupil_fill.to);

        for( i = 0; i < 4; i++ )
        {
            bool on_ink_side =
                (fg < bg) ? (shade[i] < thr - margin) : (shade[i] > thr + margin);

            if( !on_ink_side )
            {
                r.ok = false;
                r.problem = "an eye colour binarises to the background side of "
                            "the data ink";
                break;
            }
        }
    }

    if( cfg->logo )
    {
        /* The pad counts: an erased module is erased whether a picture or the
         * background colour replaced it. */
        int pad_pct = size > 0 ? (int)cfg->logo->pad_modules * 2 * 100 / size : 0;

        r.logo_area_pct = (int)cfg->logo->size_pct + pad_pct;
        if( r.logo_area_pct > r.logo_budget_pct )
        {
            r.ok = false;
            r.problem = "logo is larger than this error correction level can repair";
        }
    }

    if( cfg->quiet_modules < 4 && r.ok )
    {
        r.ok = false;
        r.problem = "quiet zone is under the 4 modules the standard requires";
    }

    if( out )
        *out = r;
    return r.ok;
}

int
QrStyle_PixelsPerModule(
    const uint8_t qrcode[], const struct QrStyle_Config *cfg, int box_size)
{
    int total = qrcodegen_getSize(qrcode) + 2 * (int)cfg->quiet_modules;

    return total > 0 ? box_size / total : 0;
}

/*---- Rendering ----*/

struct Ctx
{
    const uint8_t *qr;
    int size;
    const struct QrStyle_Config *cfg;
    enum QrStyle_ModuleShape shape;

    int32_t half;   /* module half-extent, Q8 */
    int32_t radius; /* ROUNDED corner radius, Q8 */

    int eye_x1; /* left module of the top-right finder */
    int eye_y2; /* top module of the bottom-left finder */
    int32_t eye_r[3][4];    /* outer corner radii, per eye, TL TR BR BL */
    int32_t eye_ri[3][4];   /* the same corners on the inner edge */
    int32_t pupil_r;        /* rounded/circular pupils */

    bool logo_knockout;
    int32_t pad_cx, pad_cy, pad_hw, pad_hh, pad_r; /* module space, Q8 */
};

/*
 * The corner radii of one eye.
 *
 * `out` is the corner that points away from the middle of the symbol: the
 * top-left one for the top-left eye, and so on. That is what makes three
 * teardrops mirror each other into a set instead of three copies of the same
 * rotation.
 */
static void
eye_radii(enum QrStyle_EyeFrame frame, int eye, int32_t r[4])
{
    static const int outward[3] = { 0, 1, 3 }; /* TL, TR, BL */
    int i, o = outward[eye];

    for( i = 0; i < 4; i++ )
    {
        switch( frame )
        {
        case QRSTYLE_EYE_ROUNDED:
            r[i] = EYE_HALF * 5 / 14; /* 2.5 modules of the 7 */
            break;
        case QRSTYLE_EYE_CIRCLE:
            r[i] = EYE_HALF;
            break;
        case QRSTYLE_EYE_TEARDROP:
            r[i] = (i == o) ? 0 : EYE_HALF;
            break;
        case QRSTYLE_EYE_LEAF:
            r[i] = (i == o || i == (o + 2) % 4) ? 0 : EYE_HALF;
            break;
        case QRSTYLE_EYE_SQUARE:
        default:
            r[i] = 0;
            break;
        }
    }
}

static void
ctx_init(struct Ctx *c, const uint8_t qr[], const struct QrStyle_Config *cfg)
{
    int e, i;

    c->qr = qr;
    c->size = qrcodegen_getSize(qr);
    c->cfg = cfg;
    c->shape = cfg->module_shape;

    if( c->shape == QRSTYLE_MODULE_LIQUID )
        c->half = HALFONE;
    else
    {
        int pct = cfg->module_size_pct;

        if( pct < 55 )
            pct = 55;
        if( pct > 100 )
            pct = 100;
        c->half = HALFONE * pct / 100;
    }

    c->radius = (c->shape == QRSTYLE_MODULE_CIRCLE) ? c->half
                : (c->shape == QRSTYLE_MODULE_ROUNDED) ? c->half * 45 / 100
                                                       : 0;

    c->eye_x1 = c->size - 7;
    c->eye_y2 = c->size - 7;
    for( e = 0; e < 3; e++ )
    {
        eye_radii(cfg->eye_frame, e, c->eye_r[e]);
        for( i = 0; i < 4; i++ )
            /* One module thinner, so the ring keeps a constant thickness
             * instead of pinching at the corners. */
            c->eye_ri[e][i] = imax32(0, c->eye_r[e][i] - ONE);
    }

    c->pupil_r = (cfg->eye_pupil == QRSTYLE_PUPIL_CIRCLE)    ? EYE_PUPIL
                 : (cfg->eye_pupil == QRSTYLE_PUPIL_ROUNDED) ? EYE_PUPIL * 2 / 5
                                                             : 0;

    c->logo_knockout = false;
    c->pad_cx = c->pad_cy = c->pad_hw = c->pad_hh = c->pad_r = 0;
}

/* Which finder block a module is in, or -1. */
static int
eye_of(const struct Ctx *c, int mx, int my)
{
    if( mx < 7 && my < 7 )
        return 0;
    if( mx >= c->eye_x1 && my < 7 )
        return 1;
    if( mx < 7 && my >= c->eye_y2 )
        return 2;
    return -1;
}

static int
eye_class(const struct Ctx *c, int eye, int32_t fx, int32_t fy)
{
    int32_t ox = (eye == 1) ? (int32_t)c->eye_x1 * ONE : 0;
    int32_t oy = (eye == 2) ? (int32_t)c->eye_y2 * ONE : 0;
    int32_t dx = fx - (ox + EYE_HALF);
    int32_t dy = fy - (oy + EYE_HALF);
    const int32_t *r = c->eye_r[eye];
    const int32_t *ri = c->eye_ri[eye];

    switch( c->cfg->eye_pupil )
    {
    case QRSTYLE_PUPIL_DIAMOND:
        if( diamond_in(dx, dy, EYE_PUPIL) )
            return CLS_EYE_PUPIL;
        break;
    case QRSTYLE_PUPIL_STAR:
        if( star_in(dx, dy, EYE_PUPIL) )
            return CLS_EYE_PUPIL;
        break;
    default:
        if( rrect_in(
                dx, dy, EYE_PUPIL, EYE_PUPIL, c->pupil_r, c->pupil_r, c->pupil_r,
                c->pupil_r) )
            return CLS_EYE_PUPIL;
        break;
    }

    if( rrect_in(dx, dy, EYE_HALF, EYE_HALF, r[0], r[1], r[2], r[3]) &&
        !rrect_in(dx, dy, EYE_INNER, EYE_INNER, ri[0], ri[1], ri[2], ri[3]) )
        return CLS_EYE_FRAME;

    return CLS_NONE;
}

static bool
module_in(const struct Ctx *c, int mx, int my, int32_t dx, int32_t dy)
{
    int32_t h = c->half;

    switch( c->shape )
    {
    case QRSTYLE_MODULE_DIAMOND:
        return diamond_in(dx, dy, h);
    case QRSTYLE_MODULE_CROSS:
        return cross_in(dx, dy, h);
    case QRSTYLE_MODULE_STAR:
        return star_in(dx, dy, h);
    case QRSTYLE_MODULE_LIQUID:
    {
        /*
         * A corner is rounded only where both of the neighbours that meet at
         * it are light. One dark neighbour squares the corner off, and the two
         * squared corners the pair then shares is what makes the run read as
         * one continuous form rather than two touching blobs.
         */
        bool l = qrcodegen_getModule(c->qr, mx - 1, my);
        bool rr = qrcodegen_getModule(c->qr, mx + 1, my);
        bool u = qrcodegen_getModule(c->qr, mx, my - 1);
        bool d = qrcodegen_getModule(c->qr, mx, my + 1);
        int32_t R = HALFONE;

        return rrect_in(
            dx, dy, h, h, (l || u) ? 0 : R, (rr || u) ? 0 : R, (rr || d) ? 0 : R,
            (l || d) ? 0 : R);
    }
    default:
        return rrect_in(dx, dy, h, h, c->radius, c->radius, c->radius, c->radius);
    }
}

/* Which of the three ink classes, if any, covers this point in module space. */
static int
classify(const struct Ctx *c, int32_t fx, int32_t fy)
{
    int mx, my, eye;

    if( fx < 0 || fy < 0 )
        return CLS_NONE;
    mx = fx >> Q;
    my = fy >> Q;
    if( mx >= c->size || my >= c->size )
        return CLS_NONE;

    if( c->logo_knockout &&
        rrect_in(
            fx - c->pad_cx, fy - c->pad_cy, c->pad_hw, c->pad_hh, c->pad_r, c->pad_r,
            c->pad_r, c->pad_r) )
        return CLS_NONE;

    eye = eye_of(c, mx, my);
    if( eye >= 0 )
        return eye_class(c, eye, fx, fy);

    if( !qrcodegen_getModule(c->qr, mx, my) )
        return CLS_NONE;

    return module_in(c, mx, my, (fx & (ONE - 1)) - HALFONE, (fy & (ONE - 1)) - HALFONE)
               ? CLS_FG
               : CLS_NONE;
}

void
QrStyle_Render(
    const uint8_t qrcode[],
    const struct QrStyle_Config *cfg,
    const struct QrStyle_Target *tg,
    const struct QrStyle_Box *box)
{
    struct Ctx c;
    struct FillCtx fbg, fcls[CLS_COUNT];
    int quiet = cfg->quiet_modules;
    int size = qrcodegen_getSize(qrcode);
    int total = size + 2 * quiet;
    int ppm, draw, ox, oy, code_x0, code_y0, code_px;
    int ss, nsamp, s;
    int32_t subo[4];
    int x, y, cx0, cx1, cy0, cy1;
    /* The logo's image rectangle in whole pixels. */
    int img_x0 = 0, img_y0 = 0, img_w = 0, img_h = 0;
    bool has_img = false;

    if( total <= 0 || box->size <= 0 || tg->pixels == NULL )
        return;

    ppm = box->size / total;
    if( ppm < 1 )
        return; /* nothing legible fits; drawing it anyway would be a lie */

    draw = ppm * total;
    ox = box->x + (box->size - draw) / 2;
    oy = box->y + (box->size - draw) / 2;
    code_x0 = ox + quiet * ppm;
    code_y0 = oy + quiet * ppm;
    code_px = size * ppm;

    ss = cfg->supersample;
    if( ss < 1 )
        ss = 1;
    if( ss > 4 )
        ss = 4;
    nsamp = ss * ss;
    for( s = 0; s < ss; s++ )
        subo[s] = ((2 * s + 1) << Q) / (2 * ss);

    ctx_init(&c, qrcode, cfg);

    if( cfg->logo && cfg->logo->rgb565 && cfg->logo->w && cfg->logo->h )
    {
        const struct QrStyle_Logo *lg = cfg->logo;
        int boxpx = (int)lg->size_pct * code_px / 100;
        int pad = (int)lg->pad_modules * ppm;
        int pad_x0, pad_y0, pad_w, pad_h;

        if( boxpx > 0 )
        {
            /* Fit the image inside a square box without distorting it. */
            if( lg->w >= lg->h )
            {
                img_w = boxpx;
                img_h = boxpx * lg->h / lg->w;
            }
            else
            {
                img_h = boxpx;
                img_w = boxpx * lg->w / lg->h;
            }
            img_x0 = code_x0 + (code_px - img_w) / 2;
            img_y0 = code_y0 + (code_px - img_h) / 2;
            has_img = img_w > 0 && img_h > 0;

            pad_x0 = img_x0 - pad;
            pad_y0 = img_y0 - pad;
            pad_w = img_w + 2 * pad;
            pad_h = img_h + 2 * pad;

            /* Into module space, which is where classify() works. */
            c.logo_knockout = true;
            c.pad_hw = (int32_t)pad_w * ONE / (2 * ppm);
            c.pad_hh = (int32_t)pad_h * ONE / (2 * ppm);
            c.pad_cx = (int32_t)(pad_x0 - code_x0) * ONE / ppm + c.pad_hw;
            c.pad_cy = (int32_t)(pad_y0 - code_y0) * ONE / ppm + c.pad_hh;
            c.pad_r = (int32_t)lg->pad_radius_pct * imin32(c.pad_hw, c.pad_hh) / 100;
        }
    }

    fill_prepare(&fbg, &cfg->bg, ox, oy, draw);
    fill_prepare(&fcls[CLS_FG - 1], &cfg->fg, ox, oy, draw);
    fill_prepare(
        &fcls[CLS_EYE_FRAME - 1],
        cfg->eyes_use_fg ? &cfg->fg : &cfg->eye_frame_fill,
        ox,
        oy,
        draw);
    fill_prepare(
        &fcls[CLS_EYE_PUPIL - 1],
        cfg->eyes_use_fg ? &cfg->fg : &cfg->eye_pupil_fill,
        ox,
        oy,
        draw);

    /* Pass one: the background, over the caller's whole box rather than the
     * snapped code, so the slack at the edges is not a hole. */
    cx0 = imax32(box->x, tg->origin_x);
    cy0 = imax32(box->y, tg->origin_y);
    cx1 = imin32(box->x + box->size, tg->origin_x + tg->width);
    cy1 = imin32(box->y + box->size, tg->origin_y + tg->height);

    if( !cfg->bg_transparent )
        for( y = cy0; y < cy1; y++ )
        {
            uint16_t *row = tg->pixels + (size_t)(y - tg->origin_y) * tg->stride;

            for( x = cx0; x < cx1; x++ )
                row[x - tg->origin_x] = pack565(fill_at(&fbg, x, y), tg->big_endian);
        }

    /* Pass two: the code itself, blended over whatever pass one left. */
    cx0 = imax32(ox, tg->origin_x);
    cy0 = imax32(oy, tg->origin_y);
    cx1 = imin32(ox + draw, tg->origin_x + tg->width);
    cy1 = imin32(oy + draw, tg->origin_y + tg->height);

    for( y = cy0; y < cy1; y++ )
    {
        uint16_t *row = tg->pixels + (size_t)(y - tg->origin_y) * tg->stride;

        for( x = cx0; x < cx1; x++ )
        {
            int cov[CLS_COUNT] = { 0, 0, 0 };
            int32_t lr = 0, lg_ = 0, lb = 0;
            int la = 0;
            struct QrStyle_Rgb px;
            int sx, sy, k;

            for( sy = 0; sy < ss; sy++ )
            {
                int32_t spy = ((int32_t)y << Q) + subo[sy];
                int32_t fy = (spy - ((int32_t)oy << Q)) / ppm - (int32_t)quiet * ONE;

                for( sx = 0; sx < ss; sx++ )
                {
                    int32_t spx = ((int32_t)x << Q) + subo[sx];
                    int32_t fx =
                        (spx - ((int32_t)ox << Q)) / ppm - (int32_t)quiet * ONE;
                    int cls = classify(&c, fx, fy);

                    if( cls != CLS_NONE )
                        cov[cls - 1]++;

                    if( has_img )
                    {
                        int32_t rx = spx - ((int32_t)img_x0 << Q);
                        int32_t ry = spy - ((int32_t)img_y0 << Q);

                        if( rx >= 0 && ry >= 0 )
                        {
                            int u = (int)(rx * cfg->logo->w / ((int32_t)img_w << Q));
                            int v = (int)(ry * cfg->logo->h / ((int32_t)img_h << Q));

                            if( u < cfg->logo->w && v < cfg->logo->h )
                            {
                                size_t idx = (size_t)v * cfg->logo->w + (size_t)u;
                                bool opaque = true;

                                if( cfg->logo->alpha_bits )
                                {
                                    size_t stride_bits = ((size_t)cfg->logo->w + 7) / 8;
                                    size_t byte = (size_t)v * stride_bits + (size_t)u / 8;

                                    opaque = (cfg->logo->alpha_bits[byte] >>
                                              (7 - ((size_t)u & 7))) &
                                             1;
                                }
                                if( opaque )
                                {
                                    struct QrStyle_Rgb lc = unpack565(
                                        cfg->logo->rgb565[idx],
                                        cfg->logo->rgb565_big_endian);

                                    lr += lc.r;
                                    lg_ += lc.g;
                                    lb += lc.b;
                                    la++;
                                }
                            }
                        }
                    }
                }
            }

            if( cov[0] == 0 && cov[1] == 0 && cov[2] == 0 && la == 0 )
                continue;

            px = unpack565(row[x - tg->origin_x], tg->big_endian);
            for( k = 0; k < CLS_COUNT; k++ )
                if( cov[k] )
                    blend_rgb(&px, fill_at(&fcls[k], x, y), cov[k], nsamp);

            if( la )
            {
                struct QrStyle_Rgb lc;

                lc.r = (uint8_t)(lr / la);
                lc.g = (uint8_t)(lg_ / la);
                lc.b = (uint8_t)(lb / la);
                blend_rgb(&px, lc, la, nsamp);
            }

            row[x - tg->origin_x] = pack565(px, tg->big_endian);
        }
    }
}
