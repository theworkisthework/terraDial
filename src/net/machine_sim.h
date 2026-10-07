#pragma once

#include <Arduino.h>

// A pretend FluidNC for demo mode (see demo_mode.h).
//
// It doesn't set the panel's machine state directly. It answers commands
// and reports status in FluidNC's own words -- "<Run|MPos:...|SD:42.0,...>",
// "ok", "ALARM:3" -- and those lines go through FluidNCClient's real
// parser. So a demo exercises the same state tracking, job detection and
// auto-navigation a real machine does, rather than a second copy of them
// that could quietly drift.
//
// Covers what the panel sends: jogs and jog-cancel, homing, $X, SD job
// run/pause/resume/stop, file delete, work-zero (G10 L20) and plain G-code
// moves -- one at a time (the Park macro) or streamed (the spirograph),
// through a small planner queue that paces the stream the way FluidNC's
// does: when it's full, the next line's "ok" waits for room. A job takes DEMO_JOB_MS and draws a fan of wavy
// arcs, pen down along each and up between them, so the position readouts
// move and the Job Progress mirror has a believable plot to draw. The SD card is a fixed
// set of folders and files, served as the same JSON FluidNC's HTTP
// listing returns.
class MachineSim
{
public:
    // Receives each line the "machine" says. `line` is a scratch buffer
    // the sink may modify.
    typedef void (*LineSink)(void *ctx, char *line);

    void reset();

    // One command off the panel's queue: `raw` realtime bytes ('?', '!',
    // '~', 0x18, 0x85) or a line command.
    void command(bool raw, const char *text, LineSink sink, void *ctx);

    // Advances motion and jobs, and reports status every REPORT_MS.
    void tick(LineSink sink, void *ctx);

    // The listing of `dir` ("" is the root) as FluidNC's GET /upload JSON.
    String listJson(const char *dir) const;

private:
    enum class State : uint8_t { Idle, Jog, Run, Hold, Home, Alarm };

    static const uint32_t REPORT_MS = 100;       // matches $Report/Interval=100
    static const uint32_t DEMO_JOB_MS = 45000;   // long enough to watch, short enough to wait for
    static const uint32_t HOMING_MS = 3000;

    State state_ = State::Idle;
    State heldFrom_ = State::Idle; // what Hold resumes to
    float mpos_[3] = {0, 0, 0};
    float wco_[3] = {0, 0, 0};

    // Point-to-point motion (jogs, G0/G1, homing).
    bool moving_ = false;
    float target_[3] = {0, 0, 0};
    float feedMmMin_ = 0;

    // Streamed G-code: moves waiting their turn, like FluidNC's planner.
    // Lines that arrive while it's full wait in the inbox, unanswered,
    // which is what makes a streamer wait for its "ok".
    struct Move
    {
        float target[3];
        float feed;
    };
    static const int PLAN_DEPTH = 16;
    Move plan_[PLAN_DEPTH];
    int planHead_ = 0, planCount_ = 0;
    static const int INBOX_DEPTH = 24;
    static const int INBOX_LINE = 96;
    char inbox_[INBOX_DEPTH][INBOX_LINE];
    int inHead_ = 0, inCount_ = 0;
    float modalFeed_ = 0; // F is modal in G-code: it carries over to later lines

    // The running SD job.
    char jobPath_[256] = "";
    uint32_t jobElapsedMs_ = 0; // excludes time spent in Hold
    float jobStartPos_[3] = {0, 0, 0}; // where the head was when the job began

    uint32_t lastTickMs_ = 0;
    uint32_t lastReportMs_ = 0;
    uint32_t holdStartedMs_ = 0;

    // Bit per entry of the canned SD card: set once deleted.
    uint32_t deleted_ = 0;

    void report(LineSink sink, void *ctx);
    void say(LineSink sink, void *ctx, const char *text);
    void startMove(const float target[3], float feed);
    bool lineCommand(const char *text, LineSink sink, void *ctx); // false = rejected
    void gcodeMove(const char *text);
    void answer(const char *text, LineSink sink, void *ctx);
    void pumpInbox(LineSink sink, void *ctx);
    bool nextPlannedMove();
    void clearQueues();
    void deleteFile(const char *path);
};
