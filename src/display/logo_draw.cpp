#include "logo_draw.h"
#include "ui_scale.h"
#include "logo_stroke.h"
#include "palette.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

namespace
{
    // Roughly the bitmap logo's stroke weight (1.5px on the 240 grid),
    // rounded up: an anti-aliased 1px line reads thinner than it is.
    const lv_coord_t STROKE_W = px(2);
    const lv_coord_t PEN_SIZE = px(6);
    const uint32_t PEN_LIFT_MS = 450;

    struct State
    {
        lv_obj_t *line;
        lv_obj_t *pen;
        lv_point_t *full; // the whole stroke, in panel px
        lv_point_t *work; // what the line is showing: full, with the tip moved
        float *cum;       // stroke length up to each point
        int n;
        int tipAt;        // the work[] entry currently standing in for the tip
    };

    void apply(State *s, int32_t v)
    {
        const float total = s->cum[s->n - 1];
        const float at = total * v / 1000.0f;

        // Last point at or before `at`. Binary search: the pen's progress
        // is eased, so it doesn't move a predictable number of points a
        // frame.
        int lo = 0, hi = s->n - 1;
        while (lo < hi)
        {
            int mid = (lo + hi + 1) / 2;
            if (s->cum[mid] <= at) lo = mid;
            else hi = mid - 1;
        }

        // Put back the point the tip borrowed last frame.
        if (s->tipAt >= 0) s->work[s->tipAt] = s->full[s->tipAt];

        lv_point_t tip;
        int shown;
        if (lo >= s->n - 1)
        {
            tip = s->full[s->n - 1];
            shown = s->n;
            s->tipAt = -1;
        }
        else
        {
            // The pen is part-way along the next segment: the line ends
            // exactly under it, not at the last whole point.
            const lv_point_t &a = s->full[lo], &b = s->full[lo + 1];
            float seg = s->cum[lo + 1] - s->cum[lo];
            float f = seg > 0 ? (at - s->cum[lo]) / seg : 0;
            tip.x = (lv_coord_t)lroundf(a.x + (b.x - a.x) * f);
            tip.y = (lv_coord_t)lroundf(a.y + (b.y - a.y) * f);
            s->work[lo + 1] = tip;
            s->tipAt = lo + 1;
            shown = lo + 2;
        }

        lv_line_set_points(s->line, s->work, shown);
        lv_obj_set_pos(s->pen, tip.x - PEN_SIZE / 2, tip.y - PEN_SIZE / 2);
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
        free(s->full);
        free(s->work);
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
        lv_obj_set_size(box, px(LOGO_STROKE_BOX), px(LOGO_STROKE_BOX));
        lv_obj_clear_flag(box, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
        // The pen dot hangs half outside the box at the stroke's ends.
        lv_obj_add_flag(box, LV_OBJ_FLAG_OVERFLOW_VISIBLE);

        const int n = LOGO_STROKE_COUNT;
        State *s = (State *)calloc(1, sizeof(State));
        if (s)
        {
            s->full = (lv_point_t *)malloc(sizeof(lv_point_t) * n);
            s->work = (lv_point_t *)malloc(sizeof(lv_point_t) * n);
            s->cum = (float *)malloc(sizeof(float) * n);
        }
        if (!s || !s->full || !s->work || !s->cum || n < 2)
        {
            if (s) { free(s->full); free(s->work); free(s->cum); free(s); }
            return box; // nothing to draw with; an empty box rather than a crash
        }

        s->n = n;
        s->tipAt = -1;
        for (int i = 0; i < n; i++)
        {
            s->full[i].x = (lv_coord_t)lroundf(pxf((float)LOGO_STROKE[i].x / LOGO_STROKE_SUBPX));
            s->full[i].y = (lv_coord_t)lroundf(pxf((float)LOGO_STROKE[i].y / LOGO_STROKE_SUBPX));
            // Measured on the source points, so the pen's speed doesn't pick
            // up the panel's rounding.
            s->cum[i] = i == 0 ? 0
                               : s->cum[i - 1] + hypotf((float)(LOGO_STROKE[i].x - LOGO_STROKE[i - 1].x),
                                                        (float)(LOGO_STROKE[i].y - LOGO_STROKE[i - 1].y));
        }
        memcpy(s->work, s->full, sizeof(lv_point_t) * n);

        s->line = lv_line_create(box);
        lv_obj_set_style_line_width(s->line, STROKE_W, 0);
        lv_obj_set_style_line_color(s->line, stroke, 0);
        lv_obj_set_style_line_rounded(s->line, true, 0);
        lv_obj_set_pos(s->line, 0, 0);

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
