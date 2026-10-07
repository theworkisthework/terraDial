#include "machine_sim.h"
#include "machine_extents.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

namespace
{
    // The demo SD card. A size of -1 is a folder, as in FluidNC's listing.
    // One long name on purpose: it's what shows off the hub's scrolling.
    struct SimFile
    {
        const char *dir;
        const char *name;
        int32_t size;
    };

    const SimFile SIM_FILES[] = {
        {"", "Portraits", -1},
        {"", "Test patterns", -1},
        {"", "snowflake.gcode", 91109},
        {"", "spirograph_rose.gcode", 248331},
        {"", "botanical study - fern fronds, layer 2 of 3 (0.3mm fineliner).gcode", 532175},
        {"", "calibration_square.nc", 2048},
        {"Portraits", "ada_lovelace.gcode", 612340},
        {"Portraits", "alan_turing.gcode", 574955},
        {"Test patterns", "pen_pressure_grid.gcode", 18233},
        {"Test patterns", "hatching_test.g", 9120},
    };
    const int SIM_FILE_COUNT = sizeof(SIM_FILES) / sizeof(SIM_FILES[0]);

    // Only Y travel is known for the real machine (machine_extents.h); X is
    // a plausible width for the demo to draw across.
    const float DEMO_X_MAX_MM = 300.0f;

    const float RAPID_MM_MIN = 6000.0f;
    const float DEFAULT_FEED_MM_MIN = 1500.0f;
    // A reset this soon after a feed hold lands while the machine is still
    // decelerating, which FluidNC treats as losing position: ALARM:3.
    const uint32_t HOLD_DECEL_MS = 500;

    const char AXES[] = "XYZ";

    void entryPath(const SimFile &f, char *out, size_t outSize)
    {
        if (f.dir[0]) snprintf(out, outSize, "%s/%s", f.dir, f.name);
        else snprintf(out, outSize, "%s", f.name);
    }

    // The value after `letter` in a G-code line, e.g. word("G0 X12.5", 'X').
    bool word(const char *text, char letter, float &out)
    {
        for (const char *p = text; *p; p++)
        {
            if (toupper((unsigned char)*p) != letter) continue;
            if (p != text && isalpha((unsigned char)p[-1])) continue; // part of a longer token
            char *end;
            float v = strtof(p + 1, &end);
            if (end == p + 1) continue;
            out = v;
            return true;
        }
        return false;
    }

    // A line that moves an axis: has an X, Y or Z word and isn't a $ command.
    bool isMoveLine(const char *text)
    {
        if (text[0] == '$') return false;
        float v;
        return word(text, 'X', v) || word(text, 'Y', v) || word(text, 'Z', v);
    }

    bool hasToken(const char *text, const char *token)
    {
        size_t n = strlen(token);
        for (const char *p = strstr(text, token); p; p = strstr(p + 1, token))
        {
            bool startOk = p == text || !isalnum((unsigned char)p[-1]);
            bool endOk = !isdigit((unsigned char)p[n]) && p[n] != '.';
            if (startOk && endOk) return true;
        }
        return false;
    }
}

void MachineSim::reset()
{
    state_ = State::Idle;
    heldFrom_ = State::Idle;
    // Parked somewhere mid-bed with the pen up, so the first jog visibly
    // moves both ways.
    mpos_[0] = 120.0f;
    mpos_[1] = 180.0f;
    mpos_[2] = 5.0f;
    wco_[0] = wco_[1] = wco_[2] = 0.0f;
    moving_ = false;
    jobPath_[0] = '\0';
    jobElapsedMs_ = 0;
    lastTickMs_ = lastReportMs_ = millis();
    deleted_ = 0;
    modalFeed_ = DEFAULT_FEED_MM_MIN;
    clearQueues();
}

void MachineSim::clearQueues()
{
    planHead_ = planCount_ = 0;
    inHead_ = inCount_ = 0;
}

void MachineSim::answer(const char *text, LineSink sink, void *ctx)
{
    say(sink, ctx, lineCommand(text, sink, ctx) ? "ok" : (state_ == State::Alarm ? "error:9" : "error:8"));
}

// Answers waiting lines in order, for as long as the planner has room for
// the next one.
void MachineSim::pumpInbox(LineSink sink, void *ctx)
{
    while (inCount_ > 0)
    {
        const char *front = inbox_[inHead_];
        if (isMoveLine(front) && planCount_ >= PLAN_DEPTH) return;
        answer(front, sink, ctx);
        inHead_ = (inHead_ + 1) % INBOX_DEPTH;
        inCount_--;
    }
}

// Starts the next queued move, if there is one.
bool MachineSim::nextPlannedMove()
{
    if (planCount_ == 0) return false;
    const Move &m = plan_[planHead_];
    startMove(m.target, m.feed);
    planHead_ = (planHead_ + 1) % PLAN_DEPTH;
    planCount_--;
    state_ = State::Run;
    return true;
}

void MachineSim::say(LineSink sink, void *ctx, const char *text)
{
    char buf[128];
    strncpy(buf, text, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    sink(ctx, buf);
}

void MachineSim::report(LineSink sink, void *ctx)
{
    const char *name = "Idle";
    switch (state_)
    {
        case State::Jog:   name = "Jog"; break;
        case State::Run:   name = "Run"; break;
        case State::Hold:  name = "Hold:0"; break;
        case State::Home:  name = "Home"; break;
        case State::Alarm: name = "Alarm"; break;
        default: break;
    }
    bool drawing = jobPath_[0] && state_ == State::Run;
    int feed = moving_ ? (int)feedMmMin_ : (drawing ? (int)DEFAULT_FEED_MM_MIN : 0);

    char sd[300] = "";
    if (jobPath_[0])
        snprintf(sd, sizeof(sd), "|SD:%.2f,%s", 100.0f * jobElapsedMs_ / DEMO_JOB_MS, jobPath_);

    char line[512];
    snprintf(line, sizeof(line), "<%s|MPos:%.3f,%.3f,%.3f|FS:%d,0|WCO:%.3f,%.3f,%.3f%s>",
             name, mpos_[0], mpos_[1], mpos_[2], feed, wco_[0], wco_[1], wco_[2], sd);
    sink(ctx, line);
}

void MachineSim::startMove(const float target[3], float feed)
{
    memcpy(target_, target, sizeof(target_));
    feedMmMin_ = feed > 0 ? feed : DEFAULT_FEED_MM_MIN;
    moving_ = true;
}

void MachineSim::command(bool raw, const char *text, LineSink sink, void *ctx)
{
    if (!raw)
    {
        // Behind lines already waiting, or a move with the planner full:
        // wait, unanswered, like a line in FluidNC's receive buffer.
        if (inCount_ > 0 || (isMoveLine(text) && planCount_ >= PLAN_DEPTH))
        {
            if (inCount_ >= INBOX_DEPTH)
            {
                say(sink, ctx, "error:8"); // a sender ignoring the oks: refuse rather than lose order
                return;
            }
            int slot = (inHead_ + inCount_) % INBOX_DEPTH;
            strncpy(inbox_[slot], text, INBOX_LINE - 1);
            inbox_[slot][INBOX_LINE - 1] = '\0';
            inCount_++;
            return;
        }
        answer(text, sink, ctx);
        return;
    }

    switch ((uint8_t)text[0])
    {
        case '?':
            report(sink, ctx);
            break;

        case '!': // feed hold
            if (state_ == State::Run)
            {
                heldFrom_ = State::Run;
                state_ = State::Hold;
                holdStartedMs_ = millis(); // see HOLD_DECEL_MS
            }
            else if (state_ == State::Jog)
            {
                moving_ = false; // a hold during a jog just ends the jog
                state_ = State::Idle;
            }
            break;

        case '~': // cycle start / resume
            if (state_ == State::Hold) state_ = heldFrom_;
            break;

        case 0x18: // soft reset
        {
            clearQueues(); // FluidNC drops everything buffered on a reset
            bool inMotion = state_ == State::Run || state_ == State::Jog || state_ == State::Home ||
                            (state_ == State::Hold && millis() - holdStartedMs_ < HOLD_DECEL_MS);
            moving_ = false;
            jobPath_[0] = '\0';
            jobElapsedMs_ = 0;
            if (inMotion)
            {
                state_ = State::Alarm;
                say(sink, ctx, "ALARM:3"); // reset while in motion: position no longer trusted
            }
            else if (state_ != State::Alarm)
            {
                state_ = State::Idle;
            }
            break;
        }

        case 0x85: // jog cancel
            if (state_ == State::Jog)
            {
                moving_ = false;
                state_ = State::Idle;
            }
            break;
    }
}

// Returns false to reject the command (the caller answers with an error).
bool MachineSim::lineCommand(const char *text, LineSink sink, void *ctx)
{
    if (!strcmp(text, "$X"))
    {
        if (state_ == State::Alarm)
        {
            state_ = State::Idle;
            say(sink, ctx, "[MSG:INFO: Caution: Unlocked]");
        }
        return true;
    }

    if (!strcmp(text, "$H"))
    {
        if (state_ != State::Idle && state_ != State::Alarm) return false;
        const float home[3] = {0, 0, 0};
        float dist = sqrtf(mpos_[0] * mpos_[0] + mpos_[1] * mpos_[1] + mpos_[2] * mpos_[2]);
        // Whatever the distance, take about HOMING_MS -- long enough to see.
        startMove(home, dist > 1.0f ? dist * 60000.0f / HOMING_MS : RAPID_MM_MIN);
        state_ = State::Home;
        return true;
    }

    if (!strncmp(text, "$SD/Delete=", 11))
    {
        deleteFile(text + 11);
        return true;
    }

    // Settings and queries always answer, alarm or not.
    if (text[0] == '$' && strncmp(text, "$J=", 3) != 0 && strncmp(text, "$SD/Run=", 8) != 0) return true;

    // Everything below moves the machine: locked out in an alarm (error:9).
    if (state_ == State::Alarm) return false;

    if (!strncmp(text, "$J=", 3))
    {
        if (state_ != State::Idle && state_ != State::Jog) return false; // error:8, not idle
        float target[3];
        memcpy(target, mpos_, sizeof(target));
        bool relative = hasToken(text, "G91");
        for (int a = 0; a < 3; a++)
        {
            float v;
            if (word(text + 3, AXES[a], v)) target[a] = relative ? mpos_[a] + v : v + wco_[a];
        }
        float feed = DEFAULT_FEED_MM_MIN;
        word(text + 3, 'F', feed);
        startMove(target, feed);
        state_ = State::Jog;
        return true;
    }

    if (!strncmp(text, "$SD/Run=", 8))
    {
        if (state_ != State::Idle) return false;
        strncpy(jobPath_, text + 8, sizeof(jobPath_) - 1);
        jobPath_[sizeof(jobPath_) - 1] = '\0';
        jobElapsedMs_ = 0;
        memcpy(jobStartPos_, mpos_, sizeof(jobStartPos_));
        moving_ = false;
        state_ = State::Run;
        return true;
    }

    // G-code. Work zero (G10 L20 P0 X0) moves the work origin, not the head.
    if (hasToken(text, "G10") && hasToken(text, "L20"))
    {
        for (int a = 0; a < 3; a++)
        {
            float v;
            if (word(text, AXES[a], v)) wco_[a] = mpos_[a] - v;
        }
        return true;
    }

    // Idle, or already working through streamed moves (not an SD job, and
    // not a jog or homing cycle): a move joins the queue.
    bool streaming = (state_ == State::Run && !jobPath_[0]) || (state_ == State::Hold && heldFrom_ == State::Run && !jobPath_[0]);
    if (state_ != State::Idle && !streaming) return false;
    gcodeMove(text);
    return true;
}

// A plain move (the Park macro's "G90 G53 G0 Y420 F3000", say).
void MachineSim::gcodeMove(const char *text)
{
    bool relative = hasToken(text, "G91");
    bool machineCoords = hasToken(text, "G53");
    float target[3];
    memcpy(target, mpos_, sizeof(target));
    bool any = false;
    for (int a = 0; a < 3; a++)
    {
        float v;
        if (!word(text, AXES[a], v)) continue;
        any = true;
        if (relative) target[a] = mpos_[a] + v;
        else if (machineCoords) target[a] = v;
        else target[a] = v + wco_[a];
    }
    if (!any) return; // modal-only line (G90, M5 ...): nothing moves

    float f;
    if (word(text, 'F', f) && f > 0) modalFeed_ = f;
    float feed = hasToken(text, "G0") ? RAPID_MM_MIN : modalFeed_;

    // Queued behind whatever's already moving; straight off if nothing is.
    // Targets chain: a queued move starts from the previous one's end,
    // which is mpos_ by the time it runs.
    if (planCount_ < PLAN_DEPTH)
    {
        Move &m = plan_[(planHead_ + planCount_) % PLAN_DEPTH];
        memcpy(m.target, target, sizeof(m.target));
        m.feed = feed;
        planCount_++;
    }
    if (!moving_ && state_ != State::Hold) nextPlannedMove();
    else if (state_ == State::Idle) state_ = State::Run; // FluidNC reports Run for any motion that isn't a jog
}

void MachineSim::deleteFile(const char *path)
{
    char p[300];
    for (int i = 0; i < SIM_FILE_COUNT; i++)
    {
        entryPath(SIM_FILES[i], p, sizeof(p));
        if (!strcmp(p, path)) deleted_ |= 1u << i;
    }
}

void MachineSim::tick(LineSink sink, void *ctx)
{
    uint32_t now = millis();
    uint32_t dt = now - lastTickMs_;
    if (dt > 200) dt = 200; // a stalled task shouldn't teleport the head
    lastTickMs_ = now;

    pumpInbox(sink, ctx);
    if (!moving_ && planCount_ > 0 && state_ != State::Hold && state_ != State::Alarm) nextPlannedMove();

    if (state_ == State::Run && jobPath_[0])
    {
        jobElapsedMs_ += dt;
        if (jobElapsedMs_ >= DEMO_JOB_MS)
        {
            // Done: park the pen up where it finished, as a real job would.
            jobPath_[0] = '\0';
            jobElapsedMs_ = 0;
            mpos_[2] = 5.0f;
            state_ = State::Idle;
            say(sink, ctx, "[MSG:INFO: Program End]");
        }
        else
        {
            // Draw something that looks like a plot, because the Job
            // Progress screen draws it back (plot_mirror.h): a fan of wavy
            // arcs radiating from the bottom-right of the bed, one at a
            // time. Arcs alternate direction, so the pen-up travel between
            // them is a short hop outwards rather than a trip back across.
            // (This was a Lissajous figure retraced six times with a pen
            // lift every second -- fine for the readouts, but mirrored on
            // screen it read as a broken drawing going round in circles.)
            const int ARCS = 9;
            const float DRAW_SHARE = 0.85f; // of each arc's slot; the rest is travel
            const float CX = DEMO_X_MAX_MM * 0.9f, CY = MACHINE_Y_MAX_MM * 0.1f;
            const float R0 = 30.0f, R_STEP = 24.0f;
            // Ripples along each quarter-turn. Few enough that the ten
            // position reports a second land ~10 to a ripple -- more, and
            // the mirror would draw them as zig-zags.
            const float WAVE_MM = 5.0f, WAVES = 4.0f;

            // The first moments glide in from wherever the head was, pen up.
            const float LEAD_IN_MS = 1500.0f;
            float lead = jobElapsedMs_ >= LEAD_IN_MS ? 1.0f : jobElapsedMs_ / LEAD_IN_MS;
            float t = jobElapsedMs_ < LEAD_IN_MS ? 0.0f
                                                 : (jobElapsedMs_ - LEAD_IN_MS) / (DEMO_JOB_MS - LEAD_IN_MS);

            float slot = t * ARCS;
            int k = (int)slot;
            if (k >= ARCS) k = ARCS - 1;
            float f = slot - k; // 0..1 through this arc's slot

            // Arc k at a fraction u along it (0..1, in its own direction).
            auto arcPoint = [&](int arc, float u, float &x, float &y) {
                if (arc % 2) u = 1.0f - u;
                float th = (float)M_PI * (0.5f + 0.5f * u); // straight up round to straight left
                float r = R0 + R_STEP * arc + WAVE_MM * sinf(WAVES * 4.0f * th + arc * 0.9f);
                x = CX + r * cosf(th);
                y = CY + r * sinf(th);
            };

            float pattern[3];
            bool drawing = f < DRAW_SHARE;
            if (drawing)
            {
                arcPoint(k, f / DRAW_SHARE, pattern[0], pattern[1]);
            }
            else
            {
                // Travel: from the end of this arc to the start of the next.
                float ax, ay, bx, by;
                arcPoint(k, 1.0f, ax, ay);
                arcPoint(k + 1 < ARCS ? k + 1 : k, 0.0f, bx, by);
                float g = (f - DRAW_SHARE) / (1.0f - DRAW_SHARE);
                pattern[0] = ax + (bx - ax) * g;
                pattern[1] = ay + (by - ay) * g;
            }
            pattern[2] = (drawing && lead >= 1.0f) ? 0.0f : 5.0f;

            for (int i = 0; i < 2; i++)
                mpos_[i] = jobStartPos_[i] + (pattern[i] - jobStartPos_[i]) * lead;
            mpos_[2] = pattern[2]; // the pen goes straight up or down, never glides
        }
    }
    else if (moving_ && state_ != State::Hold)
    {
        float d[3], dist = 0;
        for (int a = 0; a < 3; a++)
        {
            d[a] = target_[a] - mpos_[a];
            dist += d[a] * d[a];
        }
        dist = sqrtf(dist);
        float step = feedMmMin_ * dt / 60000.0f;
        if (dist <= step || dist < 0.001f)
        {
            memcpy(mpos_, target_, sizeof(mpos_));
            moving_ = false;
            if (!nextPlannedMove() &&
                (state_ == State::Jog || state_ == State::Run || state_ == State::Home))
                state_ = State::Idle;
        }
        else
        {
            for (int a = 0; a < 3; a++) mpos_[a] += d[a] / dist * step;
        }
    }

    if (now - lastReportMs_ >= REPORT_MS)
    {
        lastReportMs_ = now;
        report(sink, ctx);
    }
}

String MachineSim::listJson(const char *dir) const
{
    String json = "{\"files\":[";
    bool first = true;
    for (int i = 0; i < SIM_FILE_COUNT; i++)
    {
        const SimFile &f = SIM_FILES[i];
        if ((deleted_ & (1u << i)) || strcmp(f.dir, dir) != 0) continue;
        if (!first) json += ",";
        first = false;
        json += "{\"name\":\"";
        for (const char *c = f.name; *c; c++)
        {
            if (*c == '"' || *c == '\\') json += '\\';
            json += *c;
        }
        json += "\",\"size\":\"";
        json += String(f.size);
        json += "\"}";
    }
    json += "],\"path\":\"/";
    json += dir;
    json += "\",\"status\":\"Ok\"}";
    return json;
}
