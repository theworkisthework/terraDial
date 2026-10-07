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
    //
    // A gear of `gear` teeth rolls round a ring of `ring` teeth -- inside it,
    // or (Out) round the outside -- with the pen a distance `pen`% of the
    // gear's radius from the gear's centre, at an angle set by where the
    // nib was dragged to. Optionally the whole pattern is repeated `copies`
    // times, each turned to sit between the petals of the last: the classic
    // layered spirograph lace.
    //
    // In complex terms the pen is at
    //     c e^{it}  +  d e^{i(phase -/+ k t)},   c = ring -/+ gear, k = c / gear
    // -- the gear's centre going round the ring, plus the pen arm turning on
    // the gear. Inside, the gear turns against its travel (the minus); round
    // the outside, with it.

    struct Param
    {
        const char *name; // for the toast
        const char *tag;  // for the chip
        int value, min, max;
    };
    enum { P_RING, P_GEAR, P_PEN, P_COPIES, P_COUNT };
    Param params[P_COUNT] = {
        {"Ring", "R", 96, 40, 150},
        {"Gear", "G", 36, 8, 140},
        {"Pen", "P", 70, 5, 150}, // % of the gear's radius: past 100 it loops
        {"Copies", "x", 1, 1, 6},
    };
    int selected = P_GEAR;
    bool outside = false;
    float phase = 0; // where on the gear the nib sits, radians

    // How finely the curve is cut up. Enough points per trip round the ring
    // to look smooth, but capped overall: a dense curve can need a hundred
    // trips, and every point becomes a line of G-code.
    const int MAX_POINTS = 3000;

    int gcd(int a, int b)
    {
        while (b) { int t = a % b; a = b; b = t; }
        return a;
    }

    struct Curve
    {
        float ring, gear, pen; // in teeth
        float c, k;            // gear-centre orbit radius, gear spin ratio
        int turns;             // trips round the ring before it closes
        int petals;            // the pattern's rotational symmetry
        int copies;
        int perCopy;           // segments in one copy (points = perCopy + 1)
        float reach;           // furthest the pen gets from the centre
        float frame;           // what the drawing area's rim is, in teeth
    } curve;

    int gearTeeth()
    {
        int g = params[P_GEAR].value;
        // Inside, the gear has to fit in the ring.
        if (!outside && g >= params[P_RING].value) g = params[P_RING].value - 1;
        return g;
    }

    // The drawing area's rim, in teeth. Inside, the ring IS the rim, unless
    // the pen reaches past it; outside, the rim takes in the gear's whole
    // orbit and the pen's.
    float frameFor(float pen)
    {
        if (!outside) return fmaxf(curve.ring, curve.c + pen);
        return curve.c + fmaxf(curve.gear, pen);
    }

    void buildCurve()
    {
        int ring = params[P_RING].value;
        int gear = gearTeeth();
        curve.ring = ring;
        curve.gear = gear;
        curve.pen = params[P_PEN].value * gear / 100.0f;
        curve.c = outside ? ring + gear : ring - gear;
        curve.k = curve.c / gear;
        int g = gcd(ring, gear);
        curve.turns = gear / g;
        curve.petals = ring / g;
        curve.copies = params[P_COPIES].value;
        int perTurn = MAX_POINTS / (curve.copies * curve.turns);
        if (perTurn > 240) perTurn = 240;
        if (perTurn < 12) perTurn = 12;
        curve.perCopy = curve.turns * perTurn;
        curve.reach = curve.c + curve.pen;
        curve.frame = frameFor(curve.pen);
    }

    int totalPoints() { return curve.copies * (curve.perCopy + 1); }

    // Turn of copy j: an even share of the gap between two petals, so the
    // copies interleave rather than land on each other.
    float copyTurn(int copy) { return copy * (2.0f * (float)M_PI / curve.petals) / curve.copies; }

    float stepAngle(int i) { return 2.0f * (float)M_PI * curve.turns * i / curve.perCopy; }

    void rotate(float a, float &x, float &y)
    {
        float cs = cosf(a), sn = sinf(a);
        float nx = x * cs - y * sn;
        y = x * sn + y * cs;
        x = nx;
    }

    // Global point g (copies run one after another), in teeth.
    void rawPoint(int g, float &x, float &y)
    {
        int copy = g / (curve.perCopy + 1);
        float t = stepAngle(g % (curve.perCopy + 1));
        float arm = outside ? phase + curve.k * t : phase - curve.k * t;
        x = curve.c * cosf(t) + curve.pen * cosf(arm);
        y = curve.c * sinf(t) + curve.pen * sinf(arm);
        rotate(copyTurn(copy), x, y);
    }

    // Where the gear's centre is at global point g, in teeth.
    void gearCentre(int g, float &x, float &y)
    {
        int copy = g / (curve.perCopy + 1);
        float t = stepAngle(g % (curve.perCopy + 1));
        x = curve.c * cosf(t);
        y = curve.c * sinf(t);
        rotate(copyTurn(copy), x, y);
    }

    bool copyStart(int g) { return g % (curve.perCopy + 1) == 0; }

    // ---------------------------------------------------------------- plot
    //
    // Streams the curve to FluidNC as G-code. There's no file: the lines
    // are made one at a time as they go out. They go in batches -- a few
    // fire-and-forget lines, then one whose "ok" is waited for -- because
    // waiting on every line would leave the planner starving between the
    // short segments a curve is made of, and the pen would stutter.
    // FluidNC answers lines in order, so the batch's last "ok" means the
    // whole batch has been taken.
    //
    // Lines: G21, G90, then per copy [pen up, travel to its start, pen
    // down, its points], then a final pen up.

    const float FEED_MM_MIN = 2000.0f;
    const int BATCH = 6;
    const uint32_t ACK_TIMEOUT_MS = 60000; // a full planner can hold an ok back a while
    const int HEAD_LINES = 2;
    const int COPY_LEAD = 3;

    struct Plot
    {
        bool active = false;
        float cx = 0, cy = 0, scale = 1; // mm per tooth
        int next = 0;   // next line to send
        int total = 0;  // lines in the whole plot
        uint32_t ticket = 0;
        uint32_t ticketAt = 0;
        uint32_t startedAt = 0;
        char result[48] = "";
        uint32_t resultAt = 0;
    } plot;

    int copyBlock() { return COPY_LEAD + curve.perCopy + 1; }

    void setResult(const char *msg)
    {
        snprintf(plot.result, sizeof(plot.result), "%s", msg);
        plot.resultAt = millis();
    }

    // The global curve point line i draws to, or -1 for a line that isn't one.
    int linePoint(int i)
    {
        int j = i - HEAD_LINES;
        if (j < 0 || j >= curve.copies * copyBlock()) return -1;
        int within = j % copyBlock() - COPY_LEAD;
        if (within < 0) return -1;
        return (j / copyBlock()) * (curve.perCopy + 1) + within;
    }

    // Line i of the plot, or "" for one that has nothing to send (a pen
    // command left blank in Settings).
    void plotLine(int i, char *out, size_t n)
    {
        const AppSettings &cfg = Config::get();
        if (i == 0) { snprintf(out, n, "G21"); return; }
        if (i == 1) { snprintf(out, n, "G90"); return; }
        int j = i - HEAD_LINES;
        if (j >= curve.copies * copyBlock()) { snprintf(out, n, "%s", cfg.penUpCmd); return; }

        int copy = j / copyBlock(), within = j % copyBlock();
        float x, y;
        if (within == 0) { snprintf(out, n, "%s", cfg.penUpCmd); return; }
        if (within == 2) { snprintf(out, n, "%s", cfg.penDownCmd); return; }
        if (within == 1)
        {
            rawPoint(copy * (curve.perCopy + 1), x, y);
            snprintf(out, n, "G0 X%.3f Y%.3f", plot.cx + x * plot.scale, plot.cy + y * plot.scale);
            return;
        }
        rawPoint(linePoint(i), x, y);
        if (within == COPY_LEAD) // each copy's first draw move sets the feed
            snprintf(out, n, "G1 F%.0f X%.3f Y%.3f", FEED_MM_MIN, plot.cx + x * plot.scale, plot.cy + y * plot.scale);
        else
            snprintf(out, n, "G1 X%.3f Y%.3f", plot.cx + x * plot.scale, plot.cy + y * plot.scale);
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
        // Sized by the drawing itself, not the preview's frame: "100mm" is
        // the pattern's own width.
        plot.scale = (sizeMm / 2.0f) / curve.reach;
        plot.total = HEAD_LINES + curve.copies * copyBlock() + 1;
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

    // The toy itself, drawn over the paper: the gear outline, its arm out
    // to the nib, and the nib. They roll round as the pattern draws and
    // park at the start when it's done -- which is where you grab the nib.
    lv_obj_t *gearObj = nullptr;
    lv_obj_t *armObj = nullptr;
    lv_point_t armPts[2];
    lv_obj_t *penDot = nullptr;

    lv_obj_t *chips[P_COUNT] = {nullptr};
    lv_obj_t *chipLbls[P_COUNT] = {nullptr};
    lv_obj_t *modeBtn = nullptr;
    lv_obj_t *modeLbl = nullptr;
    lv_obj_t *plotBtn = nullptr;
    lv_obj_t *toast = nullptr;
    uint32_t toastAt = 0;

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
    const uint32_t FRAME_MS = 30;
    uint32_t previewMs() { return 2600 + 900 * (curve.copies - 1); }

    // Dragging the nib.
    bool dragging = false;

    float rimPx() { return canvasD / 2.0f - px(3); }

    // Teeth (y up, frame = rim) -> screen px, absolute within the screen.
    lv_point_t toScreen(float x, float y)
    {
        float s = rimPx() / curve.frame;
        lv_point_t p;
        p.x = (lv_coord_t)lroundf(lv_obj_get_x(canvas) + canvasD / 2.0f + x * s);
        p.y = (lv_coord_t)lroundf(lv_obj_get_y(canvas) + canvasD / 2.0f - y * s);
        return p;
    }

    lv_point_t toCanvas(float x, float y)
    {
        lv_point_t p = toScreen(x, y);
        p.x -= lv_obj_get_x(canvas);
        p.y -= lv_obj_get_y(canvas);
        return p;
    }

    // Puts the gear, arm and nib where they are at global point g.
    void placeToy(int g)
    {
        if (!canvas) return;
        float gx, gy, nx, ny;
        gearCentre(g, gx, gy);
        rawPoint(g, nx, ny);
        lv_point_t gc = toScreen(gx, gy), nib = toScreen(nx, ny);

        lv_coord_t gd = (lv_coord_t)lroundf(2.0f * curve.gear * rimPx() / curve.frame);
        lv_obj_set_size(gearObj, gd, gd);
        lv_obj_set_pos(gearObj, gc.x - gd / 2, gc.y - gd / 2);

        armPts[0] = gc;
        armPts[1] = nib;
        lv_line_set_points(armObj, armPts, 2);

        lv_coord_t s = lv_obj_get_width(penDot);
        lv_obj_set_pos(penDot, nib.x - s / 2, nib.y - s / 2);

        lv_obj_clear_flag(gearObj, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(armObj, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(penDot, LV_OBJ_FLAG_HIDDEN);
    }

    lv_draw_line_dsc_t inkStyle()
    {
        lv_draw_line_dsc_t d;
        lv_draw_line_dsc_init(&d);
        d.color = Palette::text();
        d.width = px(1);
        d.round_start = d.round_end = 1;
        return d;
    }

    // The ring, faint, round the rim of the paper (or, rolling outside,
    // where it sits in the middle).
    void drawRing()
    {
        lv_draw_arc_dsc_t a;
        lv_draw_arc_dsc_init(&a);
        a.color = Palette::border();
        a.width = px(1);
        a.opa = LV_OPA_60;
        lv_point_t c = toCanvas(0, 0);
        lv_coord_t r = (lv_coord_t)lroundf(curve.ring * rimPx() / curve.frame);
        lv_canvas_draw_arc(canvas, c.x, c.y, r, 0, 360, &a);
    }

    void clearPaper()
    {
        lv_canvas_fill_bg(canvas, Palette::bgApp(), LV_OPA_COVER);
        drawRing();
    }

    void drawTick(lv_timer_t *)
    {
        if (!canvas || dragging || drawnTo >= totalPoints()) return;
        int total = totalPoints();
        int perFrame = (int)(total * FRAME_MS / previewMs()) + 1;
        int to = drawnTo + perFrame;
        if (to > total - 1) to = total - 1;

        lv_draw_line_dsc_t d = inkStyle();
        static lv_point_t run[64];
        int n = 0;
        for (int g = drawnTo; g <= to; g++)
        {
            // A new copy starts a new line: the pen lifts between them.
            if (copyStart(g) && n)
            {
                if (n >= 2) lv_canvas_draw_line(canvas, run, n, &d);
                n = 0;
            }
            float x, y;
            rawPoint(g, x, y);
            run[n++] = toCanvas(x, y);
            if (n == 64)
            {
                lv_canvas_draw_line(canvas, run, n, &d);
                run[0] = run[n - 1];
                n = 1;
            }
        }
        if (n >= 2) lv_canvas_draw_line(canvas, run, n, &d);
        drawnTo = to + 1 >= total ? total : to;

        if (!plot.active) placeToy(drawnTo >= total ? 0 : drawnTo); // done: park at the start
    }

    void restartPreview()
    {
        buildCurve();
        if (!canvas) return;
        clearPaper();
        drawnTo = 0;
        placeToy(0);
    }

    void showToast(const char *text)
    {
        lv_label_set_text(toast, text);
        lv_obj_clear_flag(toast, LV_OBJ_FLAG_HIDDEN);
        toastAt = millis();
    }

    void toastParam(int i)
    {
        char buf[32];
        if (i == P_PEN) snprintf(buf, sizeof(buf), "%s %d%%", params[i].name, params[i].value);
        else if (i == P_GEAR) snprintf(buf, sizeof(buf), "%s %d", params[i].name, gearTeeth());
        else snprintf(buf, sizeof(buf), "%s %d", params[i].name, params[i].value);
        showToast(buf);
    }

    void refreshChips()
    {
        char buf[16];
        for (int i = 0; i < P_COUNT; i++)
        {
            int v = i == P_GEAR ? gearTeeth() : params[i].value;
            if (i == P_COPIES) snprintf(buf, sizeof(buf), "x%d", v);
            else snprintf(buf, sizeof(buf), "%s%d", params[i].tag, v);
            lv_label_set_text(chipLbls[i], buf);
            bool sel = i == selected;
            lv_obj_set_style_bg_color(chips[i], sel ? Palette::accent() : Palette::bgSecondary(), 0);
            lv_obj_set_style_text_color(chipLbls[i], sel ? Palette::accentFg() : Palette::textMuted(), 0);
        }
        lv_label_set_text(modeLbl, outside ? "Out" : "In");
    }

    void chipCb(lv_event_t *e)
    {
        selected = (int)(intptr_t)lv_event_get_user_data(e);
        refreshChips();
        toastParam(selected);
    }

    void modeCb(lv_event_t *)
    {
        if (plot.active) return;
        outside = !outside;
        // The usual starting nib for each: pointing at the rim inside,
        // at the ring's centre outside.
        phase = outside ? (float)M_PI : 0.0f;
        refreshChips();
        showToast(outside ? "Rolling outside" : "Rolling inside");
        restartPreview();
    }

    // Dragging on the paper puts the nib under your finger: its distance
    // from the gear's centre is Pen, its direction the phase. The frame is
    // held still for the drag, or the pen reaching past the rim would
    // rescale the view under the finger.
    void paperCb(lv_event_t *e)
    {
        if (plot.active) return;
        lv_event_code_t code = lv_event_get_code(e);
        if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST)
        {
            if (!dragging) return;
            dragging = false;
            restartPreview();
            return;
        }
        if (code != LV_EVENT_PRESSED && code != LV_EVENT_PRESSING) return;

        lv_point_t pt;
        lv_indev_get_point(lv_indev_get_act(), &pt);
        if (!dragging)
        {
            dragging = true;
            clearPaper(); // the half-drawn pattern would only mislead
        }

        float s = rimPx() / curve.frame;
        float x = (pt.x - (lv_obj_get_x(canvas) + canvasD / 2.0f)) / s;
        float y = -(pt.y - (lv_obj_get_y(canvas) + canvasD / 2.0f)) / s;
        float vx = x - curve.c, vy = y; // from the gear's centre at the start
        float d = sqrtf(vx * vx + vy * vy);
        int pct = (int)lroundf(100.0f * d / curve.gear);
        if (pct < params[P_PEN].min) pct = params[P_PEN].min;
        if (pct > params[P_PEN].max) pct = params[P_PEN].max;
        params[P_PEN].value = pct;
        if (d > 0.01f) phase = atan2f(vy, vx);

        float keepFrame = curve.frame;
        buildCurve();
        curve.frame = keepFrame;
        selected = P_PEN;
        refreshChips();
        toastParam(P_PEN);
        placeToy(0);
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
            lv_obj_set_style_bg_color(sizeChips[i], i == sizeIdx ? Palette::accent() : Palette::bgSecondary(), 0);
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
        lv_obj_align(plotBox, LV_ALIGN_CENTER, 0, px(62));
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
    // plot's progress, keeps the toy on the plotter's pen, and lets the
    // toast go.
    void refreshPlotUi()
    {
        if (!lv_obj_has_flag(toast, LV_OBJ_FLAG_HIDDEN) && millis() - toastAt > 1200)
            lv_obj_add_flag(toast, LV_OBJ_FLAG_HIDDEN);

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
        lv_obj_set_style_opa(modeBtn, plot.active ? LV_OPA_40 : LV_OPA_COVER, 0);
        if (!busy) return;

        char buf[56];
        if (plot.active)
        {
            int pct = plot.total ? plot.next * 100 / plot.total : 0;
            snprintf(buf, sizeof(buf), "Plotting  %d%%", pct);
            lv_bar_set_value(plotBar, plot.total ? plot.next * 1000 / plot.total : 0, LV_ANIM_OFF);
            int g = linePoint(plot.next);
            if (g >= 0 && drawnTo >= totalPoints()) placeToy(g);
        }
        else
        {
            snprintf(buf, sizeof(buf), "%s", plot.result);
        }
        lv_label_set_text(plotLbl, buf);
    }
}

lv_obj_t *uiSpiroCreate()
{
    screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(screen, Palette::bgApp(), 0);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    // The paper. Square, but everything on it stays inside its circle.
    canvasD = px(128);
    canvasBuf = (lv_color_t *)heap_caps_malloc(LV_CANVAS_BUF_SIZE_TRUE_COLOR(canvasD, canvasD), MALLOC_CAP_SPIRAM);
    if (canvasBuf)
    {
        canvas = lv_canvas_create(screen);
        lv_canvas_set_buffer(canvas, canvasBuf, canvasD, canvasD, LV_IMG_CF_TRUE_COLOR);
        lv_obj_align(canvas, LV_ALIGN_CENTER, 0, px(-20));
        lv_obj_update_layout(canvas); // the toy reads its position
        lv_obj_add_flag(canvas, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(canvas, LV_OBJ_FLAG_SCROLL_CHAIN);
        lv_obj_add_event_cb(canvas, paperCb, LV_EVENT_ALL, nullptr);
    }

    gearObj = lv_obj_create(screen);
    lv_obj_remove_style_all(gearObj);
    lv_obj_set_style_radius(gearObj, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_color(gearObj, Palette::accentSecondary(), 0);
    lv_obj_set_style_border_width(gearObj, px(1), 0);
    lv_obj_set_style_border_opa(gearObj, LV_OPA_70, 0);
    lv_obj_clear_flag(gearObj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(gearObj, LV_OBJ_FLAG_HIDDEN);

    armObj = lv_line_create(screen);
    lv_obj_set_style_line_color(armObj, Palette::accentSecondary(), 0);
    lv_obj_set_style_line_width(armObj, px(1), 0);
    lv_obj_set_style_line_opa(armObj, LV_OPA_70, 0);
    lv_obj_clear_flag(armObj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(armObj, LV_OBJ_FLAG_HIDDEN);

    penDot = lv_obj_create(screen);
    lv_obj_remove_style_all(penDot);
    lv_obj_set_size(penDot, px(6), px(6));
    lv_obj_set_style_radius(penDot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(penDot, Palette::accent(), 0);
    lv_obj_set_style_bg_opa(penDot, LV_OPA_COVER, 0);
    lv_obj_clear_flag(penDot, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(penDot, LV_OBJ_FLAG_HIDDEN);

    // Top of the face: In/Out and Plot.
    modeBtn = makePill(screen, px(56), &modeLbl);
    lv_obj_set_style_text_color(modeLbl, Palette::text(), 0);
    lv_obj_align(modeBtn, LV_ALIGN_TOP_MID, px(-32), px(20));
    lv_obj_add_event_cb(modeBtn, modeCb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *plotLblTmp;
    plotBtn = makePill(screen, px(56), &plotLblTmp);
    lv_label_set_text(plotLblTmp, LUCIDE_PRINTER " Plot");
    lv_obj_set_style_text_font(plotLblTmp, &UI_ICONS_12, 0);
    lv_obj_set_style_text_color(plotLblTmp, Palette::accentFg(), 0);
    lv_obj_set_style_bg_color(plotBtn, Palette::accent(), 0);
    lv_obj_align(plotBtn, LV_ALIGN_TOP_MID, px(32), px(20));
    lv_obj_add_event_cb(plotBtn, plotBtnCb, LV_EVENT_CLICKED, nullptr);

    // The settings, along the bottom of the paper. Tap one, turn the knob.
    for (int i = 0; i < P_COUNT; i++)
    {
        chips[i] = makePill(screen, px(44), &chipLbls[i]);
        lv_obj_align(chips[i], LV_ALIGN_CENTER, px(-72 + 48 * i), px(60));
        lv_obj_add_event_cb(chips[i], chipCb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }

    // What the knob just changed, in words, over the middle of the paper.
    toast = lv_label_create(screen);
    lv_obj_set_style_text_font(toast, &UI_FONT_14, 0);
    lv_obj_set_style_text_color(toast, Palette::text(), 0);
    lv_obj_set_style_bg_color(toast, Palette::bgSecondary(), 0);
    lv_obj_set_style_bg_opa(toast, LV_OPA_80, 0);
    lv_obj_set_style_radius(toast, px(10), 0);
    lv_obj_set_style_pad_hor(toast, px(10), 0);
    lv_obj_set_style_pad_ver(toast, px(3), 0);
    lv_obj_clear_flag(toast, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(toast, LV_ALIGN_CENTER, 0, px(-20));
    lv_obj_add_flag(toast, LV_OBJ_FLAG_HIDDEN);

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
    if (delta == 0 || plot.active || dragging) return; // the curve can't change under a plot
    Param &p = params[selected];
    int v = p.value + (int)delta;
    if (v < p.min) v = p.min;
    if (v > p.max) v = p.max;
    if (v == p.value) return;
    p.value = v;
    refreshChips();
    toastParam(selected);
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
