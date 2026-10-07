#include "ui_spiro.h"
#include "ui_scale.h"
#include "lucide_icons.h"
#include "palette.h"
#include "ui_screen_shell.h"
#include "ui_widgets.h"
#include "../config/settings.h"
#include "../net/fluidnc_client.h"
#include <esp_heap_caps.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

namespace
{
    // ---------------------------------------------------------------- curve

    struct Param
    {
        const char *name;
        int value, min, max;
    };
    enum { P_RING, P_GEAR, P_PEN, P_COUNT };
    Param params[P_COUNT] = {
        {"Ring", 96, 40, 150},
        {"Gear", 36, 8, 140},
        {"Pen", 70, 10, 150}, // % of the gear's radius: past 100 it loops
    };
    int selected = P_GEAR;

    // How finely the curve is cut up. Enough points per trip round the
    // ring to look smooth, but capped overall: a dense curve can need a
    // hundred trips, and every point becomes a line of G-code.
    const int MAX_POINTS = 3000;

    int gcd(int a, int b)
    {
        while (b) { int t = a % b; a = b; b = t; }
        return a;
    }

    struct Curve
    {
        int ring, gear;
        float pen;   // in teeth, like the radii
        int turns;   // trips round the ring before it closes
        int points;  // segments in the whole curve
        float reach; // furthest the pen gets from the centre, for scaling
    } curve;

    void buildCurve()
    {
        int ring = params[P_RING].value;
        int gear = params[P_GEAR].value;
        if (gear >= ring) gear = ring - 1;
        curve.ring = ring;
        curve.gear = gear;
        curve.pen = params[P_PEN].value * gear / 100.0f;
        curve.turns = gear / gcd(ring, gear);
        int perTurn = MAX_POINTS / curve.turns;
        if (perTurn > 240) perTurn = 240;
        if (perTurn < 12) perTurn = 12;
        curve.points = curve.turns * perTurn;
        curve.reach = (ring - gear) + curve.pen;
    }

    // Point i of the curve, in -1..1 on both axes (y up).
    void curvePoint(int i, float &x, float &y)
    {
        float t = 2.0f * (float)M_PI * curve.turns * i / curve.points;
        float rr = curve.ring - curve.gear;
        float k = rr / curve.gear;
        x = (rr * cosf(t) + curve.pen * cosf(k * t)) / curve.reach;
        y = (rr * sinf(t) - curve.pen * sinf(k * t)) / curve.reach;
    }

    // ---------------------------------------------------------------- plot
    //
    // Streams the curve to FluidNC as G-code. There's no file: the lines
    // are made one at a time as they go out. They go in batches -- a few
    // fire-and-forget lines, then one whose "ok" is waited for -- because
    // waiting on every line would leave the planner starving between the
    // short segments a curve is made of, and the pen would stutter.
    // FluidNC answers lines in order, so the batch's last "ok" means the
    // whole batch has been taken.

    const float FEED_MM_MIN = 2000.0f;
    const int BATCH = 6;
    const uint32_t ACK_TIMEOUT_MS = 60000; // a full planner can hold an ok back a while
    const int LEAD_LINES = 5; // G21, G90, pen up, travel to the start, pen down

    struct Plot
    {
        bool active = false;
        float cx = 0, cy = 0, halfSize = 50;
        int next = 0;   // next line to send
        int total = 0;  // lines in the whole plot
        uint32_t ticket = 0;
        uint32_t ticketAt = 0;
        uint32_t startedAt = 0;
        char result[48] = "";
        uint32_t resultAt = 0;
    } plot;

    void setResult(const char *msg)
    {
        snprintf(plot.result, sizeof(plot.result), "%s", msg);
        plot.resultAt = millis();
    }

    // Line i of the plot, or "" for one that has nothing to send (a pen
    // command left blank in Settings).
    void plotLine(int i, char *out, size_t n)
    {
        const AppSettings &cfg = Config::get();
        int curveEnd = LEAD_LINES + curve.points + 1;
        float x, y;
        if (i == 0) snprintf(out, n, "G21");
        else if (i == 1) snprintf(out, n, "G90");
        else if (i == 2) snprintf(out, n, "%s", cfg.penUpCmd);
        else if (i == 3)
        {
            curvePoint(0, x, y);
            snprintf(out, n, "G0 X%.3f Y%.3f", plot.cx + x * plot.halfSize, plot.cy + y * plot.halfSize);
        }
        else if (i == 4) snprintf(out, n, "%s", cfg.penDownCmd);
        else if (i < curveEnd)
        {
            int p = i - LEAD_LINES; // 0..points: the curve closes back on point 0
            curvePoint(p, x, y);
            if (p == 0)
                snprintf(out, n, "G1 F%.0f X%.3f Y%.3f", FEED_MM_MIN, plot.cx + x * plot.halfSize, plot.cy + y * plot.halfSize);
            else
                snprintf(out, n, "G1 X%.3f Y%.3f", plot.cx + x * plot.halfSize, plot.cy + y * plot.halfSize);
        }
        else snprintf(out, n, "%s", cfg.penUpCmd);
    }

    void stopPlot(const char *why)
    {
        if (!plot.active) return;
        plot.active = false;
        plot.ticket = 0;
        setResult(why);
    }

    void abortPlot(const char *why)
    {
        if (!plot.active) return;
        stopPlot(why);
        // Whatever's in the planner shouldn't carry on drawing.
        fluidNC.feedHold();
        fluidNC.softReset();
    }

    // Why the machine can't take a plot right now, or nullptr if it can.
    const char *notReadyReason()
    {
        const FluidNCStatus &st = fluidNC.status();
        if (!st.connected) return "Not connected to the plotter";
        if (st.mode == MachineMode::Alarm) return "Clear the alarm first";
        // Done is the few seconds of celebration after a job: idle really.
        if (st.jobActive || (st.mode != MachineMode::Idle && st.mode != MachineMode::Done))
            return "Wait for the machine to be idle";
        if (!st.havePos) return "No position from the plotter yet";
        return nullptr;
    }

    void startPlot(float sizeMm)
    {
        const FluidNCStatus &st = fluidNC.status();
        plot = Plot();
        plot.active = true;
        plot.cx = st.wposX;
        plot.cy = st.wposY;
        plot.halfSize = sizeMm / 2.0f;
        plot.total = LEAD_LINES + curve.points + 2;
        plot.startedAt = millis();
    }

    void pumpPlot()
    {
        if (!plot.active) return;
        const FluidNCStatus &st = fluidNC.status();

        if (st.mode == MachineMode::Alarm) { stopPlot("Stopped: machine alarmed"); return; }
        if (!st.connected) { stopPlot("Stopped: connection lost"); return; }
        // A rejected fire-and-forget line shows up as a failure message.
        if (st.lastFailure && (int32_t)(st.lastMessageAt - plot.startedAt) > 0)
        {
            abortPlot("Stopped: the plotter refused a line");
            return;
        }

        if (plot.ticket)
        {
            switch (fluidNC.ackState(plot.ticket))
            {
                case FluidNCClient::AckState::Pending:
                    if (millis() - plot.ticketAt > ACK_TIMEOUT_MS) abortPlot("Stopped: no reply from the plotter");
                    return;
                case FluidNCClient::AckState::Ok:
                    plot.ticket = 0;
                    break;
                default:
                    abortPlot("Stopped: the plotter refused a line");
                    return;
            }
        }

        if (plot.next >= plot.total)
        {
            plot.active = false;
            setResult("Done");
            return;
        }

        char line[64];
        for (int sent = 0; sent < BATCH && plot.next < plot.total; )
        {
            plotLine(plot.next, line, sizeof(line));
            if (!line[0]) { plot.next++; continue; }

            bool last = sent == BATCH - 1 || plot.next == plot.total - 1;
            if (last)
            {
                uint32_t t = fluidNC.sendGcodeLineTracked(line);
                if (!t) return; // queue full: same line again next time
                plot.ticket = t;
                plot.ticketAt = millis();
            }
            else if (!fluidNC.sendGcodeLine(line)) return;
            plot.next++;
            sent++;
            if (last) break;
        }
    }

    // ---------------------------------------------------------------- UI

    lv_obj_t *screen = nullptr;
    lv_obj_t *canvas = nullptr;
    lv_color_t *canvasBuf = nullptr;
    lv_coord_t canvasD = 0;
    lv_obj_t *penDot = nullptr;
    lv_obj_t *chips[P_COUNT] = {nullptr};
    lv_obj_t *chipLbls[P_COUNT] = {nullptr};
    lv_obj_t *plotBtn = nullptr;

    lv_obj_t *confirmBox = nullptr;
    lv_obj_t *confirmNote = nullptr;
    lv_obj_t *sizeChips[3] = {nullptr};
    const int SIZES_MM[3] = {60, 100, 150};
    int sizeIdx = 1;

    lv_obj_t *plotBox = nullptr;
    lv_obj_t *plotLbl = nullptr;
    lv_obj_t *plotBar = nullptr;

    // Preview animation: draws the curve a chunk per frame, so it appears
    // the way it would on paper.
    lv_timer_t *drawTimer = nullptr;
    int drawnTo = 0;
    const uint32_t PREVIEW_MS = 2200;
    const uint32_t FRAME_MS = 30;

    lv_point_t toCanvas(float x, float y)
    {
        float r = canvasD / 2.0f - px(3);
        lv_point_t p;
        p.x = (lv_coord_t)lroundf(canvasD / 2.0f + x * r);
        p.y = (lv_coord_t)lroundf(canvasD / 2.0f - y * r);
        return p;
    }

    void placePen(int i)
    {
        if (!canvas) return;
        float x, y;
        curvePoint(i, x, y);
        lv_point_t p = toCanvas(x, y);
        lv_coord_t s = lv_obj_get_width(penDot);
        lv_obj_set_pos(penDot, lv_obj_get_x(canvas) + p.x - s / 2, lv_obj_get_y(canvas) + p.y - s / 2);
        lv_obj_clear_flag(penDot, LV_OBJ_FLAG_HIDDEN);
    }

    void drawTick(lv_timer_t *)
    {
        if (!canvas || drawnTo >= curve.points) return;
        int perFrame = (int)(curve.points * FRAME_MS / PREVIEW_MS) + 1;
        int to = drawnTo + perFrame;
        if (to > curve.points) to = curve.points;

        lv_draw_line_dsc_t d;
        lv_draw_line_dsc_init(&d);
        d.color = Palette::text();
        d.width = px(1);
        d.round_start = d.round_end = 1;

        static lv_point_t run[64];
        int n = 0;
        for (int i = drawnTo; i <= to; i++)
        {
            float x, y;
            curvePoint(i, x, y);
            run[n++] = toCanvas(x, y);
            if (n == 64)
            {
                lv_canvas_draw_line(canvas, run, n, &d);
                run[0] = run[n - 1];
                n = 1;
            }
        }
        if (n >= 2) lv_canvas_draw_line(canvas, run, n, &d);
        drawnTo = to;

        if (!plot.active)
        {
            if (drawnTo < curve.points) placePen(drawnTo);
            else lv_obj_add_flag(penDot, LV_OBJ_FLAG_HIDDEN); // finished: pen lifts
        }
    }

    void restartPreview()
    {
        buildCurve();
        if (!canvas) return;
        lv_canvas_fill_bg(canvas, Palette::bgApp(), LV_OPA_COVER);
        drawnTo = 0;
    }

    void refreshChips()
    {
        char buf[24];
        for (int i = 0; i < P_COUNT; i++)
        {
            int v = params[i].value;
            if (i == P_GEAR && v >= params[P_RING].value) v = params[P_RING].value - 1;
            snprintf(buf, sizeof(buf), i == P_PEN ? "%s %d%%" : "%s %d", params[i].name, v);
            lv_label_set_text(chipLbls[i], buf);
            bool sel = i == selected;
            lv_obj_set_style_bg_color(chips[i], sel ? Palette::accent() : Palette::bgSecondary(), 0);
            lv_obj_set_style_text_color(chipLbls[i], sel ? Palette::accentFg() : Palette::textMuted(), 0);
        }
    }

    void chipCb(lv_event_t *e)
    {
        selected = (int)(intptr_t)lv_event_get_user_data(e);
        refreshChips();
    }

    lv_obj_t *makePill(lv_obj_t *parent, lv_coord_t w, lv_obj_t **outLbl)
    {
        lv_obj_t *pill = lv_obj_create(parent);
        lv_obj_remove_style_all(pill);
        lv_obj_set_size(pill, w, px(22));
        lv_obj_set_style_radius(pill, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(pill, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(pill, Palette::bgSecondary(), 0);
        lv_obj_set_ext_click_area(pill, px(4));
        lv_obj_add_flag(pill, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_t *lbl = lv_label_create(pill);
        lv_obj_set_style_text_font(lbl, &UI_FONT_12, 0);
        lv_obj_center(lbl);
        *outLbl = lbl;
        return pill;
    }

    // -- confirm --

    void refreshSizeChips()
    {
        for (int i = 0; i < 3; i++)
        {
            bool sel = i == sizeIdx;
            lv_obj_set_style_bg_color(sizeChips[i], sel ? Palette::accent() : Palette::bgSecondary(), 0);
        }
    }

    void sizeCb(lv_event_t *e)
    {
        sizeIdx = (int)(intptr_t)lv_event_get_user_data(e);
        refreshSizeChips();
    }

    void closeConfirm(lv_event_t *) { lv_obj_add_flag(confirmBox, LV_OBJ_FLAG_HIDDEN); }

    void goCb(lv_event_t *)
    {
        const char *why = notReadyReason();
        if (why)
        {
            lv_label_set_text(confirmNote, why); // say so, and stay put
            lv_obj_set_style_text_color(confirmNote, Palette::accent(), 0);
            return;
        }
        lv_obj_add_flag(confirmBox, LV_OBJ_FLAG_HIDDEN);
        startPlot((float)SIZES_MM[sizeIdx]);
    }

    void plotBtnCb(lv_event_t *)
    {
        if (plot.active) return;
        const char *why = notReadyReason();
        lv_label_set_text(confirmNote, why ? why : "Centred where the pen is now.\nPen up/down: your Settings commands.");
        lv_obj_set_style_text_color(confirmNote, why ? Palette::accent() : Palette::textMuted(), 0);
        refreshSizeChips();
        lv_obj_clear_flag(confirmBox, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(confirmBox);
    }

    void stopCb(lv_event_t *) { abortPlot("Stopped"); }

    lv_obj_t *makeOverlay()
    {
        lv_obj_t *o = lv_obj_create(screen);
        lv_obj_remove_style_all(o);
        lv_obj_set_size(o, px(240), px(240));
        lv_obj_center(o);
        lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(o, Palette::bgApp(), 0);
        lv_obj_set_style_bg_opa(o, LV_OPA_90, 0);
        lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE); // swallow taps meant for what's behind
        lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
        return o;
    }

    lv_obj_t *makeRoundBtn(lv_obj_t *parent, const char *text, lv_color_t bg, lv_coord_t w)
    {
        lv_obj_t *b = lv_btn_create(parent);
        lv_obj_set_size(b, w, px(32));
        lv_obj_set_style_radius(b, px(16), 0);
        lv_obj_set_style_bg_color(b, bg, 0);
        lv_obj_set_style_shadow_width(b, 0, 0);
        lv_obj_t *l = lv_label_create(b);
        lv_label_set_text(l, text);
        lv_obj_set_style_text_font(l, &UI_FONT_12, 0);
        lv_obj_center(l);
        return b;
    }

    void buildConfirm()
    {
        confirmBox = makeOverlay();

        lv_obj_t *title = lv_label_create(confirmBox);
        lv_label_set_text(title, "Plot this?");
        lv_obj_set_style_text_font(title, &UI_FONT_16, 0);
        lv_obj_set_style_text_color(title, Palette::text(), 0);
        lv_obj_align(title, LV_ALIGN_CENTER, 0, px(-62));

        // Size, as a row of three chips.
        for (int i = 0; i < 3; i++)
        {
            lv_obj_t *lbl;
            sizeChips[i] = makePill(confirmBox, px(50), &lbl);
            char buf[12];
            snprintf(buf, sizeof(buf), "%d mm", SIZES_MM[i]);
            lv_label_set_text(lbl, buf);
            lv_obj_set_style_text_color(lbl, Palette::text(), 0);
            lv_obj_align(sizeChips[i], LV_ALIGN_CENTER, px(-56 + 56 * i), px(-30));
            lv_obj_add_event_cb(sizeChips[i], sizeCb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        }

        confirmNote = lv_label_create(confirmBox);
        lv_obj_set_width(confirmNote, px(170));
        lv_label_set_long_mode(confirmNote, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(confirmNote, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_font(confirmNote, &UI_FONT_12, 0);
        lv_obj_align(confirmNote, LV_ALIGN_CENTER, 0, px(8));

        lv_obj_t *cancel = makeRoundBtn(confirmBox, "Cancel", Palette::bgSecondary(), px(76));
        lv_obj_align(cancel, LV_ALIGN_CENTER, px(-42), px(50));
        lv_obj_add_event_cb(cancel, closeConfirm, LV_EVENT_CLICKED, nullptr);

        lv_obj_t *go = makeRoundBtn(confirmBox, LUCIDE_PRINTER " Plot", Palette::accent(), px(76));
        lv_obj_set_style_text_font(lv_obj_get_child(go, 0), &UI_ICONS_12, 0);
        lv_obj_align(go, LV_ALIGN_CENTER, px(42), px(50));
        lv_obj_add_event_cb(go, goCb, LV_EVENT_CLICKED, nullptr);
    }

    void buildPlotting()
    {
        plotBox = lv_obj_create(screen);
        lv_obj_remove_style_all(plotBox);
        lv_obj_set_size(plotBox, px(150), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(plotBox, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(plotBox, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_row(plotBox, px(6), 0);
        lv_obj_clear_flag(plotBox, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_align(plotBox, LV_ALIGN_CENTER, 0, px(64));
        lv_obj_add_flag(plotBox, LV_OBJ_FLAG_HIDDEN);

        plotLbl = lv_label_create(plotBox);
        lv_obj_set_style_text_font(plotLbl, &UI_FONT_12, 0);
        lv_obj_set_style_text_color(plotLbl, Palette::text(), 0);

        plotBar = lv_bar_create(plotBox);
        lv_obj_set_size(plotBar, px(120), px(6));
        lv_bar_set_range(plotBar, 0, 1000);
        lv_obj_set_style_bg_color(plotBar, Palette::bgPanel(), LV_PART_MAIN);
        lv_obj_set_style_bg_color(plotBar, Palette::accent(), LV_PART_INDICATOR);

        lv_obj_t *stop = makeRoundBtn(plotBox, LUCIDE_SQUARE " Stop", Palette::alert(), px(84));
        lv_obj_set_style_text_font(lv_obj_get_child(stop, 0), &UI_ICONS_12, 0);
        lv_obj_add_event_cb(stop, stopCb, LV_EVENT_CLICKED, nullptr);
    }

    // Swaps the bottom of the screen between the settings chips and the
    // plot's progress, and keeps the preview's pen on the plotter's.
    void refreshPlotUi()
    {
        bool showResult = !plot.active && plot.result[0] && millis() - plot.resultAt < 4000;
        bool busy = plot.active || showResult;
        for (int i = 0; i < P_COUNT; i++)
        {
            if (busy) lv_obj_add_flag(chips[i], LV_OBJ_FLAG_HIDDEN);
            else lv_obj_clear_flag(chips[i], LV_OBJ_FLAG_HIDDEN);
        }
        if (busy) lv_obj_clear_flag(plotBox, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(plotBox, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_opa(plotBtn, plot.active ? LV_OPA_40 : LV_OPA_COVER, 0);
        if (!busy) return;

        char buf[56];
        if (plot.active)
        {
            int pct = plot.total ? plot.next * 100 / plot.total : 0;
            snprintf(buf, sizeof(buf), "Plotting  %d%%", pct);
            lv_bar_set_value(plotBar, plot.total ? plot.next * 1000 / plot.total : 0, LV_ANIM_OFF);
            int p = plot.next - LEAD_LINES;
            if (p >= 0 && p <= curve.points && drawnTo >= curve.points) placePen(p);
        }
        else
        {
            snprintf(buf, sizeof(buf), "%s", plot.result);
            lv_obj_add_flag(penDot, LV_OBJ_FLAG_HIDDEN);
        }
        lv_label_set_text(plotLbl, buf);
    }
}

lv_obj_t *uiSpiroCreate()
{
    screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(screen, Palette::bgApp(), 0);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    // The paper. Square, but nothing is drawn outside its inscribed circle.
    canvasD = px(132);
    canvasBuf = (lv_color_t *)heap_caps_malloc(LV_CANVAS_BUF_SIZE_TRUE_COLOR(canvasD, canvasD), MALLOC_CAP_SPIRAM);
    if (canvasBuf)
    {
        canvas = lv_canvas_create(screen);
        lv_canvas_set_buffer(canvas, canvasBuf, canvasD, canvasD, LV_IMG_CF_TRUE_COLOR);
        lv_obj_clear_flag(canvas, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_align(canvas, LV_ALIGN_CENTER, 0, px(-18));
        lv_obj_update_layout(canvas); // placePen() reads its position
    }

    penDot = lv_obj_create(screen);
    lv_obj_remove_style_all(penDot);
    lv_obj_set_size(penDot, px(6), px(6));
    lv_obj_set_style_radius(penDot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(penDot, Palette::accent(), 0);
    lv_obj_set_style_bg_opa(penDot, LV_OPA_COVER, 0);
    lv_obj_clear_flag(penDot, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(penDot, LV_OBJ_FLAG_HIDDEN);

    // Plot it -- top of the face.
    lv_obj_t *plotLblTmp;
    plotBtn = makePill(screen, px(64), &plotLblTmp);
    lv_label_set_text(plotLblTmp, LUCIDE_PRINTER " Plot");
    lv_obj_set_style_text_font(plotLblTmp, &UI_ICONS_12, 0);
    lv_obj_set_style_text_color(plotLblTmp, Palette::accentFg(), 0);
    lv_obj_set_style_bg_color(plotBtn, Palette::accent(), 0);
    lv_obj_align(plotBtn, LV_ALIGN_TOP_MID, 0, px(12));
    lv_obj_add_event_cb(plotBtn, plotBtnCb, LV_EVENT_CLICKED, nullptr);

    // The three settings, along the bottom of the paper. Tap to pick one,
    // turn the knob to change it.
    for (int i = 0; i < P_COUNT; i++)
    {
        chips[i] = makePill(screen, px(58), &chipLbls[i]);
        lv_obj_align(chips[i], LV_ALIGN_CENTER, px(-62 + 62 * i), px(64));
        lv_obj_add_event_cb(chips[i], chipCb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }

    buildPlotting();
    buildConfirm();
    addBackButton(screen);

    refreshChips();
    restartPreview();
    drawTimer = lv_timer_create(drawTick, FRAME_MS, nullptr);
    return screen;
}

void uiSpiroOnShow()
{
    // Draw it fresh each visit: the drawing appearing is half the point.
    if (!plot.active) restartPreview();
}

void uiSpiroHandleRotate(int32_t delta)
{
    if (delta == 0 || plot.active) return; // the curve can't change under a plot
    Param &p = params[selected];
    int v = p.value + (int)delta;
    if (v < p.min) v = p.min;
    if (v > p.max) v = p.max;
    if (v == p.value) return;
    p.value = v;
    refreshChips();
    restartPreview();
}

void uiSpiroUpdate()
{
    pumpPlot(); // every iteration: keeps the plotter's queue topped up

    // The screen itself only needs the UI's usual pace, and only while
    // it's the one showing.
    static uint32_t lastUi = 0;
    if (!screen || lv_scr_act() != screen || millis() - lastUi < 150) return;
    lastUi = millis();
    refreshPlotUi();
}
