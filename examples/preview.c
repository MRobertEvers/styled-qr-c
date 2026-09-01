/*
 * Host tool: render the qrstyle examples to PNG and then try to READ them
 * back the way a scanner would.
 *
 * WHY THIS EXISTS AS A TEST AND NOT JUST A PREVIEW. A styled QR code that does
 * not scan looks exactly like one that does, in a picture, on a screen, to a
 * person. The only honest check is to do what a decoder does to the rendered
 * pixels: binarise them, find the finder patterns by their 1:1:3:1:1 run, and
 * sample the centre of every module -- and then compare that against the
 * matrix qrcodegen produced. Everything below the rendering is that check.
 *
 * It is not a decoder. It does not do perspective, it knows where the code is,
 * and it never runs Reed-Solomon -- so a pass here is not a promise that a
 * particular phone in particular light will read it. What a failure here IS,
 * is proof that the style broke the symbol, and that is the failure worth
 * catching before it reaches a panel somebody has already glued shut.
 *
 * Build it with `make examples`, or `.\build.ps1` on Windows -- both are
 * one call to the compiler:
 *
 *   cc -O2 -Isrc -Ithird_party/qrcodegen -Iexamples -o preview \
 *      third_party/qrcodegen/qrcodegen.c src/qrstyle.c examples/preview.c
 *
 * Then `./preview assets/examples` rewrites the images in the README.
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "qrcodegen.h"
#include "qrstyle.h"

#include "logo_tori.h"

#define MAX_VERSION 10
#define GALLERY_PX  560
#define PANEL_PX    240
#define STRIP_ROWS  120

/*---- PNG ----*/

static uint32_t
crc32_of(const uint8_t *p, size_t n, uint32_t crc)
{
    static uint32_t table[256];
    static bool built = false;
    size_t i;

    if( !built )
    {
        uint32_t c;
        int k, j;

        for( k = 0; k < 256; k++ )
        {
            c = (uint32_t)k;
            for( j = 0; j < 8; j++ )
                c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : (c >> 1);
            table[k] = c;
        }
        built = true;
    }

    crc = ~crc;
    for( i = 0; i < n; i++ )
        crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

static uint32_t
adler32_of(const uint8_t *p, size_t n)
{
    uint32_t a = 1, b = 0;
    size_t i;

    for( i = 0; i < n; i++ )
    {
        a = (a + p[i]) % 65521;
        b = (b + a) % 65521;
    }
    return (b << 16) | a;
}

static void
put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static void
png_chunk(FILE *f, const char *type, const uint8_t *data, size_t len)
{
    uint8_t hdr[8];
    uint32_t crc;

    put_be32(hdr, (uint32_t)len);
    memcpy(hdr + 4, type, 4);
    fwrite(hdr, 1, 8, f);
    if( len )
        fwrite(data, 1, len, f);

    crc = crc32_of((const uint8_t *)type, 4, 0);
    crc = crc32_of(data, len, crc);
    put_be32(hdr, crc);
    fwrite(hdr, 1, 4, f);
}

/*
 * A PNG with STORED deflate blocks -- no compression at all.
 *
 * The alternative is linking zlib into a preview tool, on a machine whose only
 * compiler is the one this repo vendors for the bake tools. A 560x560 preview
 * is 940 KB uncompressed and it is written to a scratch directory, so the
 * trade is a megabyte of temporary file against a dependency in the build.
 */
static bool
png_write_rgb(const char *path, const uint8_t *rgb, int w, int h)
{
    FILE *f = fopen(path, "wb");
    static const uint8_t sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
    uint8_t ihdr[13];
    size_t raw_len = (size_t)h * (1 + (size_t)w * 3);
    uint8_t *raw, *z;
    size_t zi = 0, off = 0;
    int y;

    if( !f )
        return false;

    raw = malloc(raw_len);
    z = malloc(raw_len + raw_len / 65535 * 5 + 64);
    if( !raw || !z )
    {
        free(raw);
        free(z);
        fclose(f);
        return false;
    }

    for( y = 0; y < h; y++ )
    {
        raw[off++] = 0; /* filter: none */
        memcpy(raw + off, rgb + (size_t)y * w * 3, (size_t)w * 3);
        off += (size_t)w * 3;
    }

    z[zi++] = 0x78;
    z[zi++] = 0x01;
    off = 0;
    while( off < raw_len )
    {
        size_t n = raw_len - off;
        int last;

        if( n > 65535 )
            n = 65535;
        last = (off + n >= raw_len);
        z[zi++] = (uint8_t)(last ? 1 : 0);
        z[zi++] = (uint8_t)(n & 0xFF);
        z[zi++] = (uint8_t)(n >> 8);
        z[zi++] = (uint8_t)(~n & 0xFF);
        z[zi++] = (uint8_t)((~n >> 8) & 0xFF);
        memcpy(z + zi, raw + off, n);
        zi += n;
        off += n;
    }
    put_be32(z + zi, adler32_of(raw, raw_len));
    zi += 4;

    fwrite(sig, 1, 8, f);
    put_be32(ihdr, (uint32_t)w);
    put_be32(ihdr + 4, (uint32_t)h);
    ihdr[8] = 8;  /* bit depth */
    ihdr[9] = 2;  /* colour type: truecolour */
    ihdr[10] = 0; /* deflate */
    ihdr[11] = 0; /* filter method */
    ihdr[12] = 0; /* no interlace */
    png_chunk(f, "IHDR", ihdr, sizeof(ihdr));
    png_chunk(f, "IDAT", z, zi);
    png_chunk(f, "IEND", NULL, 0);

    fclose(f);
    free(raw);
    free(z);
    return true;
}

/*---- What a scanner sees ----*/

static bool g_verbose = false;

static int
luma(const uint8_t *rgb, int w, int x, int y)
{
    const uint8_t *p = rgb + ((size_t)y * w + x) * 3;

    return (p[0] * 299 + p[1] * 587 + p[2] * 114) / 1000;
}

/*
 * Otsu's threshold over the module centres.
 *
 * A fixed 50% grey would pass a black-on-white code and fail a code that is
 * correct but drawn in two mid-tones, which is exactly the case gradients
 * introduce. A binariser that picks its own threshold is also what every
 * scanner actually does.
 */
static int
otsu(const int *hist, int total)
{
    long sum = 0, sum_b = 0;
    int wb = 0, wf, i, best_t = 128;
    double best_var = -1.0;

    for( i = 0; i < 256; i++ )
        sum += (long)i * hist[i];

    for( i = 0; i < 256; i++ )
    {
        wb += hist[i];
        if( wb == 0 )
            continue;
        wf = total - wb;
        if( wf == 0 )
            break;
        sum_b += (long)i * hist[i];
        {
            double mb = (double)sum_b / wb;
            double mf = (double)(sum - sum_b) / wf;
            double var = (double)wb * wf * (mb - mf) * (mb - mf);

            if( var > best_var )
            {
                best_var = var;
                best_t = i;
            }
        }
    }
    return best_t;
}

struct Scan
{
    int threshold;
    bool inverted; /* the ink is lighter than the paper */
    int modules_checked;
    int modules_wrong;      /* anywhere a decoder reads: must be zero */
    int modules_wrong_logo; /* under the logo: erasures, and expected */
    int modules_restyled;   /* inside a finder block, where a style may redraw
                             * the matrix -- see below */
    int finders_ok;         /* of 6: three eyes, scanned across and down */
    double worst_finder_err; /* worst run-length deviation, in modules */
};

/*
 * One finder-pattern cross, the way ZXing's FinderPatternFinder scores it:
 * five runs through the centre whose widths must be 1:1:3:1:1, each within
 * half a module of nominal.
 */
/*
 * Ink, not darkness.
 *
 * Otsu splits the histogram into [0..t] and [t+1..255], so the threshold bin
 * belongs to the LOW class -- `<` instead of `<=` puts every pixel of a
 * two-tone image on the same side and was worth catching. `inverted` is the
 * other half of the same idea: a light-on-dark code is a legal code, and which
 * class is the ink is a fact about the quiet zone, which is background by
 * definition.
 */
static bool
is_ink(const uint8_t *rgb, int w, int x, int y, int thr, bool inverted)
{
    bool low = luma(rgb, w, x, y) <= thr;

    return inverted ? !low : low;
}

static bool
finder_cross(
    const uint8_t *rgb, int w, int h, int thr, bool inverted, int cx, int cy,
    bool horizontal, double *worst_err, int run[5], const char **why)
{
    int i, x, y, step;
    double total, unit, err, e;

    run[0] = run[1] = run[2] = run[3] = run[4] = 0;
    *why = "ok";
    if( cx < 0 || cy < 0 || cx >= w || cy >= h )
    {
        *why = "centre is off the image";
        return false;
    }
    if( !is_ink(rgb, w, cx, cy, thr, inverted) )
    {
        *why = "centre is not ink";
        return false; /* the pupil has to be ink or there is no cross here */
    }

    /* Middle run, counted outward from the centre in both directions. */
    run[2] = 1;
    for( step = -1; step <= 1; step += 2 )
    {
        x = cx;
        y = cy;
        for( ;; )
        {
            if( horizontal )
                x += step;
            else
                y += step;
            if( x < 0 || y < 0 || x >= w || y >= h )
            {
                *why = "ran off the image inside the pupil";
                return false;
            }
            if( !is_ink(rgb, w, x, y, thr, inverted) )
                break;
            run[2]++;
        }

        /* Then the light run and the dark run beyond it. */
        for( i = 0; i < 2; i++ )
        {
            int idx = (step < 0) ? (1 - i) : (3 + i);
            bool want_dark = (i == 1);

            for( ;; )
            {
                bool dark;

                if( x < 0 || y < 0 || x >= w || y >= h )
                {
                    *why = "ran off the image before five runs closed";
                    return false;
                }
                dark = is_ink(rgb, w, x, y, thr, inverted);
                if( dark != want_dark )
                    break;
                run[idx]++;
                if( horizontal )
                    x += step;
                else
                    y += step;
            }
        }
    }

    total = run[0] + run[1] + run[2] + run[3] + run[4];
    if( total < 7 )
    {
        *why = "the cross is under seven pixels wide";
        return false;
    }
    unit = total / 7.0;

    err = 0.0;
    for( i = 0; i < 5; i++ )
    {
        double nominal = (i == 2) ? 3.0 * unit : unit;

        e = (run[i] - nominal) / unit;
        if( e < 0 )
            e = -e;
        if( e > err )
            err = e;
    }
    if( err > *worst_err )
        *worst_err = err;

    /* ZXing allows half a module on the 1s and, proportionally, on the 3. */
    for( i = 0; i < 5; i++ )
    {
        double nominal = (i == 2) ? 3.0 * unit : unit;
        double tol = (i == 2) ? 1.5 * unit : 0.5 * unit;

        if( run[i] - nominal > tol || nominal - run[i] > tol )
        {
            *why = "run widths are not 1:1:3:1:1";
            return false;
        }
    }
    return true;
}

/* The 7x7 finder and the separator ringing it: the three 8x8 corner blocks. */
static bool
in_finder_block(int size, int mx, int my)
{
    return (mx < 8 && my < 8) || (mx >= size - 8 && my < 8) ||
           (mx < 8 && my >= size - 8);
}

static void
scan_rendered(
    const uint8_t *rgb,
    int w,
    int h,
    const uint8_t qr[],
    const struct QrStyle_Config *cfg,
    int box_x,
    int box_y,
    int box_size,
    struct Scan *out)
{
    int size = qrcodegen_getSize(qr);
    int quiet = cfg->quiet_modules;
    int total_mod = size + 2 * quiet;
    int ppm = box_size / total_mod;
    int draw = ppm * total_mod;
    int ox = box_x + (box_size - draw) / 2;
    int oy = box_y + (box_size - draw) / 2;
    int code_x0 = ox + quiet * ppm;
    int code_y0 = oy + quiet * ppm;
    int code_px = size * ppm;
    int hist[256];
    int mx, my, e;
    /* The logo's erasure, recomputed here from the config rather than shared
     * with the renderer, so the two have to agree independently. */
    int lx0 = 0, ly0 = 0, lx1 = -1, ly1 = -1;

    memset(out, 0, sizeof(*out));
    memset(hist, 0, sizeof(hist));

    if( cfg->logo && cfg->logo->rgb565 )
    {
        int boxpx = (int)cfg->logo->size_pct * code_px / 100;
        int pad = (int)cfg->logo->pad_modules * ppm;
        int iw, ih;

        if( cfg->logo->w >= cfg->logo->h )
        {
            iw = boxpx;
            ih = boxpx * cfg->logo->h / cfg->logo->w;
        }
        else
        {
            ih = boxpx;
            iw = boxpx * cfg->logo->w / cfg->logo->h;
        }
        lx0 = (code_px - iw) / 2 - pad;
        ly0 = (code_px - ih) / 2 - pad;
        lx1 = lx0 + iw + 2 * pad;
        ly1 = ly0 + ih + 2 * pad;
        /* into modules, rounded outward */
        lx0 = lx0 / ppm;
        ly0 = ly0 / ppm;
        lx1 = (lx1 + ppm - 1) / ppm;
        ly1 = (ly1 + ppm - 1) / ppm;
    }

    for( my = 0; my < size; my++ )
        for( mx = 0; mx < size; mx++ )
        {
            int x = code_x0 + mx * ppm + ppm / 2;
            int y = code_y0 + my * ppm + ppm / 2;

            if( x >= 0 && y >= 0 && x < w && y < h )
                hist[luma(rgb, w, x, y)]++;
        }
    out->threshold = otsu(hist, size * size);

    /* The quiet zone is background by definition, so if it reads as the dark
     * class then the ink is the light one and the symbol is inverted. */
    {
        int qx = ox + ppm / 2, qy = oy + ppm / 2;

        if( qx >= 0 && qy >= 0 && qx < w && qy < h )
            out->inverted = luma(rgb, w, qx, qy) <= out->threshold;
    }

    for( my = 0; my < size; my++ )
        for( mx = 0; mx < size; mx++ )
        {
            int x = code_x0 + mx * ppm + ppm / 2;
            int y = code_y0 + my * ppm + ppm / 2;
            bool want, got, in_logo;

            if( x < 0 || y < 0 || x >= w || y >= h )
                continue;
            want = qrcodegen_getModule(qr, mx, my);
            got = is_ink(rgb, w, x, y, out->threshold, out->inverted);
            in_logo = (mx >= lx0 && mx < lx1 && my >= ly0 && my < ly1);

            out->modules_checked++;
            if( want != got )
            {
                /*
                 * A circular eye leaves the corners of the 7x7 finder light
                 * where the matrix has them dark, and fills the corners of the
                 * separator where the matrix has them light. That is the style
                 * working, not a defect: no decoder reads a finder pattern
                 * module by module -- it LOCATES the pattern by the 1:1:3:1:1
                 * run, which is checked separately and is the thing that has to
                 * hold. So the three corner blocks are counted and reported,
                 * not failed.
                 *
                 * Everything else -- data, timing, alignment, format -- is read
                 * module by module by a decoder, and has to come back exactly.
                 */
                if( in_finder_block(size, mx, my) )
                    out->modules_restyled++;
                else if( in_logo )
                    out->modules_wrong_logo++;
                else
                    out->modules_wrong++;
            }
        }

    for( e = 0; e < 3; e++ )
    {
        int bx = (e == 1) ? size - 7 : 0;
        int by = (e == 2) ? size - 7 : 0;
        int cx = code_x0 + bx * ppm + 7 * ppm / 2;
        int cy = code_y0 + by * ppm + 7 * ppm / 2;
        int run[5];
        const char *why;
        int dir;

        for( dir = 0; dir < 2; dir++ )
        {
            bool ok = finder_cross(
                rgb, w, h, out->threshold, out->inverted, cx, cy, dir == 0,
                &out->worst_finder_err, run, &why);

            if( ok )
                out->finders_ok++;
            else if( g_verbose )
                printf(
                    "      eye %d %s: runs %d/%d/%d/%d/%d -- %s\n",
                    e,
                    dir == 0 ? "across" : "down",
                    run[0], run[1], run[2], run[3], run[4], why);
        }
    }
}

/*---- Examples ----*/

static const struct QrStyle_Logo TORI = {
    .rgb565 = tori_logo_rgb565,
    .alpha_bits = tori_logo_alpha,
    .w = TORI_LOGO_W,
    .h = TORI_LOGO_H,
    .rgb565_big_endian = false,
    .size_pct = 22,
    .pad_modules = 1,
    .pad_radius_pct = 30,
};

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

struct Example
{
    const char *file;
    const char *title;
    const char *note;
    /* Render at 240x240 in the panel's byte order, as two strips, instead of
     * at gallery size. */
    bool panel;
};

/*
 * The palette is the sprite's own: the near-black of its outline, the browns
 * of the hair and the warm paper it wants to sit on. A logo that shares no
 * colour with the code it sits in reads as a sticker over a QR code rather
 * than one object, and the contrast check below is satisfied either way -- so
 * this part is taste, and it is the part that decides whether it looks good.
 */
#define INK        QRSTYLE_HEX(0x241610)
#define INK_WARM   QRSTYLE_HEX(0x4A2C18)
#define INK_COPPER QRSTYLE_HEX(0x8A4B26)
#define PAPER      QRSTYLE_HEX(0xFAF3E7)
#define PAPER_WARM QRSTYLE_HEX(0xF3E4CE)
#define PINE       QRSTYLE_HEX(0x1C4B3C)
#define PINE_LIGHT QRSTYLE_HEX(0x2F7A5C)
#define NIGHT      QRSTYLE_HEX(0x140F0A)
#define CANDLE     QRSTYLE_HEX(0xF0D9AC)
/*
 * The eye colour for a dark design, and the reason it is this light.
 *
 * PINE_LIGHT was here first and it looks better -- and the preview's own
 * scanner could not find a single finder pattern, because a decoder's one
 * threshold sits near the midpoint of CANDLE and NIGHT and PINE_LIGHT falls
 * below it. QrStyle_Check refuses that pairing now; this is what it was
 * replaced with.
 */
#define MINT       QRSTYLE_HEX(0x86D6B0)

/*
 * Kansas City Current, read off the crest: teal ground, navy shield, red band,
 * white marks. KC_TEAL_DEEP and KC_RED_DEEP are the same two hues taken darker,
 * for the cases where the palette colour has to be INK rather than ground --
 * see the note above the KC examples for why bright teal cannot be ink on white
 * and bright red cannot be an accent on teal.
 */
#define KC_TEAL      QRSTYLE_HEX(0x62C9C8)
#define KC_TEAL_DEEP QRSTYLE_HEX(0x17615E)
#define KC_NAVY      QRSTYLE_HEX(0x16283C)
#define KC_RED       QRSTYLE_HEX(0xD93A44)
#define KC_RED_DEEP  QRSTYLE_HEX(0xA11F2A)
#define KC_WHITE     QRSTYLE_HEX(0xF4F8F8)

static int
build_config(int index, struct QrStyle_Config *cfg, struct Example *ex)
{
    QrStyle_ConfigDefault(cfg);
    memset(ex, 0, sizeof(*ex));

    switch( index )
    {
    case 0:
        QrStyle_ConfigPreset(cfg, QRSTYLE_PRESET_CLASSIC);
        ex->file = "01-classic-control.png";
        ex->title = "Classic (control)";
        ex->note = "No style, no logo. If the checker ever fails on this one, the "
                   "checker is what broke.";
        break;

    case 1:
        QrStyle_ConfigPreset(cfg, QRSTYLE_PRESET_DOTS);
        cfg->fg = solid(INK);
        cfg->bg = solid(PAPER);
        cfg->logo = &TORI;
        ex->file = "02-dots.png";
        ex->title = "Dots";
        ex->note = "Circular modules at 88% and circular eyes. The plainest way to "
                   "stop looking like a barcode.";
        break;

    case 2:
        QrStyle_ConfigPreset(cfg, QRSTYLE_PRESET_LIQUID);
        cfg->fg = grad(INK_WARM, INK_COPPER, 55);
        cfg->eyes_use_fg = false;
        cfg->eye_frame_fill = solid(INK);
        cfg->eye_pupil_fill = solid(INK_COPPER);
        cfg->bg = solid(PAPER);
        cfg->logo = &TORI;
        ex->file = "03-liquid.png";
        ex->title = "Liquid";
        ex->note = "Neighbouring modules fuse; a lone one becomes a dot. Full cell "
                   "coverage, so this is the safest style here as well as the "
                   "loudest.";
        break;

    case 3:
        QrStyle_ConfigPreset(cfg, QRSTYLE_PRESET_TEARDROP);
        cfg->fg = radial(PINE, PINE_LIGHT);
        cfg->eyes_use_fg = false;
        cfg->eye_frame_fill = solid(PINE);
        cfg->eye_pupil_fill = solid(INK_COPPER);
        cfg->bg = solid(PAPER_WARM);
        cfg->logo = &TORI;
        ex->file = "04-teardrop.png";
        ex->title = "Teardrop";
        ex->note = "Each eye's sharp corner points away from the middle, so the "
                   "three of them mirror into a set. Radial gradient on the data.";
        break;

    case 4:
        QrStyle_ConfigPreset(cfg, QRSTYLE_PRESET_DIAMOND);
        cfg->fg = grad(PINE, INK, 90);
        cfg->eyes_use_fg = false;
        cfg->eye_frame_fill = solid(PINE);
        cfg->eye_pupil_fill = solid(PINE);
        cfg->bg = solid(PAPER);
        cfg->logo = &TORI;
        ex->file = "05-diamond.png";
        ex->title = "Diamond";
        ex->note = "Diamond modules with leaf eyes -- two sharp corners on the "
                   "outward diagonal.";
        break;

    case 5:
        QrStyle_ConfigPreset(cfg, QRSTYLE_PRESET_STAR);
        cfg->fg = solid(INK_WARM);
        cfg->eyes_use_fg = false;
        cfg->eye_frame_fill = solid(INK);
        cfg->eye_pupil_fill = solid(INK_COPPER);
        cfg->bg = solid(PAPER);
        cfg->logo = &TORI;
        ex->file = "06-star.png";
        ex->title = "Star";
        ex->note = "The lowest-coverage style, and the one that needs the most "
                   "pixels per module before it stops being a smudge.";
        break;

    case 6:
        QrStyle_ConfigPreset(cfg, QRSTYLE_PRESET_LIQUID);
        cfg->fg = grad(CANDLE, QRSTYLE_HEX(0xC9A15E), 45);
        cfg->eyes_use_fg = false;
        cfg->eye_frame_fill = solid(CANDLE);
        cfg->eye_pupil_fill = solid(QRSTYLE_HEX(0xE0B05A));
        cfg->bg = solid(NIGHT);
        cfg->logo = &TORI;
        ex->file = "07-night.png";
        ex->title = "Night";
        ex->note = "Light on dark. Inverted codes are legal and every current "
                   "phone reads them; a decade-old scanner may not.";
        break;

    case 7:
        QrStyle_ConfigPreset(cfg, QRSTYLE_PRESET_DOTS);
        cfg->module_size_pct = 92;
        cfg->fg = solid(CANDLE);
        cfg->eyes_use_fg = false;
        cfg->eye_frame_fill = solid(MINT);
        cfg->eye_pupil_fill = solid(CANDLE);
        cfg->bg = solid(NIGHT);
        cfg->logo = &TORI;
        ex->panel = true;
        ex->file = "08-panel.png";
        ex->title = "On the panel (240x240, two strips)";
        ex->note = "What a 240x240 SPI panel is actually handed: RGB565 big-endian, rendered "
                   "as two 240x120 strips, which is how a part without room for a whole frame draws one.";
        break;

    /*
     * KANSAS CITY CURRENT, and the one thing their palette cannot do.
     *
     * Teal, navy, red, white. Three of those four pairings work; the fourth is
     * the interesting one. KC red on the teal ground, or on the navy ground
     * beside teal data, lands on the WRONG SIDE of the single threshold a
     * decoder picks -- its grey is 106, and the midpoint between navy ink and a
     * teal ground is 103. A red eye there is a red eye that binarises to
     * background, which is the failure QrStyle_Check refuses.
     *
     * So the red used as an accent against those grounds is KC_RED_DEEP, the
     * same hue carried down until it clears the threshold with margin. Against
     * white, where the midpoint sits at 175, the bright red is ink and needs no
     * help. That is not a limitation of the club's palette -- it is what any
     * three-colour scheme runs into once one of the colours has to survive
     * being thresholded rather than merely being seen.
     */
    case 8:
        QrStyle_ConfigPreset(cfg, QRSTYLE_PRESET_LIQUID);
        cfg->fg = solid(KC_NAVY);
        cfg->eyes_use_fg = false;
        cfg->eye_frame_fill = solid(KC_NAVY);
        cfg->eye_pupil_fill = solid(KC_RED_DEEP);
        cfg->bg = solid(KC_TEAL);
        cfg->logo = &TORI;
        ex->file = "09-kc-teal.png";
        ex->title = "KC / Teal";
        ex->note = "The club's own pairing: navy on teal, fused, with a deep red "
                   "pupil in each eye.";
        break;

    case 9:
        QrStyle_ConfigPreset(cfg, QRSTYLE_PRESET_TEARDROP);
        cfg->module_shape = QRSTYLE_MODULE_ROUNDED;
        cfg->module_size_pct = 92;
        cfg->fg = grad(KC_NAVY, KC_TEAL_DEEP, 55);
        cfg->eyes_use_fg = false;
        cfg->eye_frame_fill = solid(KC_NAVY);
        cfg->eye_pupil_fill = solid(KC_RED);
        cfg->bg = solid(KC_WHITE);
        cfg->logo = &TORI;
        ex->file = "10-kc-crest.png";
        ex->title = "KC / Crest";
        ex->note = "Navy running into deep teal across the symbol, the way the "
                   "crest's swoosh does, under teardrop eyes with red pupils.";
        break;

    case 10:
        QrStyle_ConfigPreset(cfg, QRSTYLE_PRESET_DOTS);
        cfg->fg = solid(KC_RED);
        cfg->eyes_use_fg = false;
        cfg->eye_frame_fill = solid(KC_NAVY);
        cfg->eye_pupil_fill = solid(KC_RED);
        cfg->bg = solid(KC_WHITE);
        cfg->logo = &TORI;
        ex->file = "11-kc-red.png";
        ex->title = "KC / Match Red";
        ex->note = "Red is the ink here, so it is the bright one -- against white "
                   "the threshold sits well above it. Navy rings hold the eyes.";
        break;

    case 11:
        QrStyle_ConfigPreset(cfg, QRSTYLE_PRESET_LIQUID);
        cfg->fg = solid(KC_TEAL);
        cfg->eyes_use_fg = false;
        cfg->eye_frame_fill = solid(KC_WHITE);
        cfg->eye_pupil_fill = solid(KC_TEAL);
        cfg->bg = solid(KC_NAVY);
        cfg->logo = &TORI;
        ex->file = "12-kc-midnight.png";
        ex->title = "KC / Midnight";
        ex->note = "Teal on navy, white eyes. Red cannot be the accent on this "
                   "one -- it binarises below the threshold and the ring "
                   "disappears, so white takes the job.";
        break;

    case 12:
        QrStyle_ConfigPreset(cfg, QRSTYLE_PRESET_DOTS);
        cfg->module_size_pct = 92;
        cfg->fg = solid(KC_NAVY);
        cfg->eyes_use_fg = false;
        cfg->eye_frame_fill = solid(KC_NAVY);
        cfg->eye_pupil_fill = solid(KC_RED_DEEP);
        cfg->bg = solid(KC_TEAL);
        cfg->logo = &TORI;
        ex->panel = true;
        ex->file = "13-kc-panel.png";
        ex->title = "KC / On the panel";
        ex->note = "The teal scheme at the size a small panel actually gets: "
                   "240x240, five pixels per module, RGB565 big-endian.";
        break;

    default:
        return 0;
    }
    return 1;
}

/*---- Driving it ----*/

static void
rgb565_to_rgb888(const uint16_t *src, uint8_t *dst, int n, bool big_endian)
{
    int i;

    for( i = 0; i < n; i++ )
    {
        uint16_t v = src[i];
        unsigned r, g, b;

        if( big_endian )
            v = (uint16_t)((v >> 8) | (v << 8));
        r = (v >> 11) & 0x1F;
        g = (v >> 5) & 0x3F;
        b = v & 0x1F;
        dst[i * 3 + 0] = (uint8_t)((r << 3) | (r >> 2));
        dst[i * 3 + 1] = (uint8_t)((g << 2) | (g >> 4));
        dst[i * 3 + 2] = (uint8_t)((b << 3) | (b >> 2));
    }
}

int
main(int argc, char **argv)
{
    const char *outdir = (argc > 1) ? argv[1] : ".";
    int ai;
    const char *text = (argc > 2) ? argv[2]
                                  : "https://github.com/MRobertEvers/styled-qr-c";
    uint8_t qr[qrcodegen_BUFFER_LEN_FOR_VERSION(MAX_VERSION)];
    uint8_t tmp[qrcodegen_BUFFER_LEN_FOR_VERSION(MAX_VERSION)];
    static uint16_t fb[GALLERY_PX * GALLERY_PX];
    static uint8_t rgb[GALLERY_PX * GALLERY_PX * 3];
    int index, failures = 0;

    for( ai = 1; ai < argc; ai++ )
        if( strcmp(argv[ai], "-v") == 0 )
            g_verbose = true;

    printf("payload: %s\n\n", text);
    printf("%-34s %4s %4s %5s %8s %9s %7s %4s %5s\n",
           "example", "ver", "ecc", "px/m", "contrast", "logo/bud", "finders",
           "bad", "eye");
    printf("%-34s %4s %4s %5s %8s %9s %7s %4s %5s\n",
           "----------------------------------", "---", "---", "----", "--------",
           "---------", "-------", "----", "-----");

    for( index = 0;; index++ )
    {
        struct QrStyle_Config cfg;
        struct Example ex;
        struct QrStyle_Report rep;
        struct QrStyle_Target tg;
        struct QrStyle_Box box;
        struct Scan scan;
        enum qrcodegen_Ecc ecl;
        char path[512];
        int px, size, ppm, ok;
        static const char *ECC_NAME[4] = { "L", "M", "Q", "H" };

        if( !build_config(index, &cfg, &ex) )
            break;

        if( !QrStyle_EncodeText(text, &cfg, tmp, qr, MAX_VERSION, &ecl) )
        {
            printf("%-34s ENCODE FAILED\n", ex.title);
            failures++;
            continue;
        }
        size = qrcodegen_getSize(qr);

        (void)QrStyle_Check(qr, &cfg, &rep);

        px = ex.panel ? PANEL_PX : GALLERY_PX;
        box.x = 0;
        box.y = 0;
        box.size = px;
        ppm = QrStyle_PixelsPerModule(qr, &cfg, px);

        memset(fb, 0, sizeof(uint16_t) * (size_t)px * px);

        if( ex.panel )
        {
            /*
             * The strip path, exercised rather than described: two targets that
             * are windows onto the same logical frame. If origin_y were being
             * ignored the seam would land in the middle of the code and the
             * module check below would find it.
             */
            int s;

            for( s = 0; s < PANEL_PX / STRIP_ROWS; s++ )
            {
                tg.pixels = fb + (size_t)s * STRIP_ROWS * PANEL_PX;
                tg.width = PANEL_PX;
                tg.height = STRIP_ROWS;
                tg.stride = PANEL_PX;
                tg.origin_x = 0;
                tg.origin_y = s * STRIP_ROWS;
                tg.big_endian = true;
                QrStyle_Render(qr, &cfg, &tg, &box);
            }
        }
        else
        {
            tg.pixels = fb;
            tg.width = px;
            tg.height = px;
            tg.stride = px;
            tg.origin_x = 0;
            tg.origin_y = 0;
            tg.big_endian = false;
            QrStyle_Render(qr, &cfg, &tg, &box);
        }

        rgb565_to_rgb888(fb, rgb, px * px, ex.panel);

        scan_rendered(rgb, px, px, qr, &cfg, 0, 0, px, &scan);

        snprintf(path, sizeof(path), "%s/%s", outdir, ex.file);
        if( !png_write_rgb(path, rgb, px, px) )
        {
            printf("could not write %s\n", path);
            failures++;
        }

        ok = (scan.modules_wrong == 0) && (scan.finders_ok == 6) && rep.ok;
        if( !ok )
            failures++;

        printf(
            "%-34s %4d %4s %5d %6d.%d %4d/%-4d %5d/6 %4d %5d %s\n",
            ex.title,
            (size - 17) / 4,
            ECC_NAME[(int)ecl],
            ppm,
            rep.contrast_x10 / 10,
            rep.contrast_x10 % 10,
            rep.logo_area_pct,
            rep.logo_budget_pct,
            scan.finders_ok,
            scan.modules_wrong,
            scan.modules_restyled,
            ok ? "ok" : "FAIL");
        if( rep.problem )
            printf("    ! %s\n", rep.problem);
        if( scan.modules_wrong )
            printf("    ! %d module centres read back as the wrong colour\n",
                   scan.modules_wrong);
        if( scan.finders_ok != 6 )
            printf("    ! finder cross failed; worst run deviation %.2f modules\n",
                   scan.worst_finder_err);
    }

    printf("\n%d example(s) failed\n", failures);
    return failures ? 1 : 0;
}
