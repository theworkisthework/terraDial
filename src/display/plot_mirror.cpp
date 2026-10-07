#include "plot_mirror.h"
#include "ui_scale.h"
#include "palette.h"
#include "../config/settings.h"
#include <esp_heap_caps.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

namespace
{
    // The recorded path: points in 0.1mm, with BREAK marking a pen lift
    // between two pen-down runs. Kept so the view can be redrawn whole when
    // it zooms out. 50k points is ~80 minutes of continuous drawing at ten
    // reports a second; past that the canvas carries on drawing but can't
    // be refitted any more.
    struct Pt
    {
        int16_t x, y;
    };
    const int16_t BREAK = INT16_MIN;
    const int CAPACITY = 50000;

    // Never zoom in closer than this many mm across: a drawing's first
    // stroke shouldn't fill the whole circle and then jump away.
    const float MIN_SPAN_MM = 30.0f;
    // Room left round the drawing on each fit, so it doesn't refit (a full
    // redraw) for every new point near the edge.
    const float HEADROOM = 1.3f;

    lv_obj_t *box = nullptr;
    lv_obj_t *canvas = nullptr;
    lv_obj_t *head = nullptr;
    lv_color_t *canvasBuf = nullptr;
    lv_coord_t diam = 0;
    lv_coord_t inset = 0; // drawing stays this far inside the circle

    Pt *pts = nullptr;
    int count = 0;
    int drawnUpTo = 0;
    bool needsRefit = false;

    // Bounds of everything drawn, in mm.
    float minX, maxX, minY, maxY;
    bool haveBounds = false;

    // The view: drawing-space mm -> canvas px.
    float viewCx = 0, viewCy = 0, viewScale = 1;

    // Job and head tracking.
    bool wasActive = false;
    char trackedFile[sizeof(FluidNCStatus::jobFilename)] = {0};
    float lastX = NAN, lastY = NAN, lastZ = NAN;
    bool lastDown = false;
    bool headMoved = false;

    // Pen-down if Z is on the pen-down side of halfway between the two
    // heights the user's Pen commands move to (Settings > Machine).
    float penThreshold = 2.5f;
    bool downIsLower = true;

    bool parseZ(const char *cmd, float &out)
    {
        for (const char *p = cmd; *p; p++)
        {
            if (*p == 'Z' || *p == 'z')
            {
                char *end;
                float v = strtof(p + 1, &end);
                if (end == p + 1) return false;
                out = v;
                return true;
            }
        }
        return false;
    }

    void readPenHeights()
    {
        float up = 5.0f, down = 0.0f; // PEN_UP_CMD / PEN_DOWN_CMD defaults
        parseZ(Config::get().penUpCmd, up);
        parseZ(Config::get().penDownCmd, down);
        if (up == down) up = down + 1.0f; // nothing sensible to split
        penThreshold = (up + down) / 2.0f;
        downIsLower = down < up;
    }

    bool isPenDown(float z) { return downIsLower ? z < penThreshold : z > penThreshold; }

    lv_point_t toCanvas(const Pt &p)
    {
        lv_point_t c;
        c.x = (lv_coord_t)lroundf(diam / 2.0f + (p.x / 10.0f - viewCx) * viewScale);
        // Machine Y runs away from you, which is up the screen.
        c.y = (lv_coord_t)lroundf(diam / 2.0f - (p.y / 10.0f - viewCy) * viewScale);
        return c;
    }

    bool insideView(float x, float y)
    {
        float dx = (x - viewCx) * viewScale, dy = (y - viewCy) * viewScale;
        float r = diam / 2.0f - inset;
        return dx * dx + dy * dy <= r * r;
    }

    void fitView()
    {
        float w = maxX - minX, h = maxY - minY;
        // Fit the bounding box's DIAGONAL to the circle: then the whole box,
        // corners and all, is inside the round view.
        float span = sqrtf(w * w + h * h) * HEADROOM;
        if (span < MIN_SPAN_MM) span = MIN_SPAN_MM;
        viewScale = (diam - 2.0f * inset) / span;
        viewCx = (minX + maxX) / 2.0f;
        viewCy = (minY + maxY) / 2.0f;
    }

    lv_draw_line_dsc_t lineStyle()
    {
        lv_draw_line_dsc_t d;
        lv_draw_line_dsc_init(&d);
        d.color = Palette::text();
        d.width = px(1);
        d.round_start = 1;
        d.round_end = 1;
        return d;
    }

    // Draws recorded points [from, to) as polylines, split at pen lifts.
    void drawRange(int from, int to)
    {
        static lv_point_t run[96];
        lv_draw_line_dsc_t style = lineStyle();
        int n = 0;
        for (int i = from; i < to; i++)
        {
            if (pts[i].x == BREAK)
            {
                if (n >= 2) lv_canvas_draw_line(canvas, run, n, &style);
                n = 0;
                continue;
            }
            run[n++] = toCanvas(pts[i]);
            if (n == (int)(sizeof(run) / sizeof(run[0])))
            {
                lv_canvas_draw_line(canvas, run, n, &style);
                run[0] = run[n - 1]; // carry on from the last point
                n = 1;
            }
        }
        if (n >= 2) lv_canvas_draw_line(canvas, run, n, &style);
    }

    void clearCanvas() { lv_canvas_fill_bg(canvas, Palette::bgApp(), LV_OPA_COVER); }

    void startDrawing(const FluidNCStatus &st)
    {
        count = 0;
        drawnUpTo = 0;
        haveBounds = false;
        needsRefit = false;
        lastDown = false;
        snprintf(trackedFile, sizeof(trackedFile), "%s", st.jobFilename);
        readPenHeights(); // the commands could have changed since last job
        if (canvas) clearCanvas();
    }

    void push(int16_t x, int16_t y)
    {
        if (count >= CAPACITY) return;
        pts[count++] = {x, y};
    }
}

namespace PlotMirror
{
    lv_obj_t *create(lv_obj_t *parent, lv_coord_t diameter)
    {
        diam = diameter;
        inset = px(6);

        box = lv_obj_create(parent);
        lv_obj_remove_style_all(box);
        lv_obj_set_size(box, diam, diam);
        lv_obj_clear_flag(box, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);

        // Both big buffers in PSRAM: 2*diam^2 bytes for the canvas (~170KB
        // on the 360px panel) and 200KB of path.
        canvasBuf = (lv_color_t *)heap_caps_malloc(LV_CANVAS_BUF_SIZE_TRUE_COLOR(diam, diam), MALLOC_CAP_SPIRAM);
        pts = (Pt *)heap_caps_malloc(sizeof(Pt) * CAPACITY, MALLOC_CAP_SPIRAM);
        if (!canvasBuf || !pts)
        {
            // The rest of the Job Progress screen works without the mirror.
            free(canvasBuf);
            free(pts);
            canvasBuf = nullptr;
            pts = nullptr;
            return box;
        }

        // A square canvas in a round view: its corners fall outside the
        // progress ring, where they're the same colour as the screen.
        canvas = lv_canvas_create(box);
        lv_canvas_set_buffer(canvas, canvasBuf, diam, diam, LV_IMG_CF_TRUE_COLOR);
        lv_obj_clear_flag(canvas, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_center(canvas);
        clearCanvas();

        // The pen: filled while it's on the paper, a ring while it's up.
        head = lv_obj_create(box);
        lv_obj_remove_style_all(head);
        lv_obj_set_size(head, px(7), px(7));
        lv_obj_set_style_radius(head, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_color(head, Palette::accent(), 0);
        lv_obj_set_style_border_width(head, px(1), 0);
        lv_obj_set_style_bg_color(head, Palette::accent(), 0);
        lv_obj_clear_flag(head, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(head, LV_OBJ_FLAG_HIDDEN);

        readPenHeights();
        return box;
    }

    void sample(const FluidNCStatus &st)
    {
        if (!pts || !st.havePos) return;

        // A new job: the job flag rising, or the file changing under a
        // still-raised flag (back-to-back runs).
        if (st.jobActive && (!wasActive || strncmp(trackedFile, st.jobFilename, sizeof(trackedFile)) != 0))
            startDrawing(st);
        wasActive = st.jobActive;

        float x = st.wposX, y = st.wposY, z = st.wposZ;
        if (x == lastX && y == lastY && z == lastZ) return; // no new report
        lastX = x;
        lastY = y;
        lastZ = z;
        headMoved = true;
        if (!st.jobActive) return; // the pen dot follows; nothing is drawn

        bool down = isPenDown(z);
        if (down)
        {
            if (!lastDown && count > 0 && pts[count - 1].x != BREAK) push(BREAK, 0);
            push((int16_t)lroundf(x * 10.0f), (int16_t)lroundf(y * 10.0f));

            if (!haveBounds)
            {
                minX = maxX = x;
                minY = maxY = y;
                haveBounds = true;
                needsRefit = true;
            }
            else
            {
                if (x < minX) minX = x;
                if (x > maxX) maxX = x;
                if (y < minY) minY = y;
                if (y > maxY) maxY = y;
            }
            if (!insideView(x, y) && count < CAPACITY) needsRefit = true;
        }
        lastDown = down;
    }

    void refresh()
    {
        if (!canvas) return;

        if (needsRefit)
        {
            needsRefit = false;
            fitView();
            clearCanvas();
            drawRange(0, count);
            drawnUpTo = count;
        }
        else if (count > drawnUpTo)
        {
            // Start one back so the new stretch joins on to what's drawn.
            drawRange(drawnUpTo > 0 ? drawnUpTo - 1 : 0, count);
            drawnUpTo = count;
        }

        if (headMoved && haveBounds && !isnan(lastX))
        {
            headMoved = false;
            lv_point_t c = toCanvas({(int16_t)lroundf(lastX * 10.0f), (int16_t)lroundf(lastY * 10.0f)});
            // Kept inside the view, so a pen-up move off across the bed
            // parks the dot at the edge rather than losing it.
            float dx = c.x - diam / 2.0f, dy = c.y - diam / 2.0f;
            float r = sqrtf(dx * dx + dy * dy), lim = diam / 2.0f - inset;
            if (r > lim)
            {
                c.x = (lv_coord_t)(diam / 2.0f + dx * lim / r);
                c.y = (lv_coord_t)(diam / 2.0f + dy * lim / r);
            }
            lv_coord_t s = lv_obj_get_width(head);
            lv_obj_set_pos(head, c.x - s / 2, c.y - s / 2);
            lv_obj_set_style_bg_opa(head, isPenDown(lastZ) ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
            lv_obj_clear_flag(head, LV_OBJ_FLAG_HIDDEN);
        }
    }

    bool hasDrawing() { return count > 0; }
}
