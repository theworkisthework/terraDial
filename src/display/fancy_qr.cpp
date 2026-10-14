#include "fancy_qr.h"
#include <Arduino.h>
#include "ui_scale.h"
#include "palette.h"
#include "icon_logo.h"
#include <esp_heap_caps.h>
#include <math.h>
#include <string.h>
#include "extra/libs/qrcode/qrcodegen.h"

namespace
{
    // Version 10 is 57 modules square and holds 174 bytes even at the
    // highest error correction -- far past any link this panel shows.
    const int MAX_VERSION = 10;
    const int QR_BUF = qrcodegen_BUFFER_LEN_FOR_VERSION(MAX_VERSION);

    // The spec asks for 4 modules of light margin; 2 reads fine on a light
    // card and buys a bigger code in the same space.
    const int QUIET = 2;

    // Light ~240 luminance, darkest ink ~50: a wide gap, which is what a
    // phone's binariser cares about. The pupil is a deepened accent (~80):
    // the stock accent alone (~120) is too close to mid-grey to trust.
    // The terraPen skin swaps the navy inks for its shell greens and the
    // pupil for a deepened coral, keeping the same light/dark gap.
    lv_color_t paper() { return Palette::pick(0xf5f0e8, 0xf6f8ec); }
    lv_color_t pupil() { return Palette::pick(0xa3283f, 0xa8244c); }
    lv_color_t inkAt(float t) // t: 0 at the top-left corner, 1 at the bottom-right
    {
        const lv_color_t a = Palette::pick(0x1a1a2e, 0x17321b), b = Palette::pick(0x0f3460, 0x24492a),
                         c = Palette::pick(0x3d1150, 0x0e2011);
        if (t < 0.5f) return lv_color_mix(b, a, (uint8_t)(t * 2 * 255));
        return lv_color_mix(c, b, (uint8_t)((t - 0.5f) * 2 * 255));
    }

    // Fullscreen timing: the modules grow in, then the logo draws.
    const uint32_t WAVE_MS = 900;
    const uint32_t PEN_MS = 1400;

    inline float clamp01(float v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }
    inline float smooth(float t) { t = clamp01(t); return t * t * (3 - 2 * t); }
    // Anti-aliased coverage from a signed distance in pixels.
    inline float cover(float dPx) { return clamp01(0.5f - dPx); }
    inline lv_color_t blend(lv_color_t over, lv_color_t under, float a) { return lv_color_mix(over, under, (uint8_t)(a * 255)); }

    // Signed distance to a rounded rectangle centred on the origin.
    float rrect(float x, float y, float hx, float hy, float r)
    {
        float qx = fabsf(x) - hx + r, qy = fabsf(y) - hy + r;
        float ox = fmaxf(qx, 0), oy = fmaxf(qy, 0);
        return sqrtf(ox * ox + oy * oy) + fminf(fmaxf(qx, qy), 0) - r;
    }

    struct Qr
    {
        lv_obj_t *canvas = nullptr;
        lv_color_t *buf = nullptr;
        int side = 0;
        bool fancy = false;
        uint8_t code[QR_BUF];
        int n = 0;             // modules per side
        float m = 1;           // pixels per module
        float ox = 0, oy = 0;  // the code's top-left, px
        float half = 0, radius = 0, rim = 0;
        lv_color_t bg;
        // The logo, fancy only: a clear disc of clearR modules round the
        // centre, and the mark itself resampled to logoD px square.
        float clearR = 0;
        int logoD = 0;
        uint8_t *logoA = nullptr;
        uint8_t *logoArr = nullptr;
    };

    bool dark(const Qr &q, int i, int j) { return qrcodegen_getModule(q.code, i, j); } // false off the edge

    bool inFinder(const Qr &q, int i, int j)
    {
        return (i < 7 && j < 7) || (i >= q.n - 7 && j < 7) || (i < 7 && j >= q.n - 7);
    }

    bool cleared(const Qr &q, int i, int j)
    {
        if (!q.logoD) return false;
        float dx = i + 0.5f - q.n * 0.5f, dy = j + 0.5f - q.n * 0.5f;
        return dx * dx + dy * dy < q.clearR * q.clearR;
    }

    // How far grown module (i, j) is, 0..1, as a wave rolls out from the
    // centre: wave 0 is nothing, wave 1 is everything at full size.
    float grow(const Qr &q, int i, int j, float wave)
    {
        if (wave >= 1) return 1;
        float dx = i + 0.5f - q.n * 0.5f, dy = j + 0.5f - q.n * 0.5f;
        float dist = sqrtf(dx * dx + dy * dy) / (q.n * 0.7072f);
        return smooth((wave * 1.25f - dist) / 0.25f);
    }

    // Shrinks the mark from its native bitmap into a logoD-px square. Each
    // target pixel takes the STRONGEST source pixel under it, not the
    // average: averaging thins a pen stroke to a ghost at this scale.
    void buildLogo(Qr &q)
    {
        const int sw = iconLogo.header.w, sh = iconLogo.header.h, d = q.logoD;
        q.logoA = (uint8_t *)heap_caps_malloc(d * d, MALLOC_CAP_SPIRAM);
        q.logoArr = (uint8_t *)heap_caps_malloc(d * d, MALLOC_CAP_SPIRAM);
        if (!q.logoA || !q.logoArr)
        {
            q.logoD = 0;
            return;
        }
        for (int v = 0; v < d; v++)
            for (int u = 0; u < d; u++)
            {
                int x0 = u * sw / d, x1 = (u + 1) * sw / d, y0 = v * sh / d, y1 = (v + 1) * sh / d;
                uint8_t a = 0, arr = ICON_LOGO_NEVER;
                for (int y = y0; y < y1; y++)
                    for (int x = x0; x < x1; x++)
                    {
                        uint8_t s = iconLogo.data[y * sw + x];
                        if (s > a) a = s;
                        uint8_t r = iconLogoArrival[y * sw + x];
                        if (s && r < arr) arr = r;
                    }
                q.logoA[v * d + u] = a;
                q.logoArr[v * d + u] = arr;
            }
    }

    // One module's ink coverage in the fancy style: a dot, joined to each
    // dark neighbour by a bar as wide as the dot, so runs of modules read
    // as strokes. A phone samples module centres, which stay solid.
    float liquid(const Qr &q, int i, int j, float lx, float ly, float wave)
    {
        if (!dark(q, i, j)) return 0;
        float g = grow(q, i, j, wave);
        if (g <= 0) return 0;
        const float R = 0.42f;
        float d = sqrtf(lx * lx + ly * ly) - R * g;
        auto joins = [&](int ni, int nj) {
            return dark(q, ni, nj) && !inFinder(q, ni, nj) && !cleared(q, ni, nj);
        };
        if (lx > 0 && joins(i + 1, j)) d = fminf(d, fabsf(ly) - R * fminf(g, grow(q, i + 1, j, wave)));
        if (lx < 0 && joins(i - 1, j)) d = fminf(d, fabsf(ly) - R * fminf(g, grow(q, i - 1, j, wave)));
        if (ly > 0 && joins(i, j + 1)) d = fminf(d, fabsf(lx) - R * fminf(g, grow(q, i, j + 1, wave)));
        if (ly < 0 && joins(i, j - 1)) d = fminf(d, fabsf(lx) - R * fminf(g, grow(q, i, j - 1, wave)));
        return cover(d * q.m);
    }

    // The whole picture, every pixel. `wave` and `pen` run 0..1; the small
    // code is drawn once at 1, 1.
    void render(Qr &q, float wave, float pen)
    {
        const float c = q.side * 0.5f;
        const uint8_t penAt = (uint8_t)(clamp01(pen) * ICON_LOGO_ARRIVAL_MAX);
        const int logoX = (int)(c - q.logoD * 0.5f), logoY = logoX;
        for (int y = 0; y < q.side; y++)
        {
            for (int x = 0; x < q.side; x++)
            {
                float px = x + 0.5f, py = y + 0.5f;
                float dCard = rrect(px - c, py - c, q.half, q.half, q.radius);
                lv_color_t &out = q.buf[y * q.side + x];
                if (dCard > 1)
                {
                    out = q.bg;
                    continue;
                }

                lv_color_t col = paper();
                float fx = (px - q.ox) / q.m, fy = (py - q.oy) / q.m;
                if (fx >= 0 && fy >= 0 && fx < q.n && fy < q.n)
                {
                    int i = (int)fx, j = (int)fy;
                    lv_color_t ink = inkAt((fx + fy) / (2.0f * q.n));
                    if (inFinder(q, i, j))
                    {
                        // A rounded ring with a red pupil, scaled in place as
                        // its corner of the wave arrives.
                        int fi = i < 7 ? 0 : q.n - 7, fj = j < 7 ? 0 : q.n - 7;
                        float g = q.fancy ? grow(q, fi + 3, fj + 3, wave) : 1;
                        if (g > 0)
                        {
                            float sx = (fx - fi - 3.5f) / g, sy = (fy - fj - 3.5f) / g, s = q.m * g;
                            float ring = cover(rrect(sx, sy, 3.5f, 3.5f, 1.6f) * s) *
                                         (1 - cover(rrect(sx, sy, 2.5f, 2.5f, 1.1f) * s));
                            col = blend(ink, col, ring);
                            col = blend(pupil(), col, cover(rrect(sx, sy, 1.5f, 1.5f, 0.8f) * s));
                        }
                    }
                    else if (!cleared(q, i, j))
                    {
                        // Small: plain squares, sampled at the pixel centre --
                        // at two or three pixels a module, anything rounder
                        // just smudges.
                        float a = q.fancy ? liquid(q, i, j, fx - i - 0.5f, fy - j - 0.5f, wave)
                                          : (dark(q, i, j) ? 1.0f : 0.0f);
                        if (a > 0) col = blend(ink, col, a);
                    }
                }

                if (q.logoD && pen > 0)
                {
                    int u = x - logoX, v = y - logoY;
                    if (u >= 0 && v >= 0 && u < q.logoD && v < q.logoD)
                    {
                        int k = v * q.logoD + u;
                        if (q.logoArr[k] != ICON_LOGO_NEVER && q.logoArr[k] <= penAt)
                            col = blend(Palette::accent(), col, q.logoA[k] / 255.0f);
                    }
                }

                // The accent rim, then the card's own anti-aliased edge.
                float dRim = fabsf(dCard + q.rim * 0.5f) - q.rim * 0.5f;
                col = blend(Palette::accent(), col, cover(dRim));
                out = blend(col, q.bg, cover(dCard));
            }
        }
        lv_obj_invalidate(q.canvas);
    }

    // Encodes `url` and lays the code out in a `side` px card. Fancy asks
    // for the highest error correction (to pay for the logo), dropping back
    // to medium -- and no logo -- for a link too long for that.
    Qr *build(lv_obj_t *parent, const char *url, lv_coord_t side, bool fancy, lv_color_t bg)
    {
        Qr *q = new Qr();
        uint8_t tmp[QR_BUF];
        bool ok = false, high = false;
        if (fancy)
            ok = high = qrcodegen_encodeText(url, tmp, q->code, qrcodegen_Ecc_HIGH, 1, MAX_VERSION, qrcodegen_Mask_AUTO, true);
        if (!ok)
            // Smallest version that fits, at whatever correction that leaves room for.
            ok = qrcodegen_encodeText(url, tmp, q->code, qrcodegen_Ecc_MEDIUM, 1, MAX_VERSION, qrcodegen_Mask_AUTO, true);
        q->buf = ok ? (lv_color_t *)heap_caps_malloc(sizeof(lv_color_t) * side * side, MALLOC_CAP_SPIRAM) : nullptr;
        if (!q->buf)
        {
            delete q;
            return nullptr;
        }

        q->side = side;
        q->fancy = fancy;
        q->bg = bg;
        q->n = qrcodegen_getSize(q->code);
        q->m = (float)side / (q->n + 2 * QUIET);
        q->ox = q->oy = (side - q->n * q->m) * 0.5f;
        q->half = side * 0.5f - 0.5f;
        q->radius = side * 0.08f;
        q->rim = fmaxf(1.0f, side / 90.0f);
        if (high)
        {
            // About 6% of the modules: high correction can rebuild ~30%.
            q->clearR = q->n * 0.13f;
            q->logoD = (int)(q->clearR * 2 * q->m * 0.82f);
            buildLogo(*q);
        }

        q->canvas = lv_canvas_create(parent);
        lv_canvas_set_buffer(q->canvas, q->buf, side, side, LV_IMG_CF_TRUE_COLOR);
        return q;
    }

    void freeQr(Qr *q)
    {
        if (!q) return;
        heap_caps_free(q->buf);
        heap_caps_free(q->logoA);
        heap_caps_free(q->logoArr);
        delete q;
    }

    // ---- the small code ----

    struct Link
    {
        const char *url;
        const char *caption;
        Qr *qr;
    };

    void smallCb(lv_event_t *e)
    {
        Link *link = (Link *)lv_event_get_user_data(e);
        if (lv_event_get_code(e) == LV_EVENT_CLICKED) FancyQr::showFullscreen(link->url, link->caption);
        else if (lv_event_get_code(e) == LV_EVENT_DELETE)
        {
            freeQr(link->qr);
            delete link;
        }
    }

    // ---- the big one ----

    lv_obj_t *overlay = nullptr;
    Qr *big = nullptr;
    lv_timer_t *animTimer = nullptr;
    uint32_t animStart = 0;

    void animCb(lv_timer_t *)
    {
        uint32_t t = millis() - animStart;
        float wave = (float)t / WAVE_MS;
        float pen = (float)(t > WAVE_MS ? t - WAVE_MS : 0) / PEN_MS;
        render(*big, wave, pen);
        if (wave >= 1 && pen >= 1)
        {
            lv_timer_del(animTimer);
            animTimer = nullptr;
        }
    }

    void overlayCb(lv_event_t *e)
    {
        if (lv_event_get_code(e) == LV_EVENT_CLICKED) FancyQr::close();
        else if (lv_event_get_code(e) == LV_EVENT_DELETE)
        {
            if (animTimer) lv_timer_del(animTimer);
            animTimer = nullptr;
            freeQr(big);
            big = nullptr;
            overlay = nullptr;
        }
    }
}

namespace FancyQr
{
    lv_obj_t *create(lv_obj_t *parent, const char *url, const char *caption, lv_coord_t side)
    {
        Qr *q = build(parent, url, side, false, Palette::bgApp());
        if (!q) return nullptr;
        render(*q, 1, 1);
        lv_obj_add_flag(q->canvas, LV_OBJ_FLAG_CLICKABLE);
        Link *link = new Link{url, caption, q};
        lv_obj_add_event_cb(q->canvas, smallCb, LV_EVENT_ALL, link);
        return q->canvas;
    }

    void showFullscreen(const char *url, const char *caption)
    {
        close();
        overlay = lv_obj_create(lv_layer_top());
        lv_obj_remove_style_all(overlay);
        lv_obj_set_size(overlay, LV_PCT(100), LV_PCT(100));
        lv_obj_set_style_bg_color(overlay, Palette::bgApp(), 0);
        lv_obj_set_style_bg_opa(overlay, LV_OPA_COVER, 0);
        lv_obj_add_flag(overlay, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(overlay, overlayCb, LV_EVENT_ALL, nullptr);

        // 150 on the 240 grid, centred a touch high so the caption fits
        // below: the card's top corners then sit ~112px from the centre of
        // a 120px-radius face, inside the glass.
        big = build(overlay, url, px(150), true, Palette::bgApp());
        if (!big)
        {
            lv_obj_del(overlay);
            return;
        }
        lv_obj_align(big->canvas, LV_ALIGN_CENTER, 0, px(-8));
        render(*big, 0, 0);

        lv_obj_t *lbl = lv_label_create(overlay);
        lv_label_set_text(lbl, caption);
        lv_obj_set_style_text_font(lbl, &UI_FONT_12, 0);
        lv_obj_set_style_text_color(lbl, Palette::textMuted(), 0);
        lv_obj_align(lbl, LV_ALIGN_CENTER, 0, px(83));

        animStart = millis();
        animTimer = lv_timer_create(animCb, 20, nullptr);
    }

    bool isOpen() { return overlay != nullptr; }

    void close()
    {
        if (overlay) lv_obj_del(overlay); // LV_EVENT_DELETE tidies the rest
    }
}
