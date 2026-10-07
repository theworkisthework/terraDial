#include "logo_draw.h"
#include "ui_scale.h"
#include "icon_logo.h"
#include "logo_stroke.h"
#include "palette.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

// How it draws: not as a line. The first version traced the stroke with
// lv_line, and on the panel that looked lumpy and ran at a few frames a
// second -- its corners snapped to whole pixels, every one of 300-odd
// segments was drawn with its own round caps, overlapping at the joins,
// and all of them were redrawn every frame.
//
// Instead it reveals the logo bitmap itself, iconLogo, along the pen's
// path. gen_logo.py stores, for every pixel, how far along the stroke the
// pen is when it first inks it (iconLogoArrival); each frame shows the
// pixels the pen has reached, ramping their alpha in over a short distance
// so the leading edge is soft rather than stepped. The finished frame is
// exactly the static bitmap, and a frame is one image redraw.
namespace
{
    const lv_coord_t PEN_SIZE = px(5);
    const uint32_t PEN_LIFT_MS = 500;
    // How far behind the pen a pixel takes to ink in fully, in arrival
    // units (0..ICON_LOGO_ARRIVAL_MAX along the whole stroke). Small enough
    // to read as a pen tip, big enough that the edge isn't stepped.
    const float SOFT = 5.0f;

    struct State
    {
        lv_obj_t *img;
        lv_obj_t *pen;
        lv_img_dsc_t dsc; // iconLogo's header, pointing at `alpha`
        uint8_t *alpha;   // what's revealed so far
        float *cum;       // stroke length up to each LOGO_STROKE point
        float progress;   // in arrival units; lets a frame skip what's done
    };

    // Where the pen is when it's `at` arrival units along the stroke, in
    // the box's own px.
    void penPoint(const State *s, float at, lv_coord_t &x, lv_coord_t &y)
    {
        const int n = LOGO_STROKE_COUNT;
        float target = s->cum[n - 1] * at / ICON_LOGO_ARRIVAL_MAX;
        if (target >= s->cum[n - 1]) target = s->cum[n - 1];

        int lo = 0, hi = n - 1; // last point at or before target
        while (lo < hi)
        {
            int mid = (lo + hi + 1) / 2;
            if (s->cum[mid] <= target) lo = mid;
            else hi = mid - 1;
        }
        float fx = LOGO_STROKE[lo].x, fy = LOGO_STROKE[lo].y;
        if (lo < n - 1)
        {
            float seg = s->cum[lo + 1] - s->cum[lo];
            float f = seg > 0 ? (target - s->cum[lo]) / seg : 0;
            fx += (LOGO_STROKE[lo + 1].x - fx) * f;
            fy += (LOGO_STROKE[lo + 1].y - fy) * f;
        }
        x = (lv_coord_t)lroundf(pxf(fx / LOGO_STROKE_SUBPX));
        y = (lv_coord_t)lroundf(pxf(fy / LOGO_STROKE_SUBPX));
    }

    void apply(State *s, int32_t v)
    {
        // Runs a little past the end so the last pixels finish fading in.
        float p = (ICON_LOGO_ARRIVAL_MAX + SOFT) * v / 1000.0f;
        const uint8_t *src = iconLogo.data;
        const uint32_t count = (uint32_t)iconLogo.header.w * iconLogo.header.h;

        // Only pixels whose fade is still in progress can change: anything
        // reached more than SOFT ago is already fully inked.
        float doneBelow = s->progress - SOFT;
        for (uint32_t i = 0; i < count; i++)
        {
            uint8_t a = iconLogoArrival[i];
            if (a == ICON_LOGO_NEVER || a < doneBelow) continue;
            float f = (p - a) / SOFT;
            s->alpha[i] = f <= 0 ? 0 : f >= 1 ? src[i] : (uint8_t)(src[i] * f);
        }
        s->progress = p;
        lv_img_cache_invalidate_src(&s->dsc);
        lv_obj_invalidate(s->img);

        lv_coord_t x, y;
        penPoint(s, p > ICON_LOGO_ARRIVAL_MAX ? ICON_LOGO_ARRIVAL_MAX : p, x, y);
        lv_obj_set_pos(s->pen, x - PEN_SIZE / 2, y - PEN_SIZE / 2);
    }

    void animCb(void *obj, int32_t v)
    {
        State *s = (State *)lv_obj_get_user_data((lv_obj_t *)obj);
        if (s) apply(s, v);
    }

    void animDone(lv_anim_t *a)
    {
        // Mark finished: lift the pen.
        State *s = (State *)lv_obj_get_user_data((lv_obj_t *)a->var);
        if (s) lv_obj_fade_out(s->pen, PEN_LIFT_MS, 150);
    }

    void deleteCb(lv_event_t *e)
    {
        lv_obj_t *box = lv_event_get_target(e);
        State *s = (State *)lv_obj_get_user_data(box);
        if (!s) return;
        // LVGL already drops animations whose var is a deleted object.
        lv_img_cache_invalidate_src(&s->dsc);
        free(s->alpha);
        free(s->cum);
        free(s);
        lv_obj_set_user_data(box, nullptr);
    }
}

namespace LogoDraw
{
    lv_obj_t *create(lv_obj_t *parent, lv_color_t stroke, uint32_t drawMs)
    {
        lv_obj_t *box = lv_obj_create(parent);
        lv_obj_remove_style_all(box);
        lv_obj_set_size(box, iconLogo.header.w, iconLogo.header.h);
        lv_obj_clear_flag(box, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
        // The pen dot hangs half outside the box at the stroke's ends.
        lv_obj_add_flag(box, LV_OBJ_FLAG_OVERFLOW_VISIBLE);

        const uint32_t pixels = (uint32_t)iconLogo.header.w * iconLogo.header.h;
        State *s = (State *)calloc(1, sizeof(State));
        if (s)
        {
            s->alpha = (uint8_t *)calloc(pixels, 1);
            s->cum = (float *)malloc(sizeof(float) * LOGO_STROKE_COUNT);
        }

        lv_obj_t *img = lv_img_create(box);
        // Alpha-only source: without recolor_opa it draws as nothing. This
        // is also what "inverts" the mark -- the artwork is a black stroke
        // for print, painted light on the dark face here.
        lv_obj_set_style_img_recolor(img, stroke, 0);
        lv_obj_set_style_img_recolor_opa(img, LV_OPA_COVER, 0);

        if (!s || !s->alpha || !s->cum)
        {
            // Can't animate: show the finished mark rather than nothing.
            if (s) { free(s->alpha); free(s->cum); free(s); }
            lv_img_set_src(img, &iconLogo);
            return box;
        }

        for (int i = 0; i < LOGO_STROKE_COUNT; i++)
            s->cum[i] = i == 0 ? 0
                               : s->cum[i - 1] + hypotf((float)(LOGO_STROKE[i].x - LOGO_STROKE[i - 1].x),
                                                        (float)(LOGO_STROKE[i].y - LOGO_STROKE[i - 1].y));

        s->dsc = iconLogo;
        s->dsc.data = s->alpha;
        s->img = img;
        lv_img_set_src(img, &s->dsc);

        s->pen = lv_obj_create(box);
        lv_obj_remove_style_all(s->pen);
        lv_obj_set_size(s->pen, PEN_SIZE, PEN_SIZE);
        lv_obj_set_style_radius(s->pen, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(s->pen, Palette::accent(), 0);
        lv_obj_set_style_bg_opa(s->pen, LV_OPA_COVER, 0);
        lv_obj_clear_flag(s->pen, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_set_user_data(box, s);
        lv_obj_add_event_cb(box, deleteCb, LV_EVENT_DELETE, nullptr);
        apply(s, 0);

        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, box);
        lv_anim_set_exec_cb(&a, animCb);
        lv_anim_set_values(&a, 0, 1000);
        lv_anim_set_time(&a, drawMs);
        // A plotter pen accelerates away and decelerates to a stop.
        lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
        lv_anim_set_ready_cb(&a, animDone);
        lv_anim_start(&a);
        return box;
    }
}
