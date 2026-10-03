#pragma once

#include <Arduino.h>
#include <IPAddress.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include "machine_mode.h"
#include "sd_file_list.h"
#include "machine_sim.h"

// Live state parsed out of FluidNC's realtime status reports
// (`<State|MPos:...|WCO:...|FS:...|SD:pct,filename>`). Field meanings and
// the WCO-latching approach are carried over from terraPixel's proven
// parser (warderoid-ctrl/terraPixel, src/main.cpp) -- same math, different
// transport (WebSocket here vs. telnet there).
struct FluidNCStatus
{
    bool connected = false;
    MachineMode mode = MachineMode::Boot;

    bool havePos = false;
    float wposX = 0, wposY = 0, wposZ = 0;

    // -1 when FluidNC isn't reporting an SD job percentage (e.g. not
    // running from SD, or this firmware build doesn't emit the field --
    // see the plan's note on verifying the `SD:` field against your
    // specific FluidNC version).
    float jobPercent = -1;
    // Scrolled rather than truncated on screen, so kept whole (FAT's
    // limit, which is also more than FluidNC can run -- see runPathFits()).
    char jobFilename[256] = {0};

    // True only for a real SD-file job started via runFile() -- distinct
    // from MachineMode::Run, which FluidNC also reports for plain jogging
    // ("Jog" state maps to Run too, see applyState()). ui_nav uses this to
    // gate its auto-navigation to/from Job Progress so a knob jog doesn't
    // get mistaken for a job starting.
    bool jobActive = false;

    // The last thing FluidNC said that wasn't a status report or a bare
    // "ok" -- an "error:N", an "ALARM:N", or an "[MSG:...]". Status reports
    // only carry the *state* (Alarm), never the reason, so without this an
    // intermittent fault is invisible to anyone without a serial cable
    // attached. Shown on the alarm screen.
    char lastMessage[56] = {0};
    uint32_t lastMessageAt = 0;

    // True when lastMessage is an actual failure ("error:N"/"ALARM:N" or a
    // dropped socket) rather than ordinary chatter. Failures are held for a
    // while so later chatter can't bury them -- see noteMessage().
    bool lastFailure = false;

    // Bumped on every websocket drop. A pendant that loses its channel
    // mid-cycle and silently reconnects looks, from the outside, exactly
    // like a machine that alarmed on its own -- this tells the two apart.
    uint16_t disconnects = 0;
};

// THREADING: update() does genuinely blocking work -- mDNS resolution
// (seconds), and WebSocket frame reads that spin until the rest of a
// partially-arrived frame shows up. It therefore runs on main.cpp's
// dedicated networkTask (core 0), NEVER on the UI/LVGL loop, which is what
// used to make the whole panel stutter whenever the plotter was connected.
//
// Everything else here is safe to call from the UI task: the command
// methods only enqueue text for networkTask to transmit (they never touch
// the WebSocket, which is not thread-safe), status() is a plain read of a
// struct networkTask updates, and the file-list accessors take a short
// mutex.
class FluidNCClient
{
public:
    // Call once from setup(), before networkTask starts -- creates the
    // command queue and file-list mutex so UI-thread callers always have
    // somewhere to put commands, even before WiFi is up.
    void initTransport();

    // Call once, after WiFi is connected.
    void begin();

    // UI-thread safe. The FluidNC host setting changed: drop the current
    // connection (if any) and resolve the new host on the next update().
    // Without this the first address found stuck until a reboot.
    void hostChanged() { hostChanged_ = true; }

    // networkTask only. Handles mDNS resolution, (re)connection, draining
    // the outbound command queue, and pumping received frames -- or, in
    // demo mode (demo_mode.h), runs the simulated machine instead. Demo
    // needs no network, so networkTask calls this regardless of Wi-Fi while
    // demo is on or being left (inDemo()).
    void update();
    bool inDemo() const { return demoActive_; }

    const FluidNCStatus &status() const { return status_; }

    // Realtime single-byte commands (sent immediately, no line buffering).
    void requestStatus(); // '?'
    void feedHold();      // '!'
    void resume();        // '~'
    void softReset();     // 0x18

    // Line commands.
    // home() is state-aware: it only unlocks first if the machine is
    // actually alarmed, and then holds the $H back until the unlock has
    // landed. See the comment on the definition for why.
    void home();                                       // $X (if alarmed) then $H
    void jog(char axis, float deltaMm, float feedrate); // $J=G91 G21 <axis><delta> F<feed>
    void clearAlarm();                                  // $X
    // `path` is relative to the SD root, e.g. "drawings/snowflake.gcode".
    // Both return false, and send nothing, if the command would be longer
    // than FluidNC accepts -- see runPathFits().
    bool runFile(const char *path);                     // $SD/Run=<path>
    bool deleteFile(const char *path);                  // $SD/Delete=<path>
    bool sendGcodeLine(const char *line);                // arbitrary line (pen macros, etc.); false if dropped

    // FluidNC reads a command into a 255-byte buffer (Channel::maxLine,
    // v4.0.3), so the longest line it accepts is 254 characters. A longer
    // one would be cut short and run or delete a different path -- or
    // nothing -- so it's refused instead.
    static const size_t MAX_COMMAND_LEN = 254;
    static bool runPathFits(const char *path);

    // SD file listing, one folder at a time. requestFileList() asks the
    // network task to fetch `dir` (relative to the SD root, no leading or
    // trailing slash; "" is the root) over HTTP -- see SdFileList. Poll
    // fileListReady() and call clearFileListReady() once you've read the
    // results via fileListCount()/fileListEntry(). Only files with a
    // recognized G-code extension are kept, plus folders.
    void requestFileList(const char *dir);
    bool fileListReady() const;
    void clearFileListReady();
    int fileListCount() const;
    // Whether the ready list came from a failed fetch rather than a folder
    // with nothing runnable in it.
    bool fileListFailed() const;
    // The folder the ready list is from (it can trail the latest request).
    void fileListDir(char *out, size_t outSize) const;
    // Copies entry i into out; returns false if i is out of range. Copies
    // rather than returning a reference because networkTask may rewrite the
    // underlying array the moment the mutex is released.
    bool fileListEntry(int i, FluidNCFileEntry &out) const;

    // Internal: bridges the C-style WebSocketsClient event callback back
    // into this instance. Public only because the trampoline needs it;
    // not part of the intended API surface.
    void onWsEvent(uint8_t type, uint8_t *payload, size_t length);

private:
    FluidNCStatus status_;

    // Demo mode: the simulated machine stands in for the WebSocket, and
    // its lines go through handleLine() like a real machine's.
    MachineSim sim_;
    bool demoActive_ = false;
    void enterDemo();
    void leaveDemo();
    void demoUpdate();
    static void simSink(void *ctx, char *line);

    bool wsBegun_ = false;
    volatile bool hostChanged_ = false; // set by the UI task, consumed by networkTask
    uint32_t lastResolveAttempt_ = 0;
    IPAddress resolvedIp_;
    // FluidNC's HTTP port (80 unless the host setting names another), and
    // the WebSocket port, which depends on the firmware: 4.x serves the
    // socket on the HTTP port, 3.x on a separate port 81. See
    // probeWsPort().
    uint16_t httpPort_ = 80;
    uint16_t wsPort_ = 80;
    // True when neither the probe nor a version heuristic could say which
    // port the socket is on. update() then alternates between the HTTP
    // port and 81 until one connects.
    bool wsPortGuessed_ = false;
    uint32_t wsBeganAt_ = 0;
    volatile bool fileListRequested_ = false; // set by the UI task, fetched by networkTask
    char fileListDir_[SD_DIR_MAX] = "";       // the folder to fetch; guarded by fileMutex_

    // Room for a status report carrying a full-length job path in its SD:
    // field, on top of the position and feed fields.
    char lineBuf_[512];
    size_t lineLen_ = 0;

    // Deferred half of home(): $X goes out immediately, $H waits for
    // FluidNC to actually leave Alarm (or for the timeout below).
    bool homePending_ = false;
    uint32_t homeRequestedAt_ = 0;
    static const uint32_t UNLOCK_SETTLE_MS = 150;
    static const uint32_t UNLOCK_TIMEOUT_MS = 2000;

    uint32_t runStartedAt_ = 0;
    uint32_t doneAt_ = 0;
    static const uint32_t MIN_JOB_MS = 5000;
    static const uint32_t CELEBRATE_MS = 12000;

    SdFileList sdFiles_;

    // Outbound command plumbing. UI-thread callers enqueue; networkTask
    // dequeues and transmits. `raw` distinguishes realtime single bytes
    // ('?', '!', 0x18, 0x85) from newline-terminated line commands.
    struct OutCmd
    {
        bool raw;
        char text[MAX_COMMAND_LEN + 1];
    };
    static const int CMD_QUEUE_DEPTH = 12;
    QueueHandle_t cmdQueue_ = nullptr;
    mutable SemaphoreHandle_t fileMutex_ = nullptr;

    // Filters what FluidNC says down to what's worth showing on a 240px
    // screen, and keeps failures from being buried by chatter.
    void noteMessage(const char *line);
    static const uint32_t FAILURE_STICKY_MS = 20000;

    // False if the command was dropped (too long, queue full, or no
    // transport yet) -- most callers can ignore it, but anything that acts
    // on a command having been sent (the park sequence's pen lift) can't.
    bool enqueue(bool raw, const char *text);
    void drainCommandQueue();
    void servicePendingHome();

    bool resolveHost();
    uint16_t probeWsPort();
    void openSocket();
    void fetchFileList();
    void endLine();
    void sendRaw(const char *s);
    void sendLine(const String &line);
    void ingest(const char *data, size_t len);
    void handleLine(char *line);
    void applyState(const char *state);
};

extern FluidNCClient fluidNC;
