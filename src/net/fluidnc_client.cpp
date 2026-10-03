#include "fluidnc_client.h"
#include <WiFi.h>
#include <ESPmDNS.h>
#include <WebSocketsClient.h>
#include <HTTPClient.h>
#include <string.h>
#include "../config/settings.h"
#include "host_spec.h"
#include "demo_mode.h"

FluidNCClient fluidNC;

static WebSocketsClient wsClient;

// WebSocketsClient's onEvent wants a plain function pointer -- it has no
// notion of a bound member function, so this trampoline forwards to the
// (single) FluidNCClient instance.
static void wsEventTrampoline(WStype_t type, uint8_t *payload, size_t length)
{
    fluidNC.onWsEvent((uint8_t)type, payload, length);
}

void FluidNCClient::initTransport()
{
    if (!cmdQueue_) cmdQueue_ = xQueueCreate(CMD_QUEUE_DEPTH, sizeof(OutCmd));
    if (!fileMutex_) fileMutex_ = xSemaphoreCreateMutex();
}

void FluidNCClient::begin()
{
    // mDNS is started once in main.cpp (shared with TerraPixelClient)
    // before this is used -- nothing to do here.
}

bool FluidNCClient::enqueue(bool raw, const char *text, uint32_t ticket)
{
    if (!cmdQueue_) return false; // initTransport() not called yet -- nothing to do but drop
    if (strlen(text) > MAX_COMMAND_LEN)
    {
        // Never send a truncated command -- see MAX_COMMAND_LEN.
        Serial.printf("[fluidnc] command too long for FluidNC (%u chars), not sent: %.60s...\n",
                      (unsigned)strlen(text), text);
        return false;
    }
    OutCmd cmd;
    cmd.raw = raw;
    cmd.ticket = ticket;
    strcpy(cmd.text, text);
    // Never block the UI task waiting for queue space: if the network task
    // is wedged behind a slow socket, dropping a jog/status command is far
    // better than freezing the display until it recovers.
    if (xQueueSend(cmdQueue_, &cmd, 0) != pdTRUE)
    {
        Serial.printf("[fluidnc] command queue full, dropped: %s\n", text);
        return false;
    }
    return true;
}

void FluidNCClient::drainCommandQueue()
{
    if (!cmdQueue_) return;
    OutCmd cmd;
    while (xQueueReceive(cmdQueue_, &cmd, 0) == pdTRUE)
    {
        if (cmd.raw) sendRaw(cmd.text);
        else
        {
            bool sent = sendLine(String(cmd.text));
            if (cmd.ticket) trackSent(cmd.ticket, sent);
        }
    }
}

// Called once a tracked line has been handed to the transport (or failed
// to be) -- after sendLine() has counted it, and before its reply can be
// read, which only happens later on this same task (or, in demo, inside
// the sim call that follows).
void FluidNCClient::trackSent(uint32_t ticket, bool sent)
{
    if (trackedPending_) publishAck(trackedTicket_, AckState::Lost); // superseded
    trackedPending_ = false;
    if (!sent)
    {
        publishAck(ticket, AckState::Lost);
        return;
    }
    trackedTicket_ = ticket;
    trackedAckIndex_ = linesSent_;
    trackedPending_ = true;
    publishAck(ticket, AckState::Pending);
}

void FluidNCClient::noteAck(bool ok)
{
    acksReceived_++;
    if (trackedPending_ && acksReceived_ == trackedAckIndex_)
    {
        trackedPending_ = false;
        publishAck(trackedTicket_, ok ? AckState::Ok : AckState::Error);
    }
}

// A new channel (or the sim) starts its replies from scratch, and nothing
// still outstanding on the old one will ever be answered.
void FluidNCClient::resetAcks()
{
    linesSent_ = 0;
    acksReceived_ = 0;
    if (trackedPending_) publishAck(trackedTicket_, AckState::Lost);
    trackedPending_ = false;
}

uint32_t FluidNCClient::sendGcodeLineTracked(const char *line)
{
    // 30 bits, never 0. A wrap would need a billion tracked lines.
    nextTicket_ = (nextTicket_ + 1) & 0x3FFFFFFF;
    if (!nextTicket_) nextTicket_ = 1;
    return enqueue(false, line, nextTicket_) ? nextTicket_ : 0;
}

FluidNCClient::AckState FluidNCClient::ackState(uint32_t ticket) const
{
    uint32_t word = trackedAck_;
    uint32_t published = word >> 2;
    if (published == ticket) return (AckState)(word & 3);
    // Not reached the transport yet, or already superseded by a later one.
    return published < ticket ? AckState::Pending : AckState::Lost;
}

bool FluidNCClient::fileListReady() const
{
    if (!fileMutex_) return false;
    bool v = false;
    if (xSemaphoreTake(fileMutex_, pdMS_TO_TICKS(5)) == pdTRUE)
    {
        v = sdFiles_.ready();
        xSemaphoreGive(fileMutex_);
    }
    return v;
}

void FluidNCClient::clearFileListReady()
{
    if (!fileMutex_) return;
    if (xSemaphoreTake(fileMutex_, pdMS_TO_TICKS(5)) == pdTRUE)
    {
        sdFiles_.clearReady();
        xSemaphoreGive(fileMutex_);
    }
}

bool FluidNCClient::fileListFailed() const
{
    if (!fileMutex_) return false;
    bool v = false;
    if (xSemaphoreTake(fileMutex_, pdMS_TO_TICKS(5)) == pdTRUE)
    {
        v = sdFiles_.failed();
        xSemaphoreGive(fileMutex_);
    }
    return v;
}

void FluidNCClient::fileListDir(char *out, size_t outSize) const
{
    if (!outSize) return;
    out[0] = '\0';
    if (!fileMutex_) return;
    if (xSemaphoreTake(fileMutex_, pdMS_TO_TICKS(5)) == pdTRUE)
    {
        strncpy(out, sdFiles_.dir(), outSize - 1);
        out[outSize - 1] = '\0';
        xSemaphoreGive(fileMutex_);
    }
}

int FluidNCClient::fileListCount() const
{
    if (!fileMutex_) return 0;
    int n = 0;
    if (xSemaphoreTake(fileMutex_, pdMS_TO_TICKS(5)) == pdTRUE)
    {
        n = sdFiles_.count();
        xSemaphoreGive(fileMutex_);
    }
    return n;
}

bool FluidNCClient::fileListEntry(int i, FluidNCFileEntry &out) const
{
    if (!fileMutex_) return false;
    bool ok = false;
    if (xSemaphoreTake(fileMutex_, pdMS_TO_TICKS(5)) == pdTRUE)
    {
        if (i >= 0 && i < sdFiles_.count())
        {
            out = sdFiles_.entry(i);
            ok = true;
        }
        xSemaphoreGive(fileMutex_);
    }
    return ok;
}

bool FluidNCClient::resolveHost()
{
    HostSpec spec;
    if (!hostSpecParse(Config::get().fluidNcHost, spec))
    {
        Serial.println("[fluidnc] no FluidNC host set");
        return false;
    }
    IPAddress ip = hostSpecResolve(spec, 2000);
    if (ip == IPAddress((uint32_t)0))
    {
        Serial.printf("[fluidnc] mDNS lookup for %s.local failed, retrying\n", spec.name);
        return false;
    }
    resolvedIp_ = ip;
    httpPort_ = spec.port ? spec.port : 80;
    return true;
}

// Asks FluidNC which port its WebSocket is on, the way terraForge does
// (theworkisthework/terraForge, src/machine/fluidnc.ts). The two firmware
// generations differ:
//
//   4.x  the socket is on the HTTP port (80), mounted at "/"
//   3.x  the socket is a separate server on port 81; port 80 answers a
//        WebSocket upgrade with a plain HTTP page, so the socket never opens
//
// [ESP800] is the ESP3D-compatible "who are you" command both generations
// answer over HTTP, e.g.
//   FW version: FluidNC v4.0.1 # ... # webcommunication: Sync: 80 # ...
// and its "webcommunication: Sync: <port>" field names the socket port
// outright. Returns 0 if the probe gets no usable answer.
uint16_t FluidNCClient::probeWsPort()
{
    HTTPClient http;
    http.setTimeout(3000);
    http.setConnectTimeout(3000);
    char url[80];
    snprintf(url, sizeof(url), "http://%s:%u/command?plain=%%5BESP800%%5D",
             resolvedIp_.toString().c_str(), httpPort_);
    if (!http.begin(url)) return 0;
    int code = http.GET();
    String body = code == HTTP_CODE_OK ? http.getString() : String();
    http.end();
    if (body.length() == 0)
    {
        Serial.printf("[fluidnc] firmware probe failed (HTTP %d)\n", code);
        return 0;
    }

    int major = -1;
    int fw = body.indexOf("FW version");
    if (fw >= 0)
    {
        // First digit after "FW version:", skipping "FluidNC v".
        int i = fw + 10;
        while (i < (int)body.length() && !isdigit((unsigned char)body[i])) i++;
        if (i < (int)body.length()) major = atoi(body.c_str() + i);
        int end = body.indexOf('#', fw);
        Serial.printf("[fluidnc] firmware: %s\n", body.substring(fw, end > fw ? end : fw + 40).c_str());
    }

    int wc = body.indexOf("webcommunication");
    if (wc >= 0)
    {
        int sync = body.indexOf("Sync:", wc);
        int end = body.indexOf('#', wc);
        if (sync >= 0 && (end < 0 || sync < end))
        {
            long port = strtol(body.c_str() + sync + 5, nullptr, 10);
            if (port > 0 && port <= 65535) return (uint16_t)port;
        }
    }
    if (major >= 4) return httpPort_;
    if (major >= 0) return 81;
    Serial.println("[fluidnc] firmware probe: no version in the response");
    return 0;
}

void FluidNCClient::openSocket()
{
    Serial.printf("[fluidnc] %s -> ws://%s:%u/%s\n", Config::get().fluidNcHost,
                  resolvedIp_.toString().c_str(), wsPort_, wsPortGuessed_ ? " (port guessed)" : "");
    wsClient.begin(resolvedIp_.toString().c_str(), wsPort_, "/");
    wsClient.onEvent(wsEventTrampoline);
    wsClient.setReconnectInterval(3000);
    wsBeganAt_ = millis();
    wsBegun_ = true;
}

void FluidNCClient::enterDemo()
{
    // Close the real channel first: in demo nothing may reach the machine.
    if (wsBegun_)
    {
        wsClient.disconnect();
        wsBegun_ = false;
    }
    // A clean slate -- including over the "websocket dropped" the
    // disconnect above just recorded, which isn't a fault worth showing.
    status_ = FluidNCStatus();
    status_.connected = true;
    lineLen_ = 0;
    homePending_ = false;
    resetAcks();
    sim_.reset();
    demoActive_ = true;
    fileListRequested_ = true; // whatever folder Jobs is in, from the demo card
    Serial.println("[fluidnc] demo mode on: machine connection closed, nothing is sent");
}

void FluidNCClient::leaveDemo()
{
    status_ = FluidNCStatus(); // disconnected, until the real machine answers
    lineLen_ = 0;
    homePending_ = false;
    resetAcks();
    demoActive_ = false;
    lastResolveAttempt_ = 0; // reconnect straight away
    // Replace the demo card's listing with an empty one, so no demo entry
    // is left on screen to run against the real machine; the next fetch
    // brings the real card back.
    if (fileMutex_ && xSemaphoreTake(fileMutex_, pdMS_TO_TICKS(50)) == pdTRUE)
    {
        sdFiles_.setResponse("{\"files\":[]}", fileListDir_);
        xSemaphoreGive(fileMutex_);
    }
    fileListRequested_ = true;
    Serial.println("[fluidnc] demo mode off: reconnecting to the machine");
}

void FluidNCClient::simSink(void *ctx, char *line)
{
    static_cast<FluidNCClient *>(ctx)->handleLine(line);
}

void FluidNCClient::demoUpdate()
{
    servicePendingHome(); // may enqueue, so before the drain
    OutCmd cmd;
    while (cmdQueue_ && xQueueReceive(cmdQueue_, &cmd, 0) == pdTRUE)
    {
        // Counted before the sim runs it: the sim replies synchronously,
        // from inside command().
        if (!cmd.raw)
        {
            linesSent_++;
            if (cmd.ticket) trackSent(cmd.ticket, true);
        }
        sim_.command(cmd.raw, cmd.text, simSink, this);
    }
    sim_.tick(simSink, this);

    if (fileListRequested_)
    {
        fileListRequested_ = false;
        char dir[SD_DIR_MAX] = "";
        if (fileMutex_ && xSemaphoreTake(fileMutex_, pdMS_TO_TICKS(50)) == pdTRUE)
        {
            strcpy(dir, fileListDir_);
            xSemaphoreGive(fileMutex_);
        }
        String json = sim_.listJson(dir);
        if (fileMutex_ && xSemaphoreTake(fileMutex_, pdMS_TO_TICKS(50)) == pdTRUE)
        {
            sdFiles_.setResponse(json, dir);
            xSemaphoreGive(fileMutex_);
        }
    }
}

void FluidNCClient::update()
{
    // Demo mode, and the switches into and out of it, come before anything
    // network-related: the simulation needs no Wi-Fi at all.
    bool demo = Demo::isOn();
    if (demo != demoActive_)
    {
        if (demo) enterDemo();
        else leaveDemo();
        return;
    }
    if (demoActive_)
    {
        demoUpdate();
        return;
    }

    if (WiFi.status() != WL_CONNECTED) return;

    if (hostChanged_)
    {
        hostChanged_ = false;
        if (wsBegun_)
        {
            Serial.println("[fluidnc] host changed, reconnecting");
            wsClient.disconnect();
            wsBegun_ = false;
        }
        lastResolveAttempt_ = 0; // try the new host straight away
    }

    if (!wsBegun_)
    {
        uint32_t now = millis();
        if (now - lastResolveAttempt_ < 3000) return;
        lastResolveAttempt_ = now;

        if (!resolveHost()) return;

        uint16_t port = probeWsPort();
        wsPortGuessed_ = (port == 0);
        // Unknown firmware: start on the HTTP port (4.x, the current
        // release) and let the fallback below try 81.
        wsPort_ = port ? port : httpPort_;
        openSocket();
        return;
    }

    // Couldn't tell which firmware this is: if the socket hasn't opened
    // after a while, try the other generation's port. terraForge does the
    // same on seeing 3.x's HTTP reply to an upgrade; WebSocketsClient
    // doesn't surface that reply, so a timeout stands in for it. Once a
    // port connects it's kept.
    if (wsPortGuessed_ && !status_.connected && millis() - wsBeganAt_ > 10000)
    {
        wsPort_ = (wsPort_ == 81) ? httpPort_ : 81;
        Serial.printf("[fluidnc] no connection yet, trying port %u\n", wsPort_);
        wsClient.disconnect();
        openSocket();
        return;
    }
    if (status_.connected) wsPortGuessed_ = false;

    servicePendingHome(); // may enqueue, so before the drain
    drainCommandQueue();
    wsClient.loop();
    if (fileListRequested_)
    {
        fileListRequested_ = false;
        fetchFileList();
    }
}

// Blocks the network task for the length of one HTTP request, which is
// what this task is for (see the class comment). The timeout is generous
// because FluidNC can be slow on the first SD access after boot.
void FluidNCClient::fetchFileList()
{
    char dir[SD_DIR_MAX] = "";
    if (fileMutex_ && xSemaphoreTake(fileMutex_, pdMS_TO_TICKS(50)) == pdTRUE)
    {
        strcpy(dir, fileListDir_);
        xSemaphoreGive(fileMutex_);
    }

    // Percent-encode each byte of the folder path, but leave the slashes
    // between segments alone: FluidNC 3.x resets the connection when it
    // sees %2F in this parameter (terraForge's finding); 4.x takes either.
    String url = "http://" + resolvedIp_.toString() + ":" + String(httpPort_) + "/upload?path=/";
    static const char HEX_DIGITS[] = "0123456789ABCDEF";
    for (const char *p = dir; *p; p++)
    {
        unsigned char c = (unsigned char)*p;
        if (isalnum(c) || c == '/' || c == '-' || c == '_' || c == '.' || c == '~') url += (char)c;
        else
        {
            url += '%';
            url += HEX_DIGITS[c >> 4];
            url += HEX_DIGITS[c & 15];
        }
    }
    Serial.printf("[fluidnc] listing SD folder /%s\n", dir);

    HTTPClient http;
    http.setTimeout(10000);
    http.setConnectTimeout(3000);
    String body;
    int code = -1;
    if (http.begin(url))
    {
        code = http.GET();
        if (code == HTTP_CODE_OK) body = http.getString();
        http.end();
    }

    if (fileMutex_ && xSemaphoreTake(fileMutex_, pdMS_TO_TICKS(50)) == pdTRUE)
    {
        if (body.length()) sdFiles_.setResponse(body, dir);
        else
        {
            Serial.printf("[fluidnc] SD file list request failed (HTTP %d)\n", code);
            sdFiles_.fail(dir);
        }
        xSemaphoreGive(fileMutex_);
    }
}

void FluidNCClient::onWsEvent(uint8_t type, uint8_t *payload, size_t length)
{
    switch (type)
    {
        case WStype_CONNECTED:
            Serial.println("[fluidnc] websocket connected");
            status_.connected = true;
            lineLen_ = 0; // never glue a fresh connection onto a half-received line
            resetAcks();
            // Re-issued on every (re)connect -- auto-reporting is per-channel
            // and FluidNC forgets it across disconnects.
            sendLine("$Report/Interval=100");
            sendRaw("?");
            break;

        case WStype_DISCONNECTED:
            Serial.println("[fluidnc] websocket disconnected");
            status_.connected = false;
            status_.disconnects++;
            strncpy(status_.lastMessage, "websocket dropped", sizeof(status_.lastMessage) - 1);
            status_.lastMessageAt = millis();
            status_.lastFailure = true;
            status_.mode = MachineMode::Boot;
            status_.havePos = false;
            lineLen_ = 0;
            resetAcks();
            break;

        // A whole message ends a line even without a trailing newline:
        // FluidNC 4.x sends some messages ("currentID:1", "PING:...") as
        // bare frames, and running them together glued the next status
        // report onto the end of them, where it went unparsed.
        case WStype_TEXT:
        case WStype_BIN:
            ingest((const char *)payload, length);
            endLine();
            break;

        // A message too large for one frame arrives in fragments; the line
        // ends with the last one. Without these cases a fragmented message
        // was dropped outright.
        case WStype_FRAGMENT_TEXT_START:
        case WStype_FRAGMENT_BIN_START:
        case WStype_FRAGMENT:
            ingest((const char *)payload, length);
            break;
        case WStype_FRAGMENT_FIN:
            ingest((const char *)payload, length);
            endLine();
            break;

        default:
            break;
    }
}

void FluidNCClient::sendRaw(const char *s)
{
    if (!status_.connected) return;
    wsClient.sendTXT(s);
}

bool FluidNCClient::sendLine(const String &line)
{
    if (!status_.connected)
    {
        Serial.printf("[fluidnc] sendLine(\"%s\") dropped -- not connected\n", line.c_str());
        return false;
    }
    String out = line + "\n";
    if (!wsClient.sendTXT(out))
    {
        Serial.printf("[fluidnc] sendLine(\"%s\") dropped -- websocket write failed\n", line.c_str());
        return false;
    }
    // Counted only once the transport took it: every line that reaches
    // FluidNC gets exactly one ok/error back (see noteAck()), and one that
    // didn't must not leave the count waiting on a reply that won't come.
    linesSent_++;
    return true;
}

void FluidNCClient::endLine()
{
    if (!lineLen_) return;
    lineBuf_[lineLen_] = '\0';
    handleLine(lineBuf_);
    lineLen_ = 0;
}

void FluidNCClient::ingest(const char *data, size_t len)
{
    for (size_t i = 0; i < len; i++)
    {
        char c = data[i];
        if (c == '\n' || c == '\r')
        {
            endLine();
        }
        else if (lineLen_ < sizeof(lineBuf_) - 1)
        {
            lineBuf_[lineLen_++] = c;
        }
    }
}

void FluidNCClient::applyState(const char *state)
{
    MachineMode next = status_.mode;

    if      (!strncmp(state, "Run",   3)) next = MachineMode::Run;
    else if (!strncmp(state, "Jog",   3)) next = MachineMode::Run;
    else if (!strncmp(state, "Hold",  4)) next = MachineMode::Hold;
    else if (!strncmp(state, "Door",  4)) next = MachineMode::Hold;
    else if (!strncmp(state, "Alarm", 5)) next = MachineMode::Alarm;
    else if (!strncmp(state, "Home",  4)) next = MachineMode::Homing;
    else if (!strncmp(state, "Sleep", 5)) next = MachineMode::Idle;
    else if (!strncmp(state, "Idle",  4))
    {
        bool wasJob = (status_.mode == MachineMode::Run) && (millis() - runStartedAt_ > MIN_JOB_MS);
        if (wasJob) { next = MachineMode::Done; doneAt_ = millis(); }
        else if (status_.mode == MachineMode::Done && millis() - doneAt_ < CELEBRATE_MS) next = MachineMode::Done;
        else next = MachineMode::Idle;
    }

    if (next == MachineMode::Run && status_.mode != MachineMode::Run) runStartedAt_ = millis();
    if (next != MachineMode::Run && next != MachineMode::Hold) status_.jobActive = false;
    status_.mode = next;
}

// Everything FluidNC says goes to the serial log, but only some of it is
// worth putting on the alarm screen.
void FluidNCClient::noteMessage(const char *line)
{
    bool failure = !strncmp(line, "error:", 6) || !strncmp(line, "ALARM:", 6);

    if (!failure)
    {
        // A verbose FluidNC build narrates homing at about 25
        // [MSG:DBG:...] lines per cycle. Invaluable on the serial log,
        // fatal on the alarm screen: the chatter continues after an
        // aborted cycle, so an unfiltered "last message" would calmly
        // report "Homing done" over the ALARM that actually stopped you.
        if (!strncmp(line, "[MSG:DBG:", 9)) return;

        // Same reasoning, slower: don't let ordinary messages overwrite a
        // fresh failure before anyone has walked over to read it.
        if (status_.lastFailure && millis() - status_.lastMessageAt < FAILURE_STICKY_MS) return;
    }

    strncpy(status_.lastMessage, line, sizeof(status_.lastMessage) - 1);
    status_.lastMessage[sizeof(status_.lastMessage) - 1] = '\0';
    status_.lastMessageAt = millis();
    status_.lastFailure = failure;
}

void FluidNCClient::handleLine(char *line)
{
    // WebUI session bookkeeping FluidNC 4.x sends every WebSocket client
    // (the same list terraForge ignores). Not machine messages -- and
    // logging them let "PING" overwrite the last real message on the
    // alarm screen.
    if (!strncmp(line, "PING", 4) || !strncasecmp(line, "currentID:", 10) ||
        !strncasecmp(line, "activeID:", 9) || !strncmp(line, "CURRENT_ID:", 11) ||
        !strncmp(line, "ACTIVE_ID:", 10))
        return;

    // Everything that isn't a status report is a reply or a message: "ok",
    // "error:N", "ALARM:N" or an "[MSG:...]". ok/error are counted against
    // the lines sent (noteAck). Everything but ok is also the only place
    // FluidNC says *why* something failed, so it goes to the serial log
    // rather than being dropped silently.
    if (line[0] != '<')
    {
        bool ok = !strncmp(line, "ok", 2);
        if (ok || !strncmp(line, "error:", 6)) noteAck(ok);
        if (!ok)
        {
            Serial.printf("[fluidnc] %s\n", line);
            noteMessage(line);
        }
        return;
    }

    char *end = strpbrk(line + 1, "|>");
    if (!end) return;
    char saved = *end;
    *end = '\0';
    applyState(line + 1);
    *end = saved;

    static float wcoX = 0, wcoY = 0, wcoZ = 0;
    char *wco = strstr(end, "WCO:");
    if (wco) sscanf(wco + 4, "%f,%f,%f", &wcoX, &wcoY, &wcoZ);

    char *wpos = strstr(end, "WPos:");
    if (wpos)
    {
        sscanf(wpos + 5, "%f,%f,%f", &status_.wposX, &status_.wposY, &status_.wposZ);
        status_.havePos = true;
    }
    else
    {
        char *mpos = strstr(end, "MPos:");
        if (mpos)
        {
            float mx, my, mz;
            sscanf(mpos + 5, "%f,%f,%f", &mx, &my, &mz);
            status_.wposX = mx - wcoX;
            status_.wposY = my - wcoY;
            status_.wposZ = mz - wcoZ;
            status_.havePos = true;
        }
    }

    // Tentative: FluidNC's SD-job-percentage field. Confirm the exact
    // "SD:<pct>,<filename>" syntax against your firmware build -- if it's
    // absent or shaped differently, this just leaves jobPercent at -1.
    char *sd = strstr(end, "SD:");
    if (sd)
    {
        status_.jobPercent = atof(sd + 3);

        // FluidNC only reports SD: while it is actually running a file, so
        // its presence is proof of a job in a way mode==Run isn't (Run also
        // covers plain jogging). runFile() sets this too, but only for jobs
        // *we* started -- a job launched from the web UI or terraForge left
        // the pendant showing an empty ring at 0%, which is the whole point
        // of having a pendant missed. Gated on Run/Hold so a trailing SD:
        // in a post-job status report can't keep the flag raised.
        if (status_.mode == MachineMode::Run || status_.mode == MachineMode::Hold)
            status_.jobActive = true;
        char *comma = strchr(sd + 3, ',');
        if (comma)
        {
            comma++;
            size_t i = 0;
            while (*comma && *comma != '|' && *comma != '>' && i < sizeof(status_.jobFilename) - 1)
                status_.jobFilename[i++] = *comma++;
            status_.jobFilename[i] = '\0';
        }
    }
    else
    {
        status_.jobPercent = -1;
        status_.jobFilename[0] = '\0';
    }
}

// All of these are called from the UI task -- they only enqueue, so a
// stalled socket can never stall a button press (see the THREADING note in
// the header).
void FluidNCClient::requestStatus() { enqueue(true, "?"); }
void FluidNCClient::feedHold()      { enqueue(true, "!"); }
void FluidNCClient::resume()        { enqueue(true, "~"); }
void FluidNCClient::softReset()     { enqueue(true, "\x18"); }

// Homing used to be an unconditional "$X then $H" pair enqueued together,
// which meant both lines hit the websocket in the same drain, microseconds
// apart, whatever state the machine was in. That is the one thing the
// pendant did differently from the web UI -- and it broke exactly the case
// where the $X is pointless: homing a machine that is already Idle because
// it just homed. Unlocking pokes FluidNC's state machine, and a $H that
// arrives in the same breath can start a homing cycle that the pending
// unlock then aborts, which surfaces as an instant alarm. Homing once from
// a fresh (alarmed) boot always worked because there the $X had real work
// to do and was resolved before the $H landed.
//
// So: unlock only when there is an alarm to clear, and never in the same
// burst as the $H.
void FluidNCClient::home()
{
    // A second $H mid-cycle can only interfere with the one already
    // running -- the machine is going to the same place regardless.
    if (status_.mode == MachineMode::Homing) return;

    if (status_.mode == MachineMode::Alarm)
    {
        enqueue(false, "$X");
        homePending_ = true;
        homeRequestedAt_ = millis();
        return;
    }

    enqueue(false, "$H");
}

// networkTask only -- releases the $H held back by home().
void FluidNCClient::servicePendingHome()
{
    if (!homePending_) return;

    uint32_t waited = millis() - homeRequestedAt_;
    if (waited < UNLOCK_SETTLE_MS) return;
    if (status_.mode == MachineMode::Alarm && waited < UNLOCK_TIMEOUT_MS) return;

    if (status_.mode == MachineMode::Alarm)
        Serial.println("[fluidnc] still alarmed after $X, homing anyway -- watch for the error below");

    homePending_ = false;
    enqueue(false, "$H");
}
void FluidNCClient::clearAlarm() { enqueue(false, "$X"); }

void FluidNCClient::jog(char axis, float deltaMm, float feedrate)
{
    // A homing cycle is not something to jog out from under: 0x85 below is
    // a motion-cancel realtime byte, so sent mid-cycle it stops the homing
    // move, and FluidNC alarms on a cycle that didn't finish. The knob is
    // live on the Jog and Pen screens the whole time homing runs, so this
    // is one nudge away rather than hypothetical.
    if (status_.mode == MachineMode::Homing) return;

    // Jog cancel (GRBL/FluidNC realtime byte 0x85) first: without it,
    // FluidNC keeps running the previous jog move to completion before a
    // new $J= line takes effect, so alternating commands (knob jogging
    // back and forth, tapping Pen up then Pen down) felt laggy -- each new
    // press had to wait out whatever motion was still in flight. Cancelling
    // first lets a new jog interrupt the old one immediately.
    enqueue(true, "\x85");
    String cmd = "$J=G91 G21 ";
    cmd += axis;
    cmd += String(deltaMm, 3);
    cmd += " F";
    cmd += String(feedrate, 0);
    enqueue(false, cmd.c_str());
}

void FluidNCClient::requestFileList(const char *dir)
{
    if (fileMutex_ && xSemaphoreTake(fileMutex_, pdMS_TO_TICKS(5)) == pdTRUE)
    {
        strncpy(fileListDir_, dir ? dir : "", sizeof(fileListDir_) - 1);
        fileListDir_[sizeof(fileListDir_) - 1] = '\0';
        sdFiles_.beginCapture();
        xSemaphoreGive(fileMutex_);
    }
    // Fetched over HTTP by the network task -- see SdFileList for why not
    // `$SD/ListJSON` over the socket.
    fileListRequested_ = true;
}

bool FluidNCClient::runPathFits(const char *path)
{
    // "$SD/Delete=" is the longer of the two prefixes, but a file you can
    // run and not delete from the panel is fine; one you can see and not
    // run is what this guards.
    return strlen("$SD/Run=") + strlen(path) <= MAX_COMMAND_LEN;
}

bool FluidNCClient::runFile(const char *path)
{
    String cmd = "$SD/Run=";
    cmd += path;
    if (cmd.length() > MAX_COMMAND_LEN) return false;
    status_.jobActive = true;
    enqueue(false, cmd.c_str());
    return true;
}

bool FluidNCClient::deleteFile(const char *path)
{
    String cmd = "$SD/Delete=";
    cmd += path;
    if (cmd.length() > MAX_COMMAND_LEN) return false;
    enqueue(false, cmd.c_str());
    return true;
}

bool FluidNCClient::sendGcodeLine(const char *line) { return enqueue(false, line); }
