#include "terrapixel_client.h"
#include <WiFi.h>
#include <ESPmDNS.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <string.h>
#include "../config/settings.h"
#include "host_spec.h"
#include "demo_mode.h"

TerraPixelClient terraPixel;

void TerraPixelClient::begin()
{
    // mDNS is started once in main.cpp (shared with FluidNCClient) before
    // this is used -- nothing to do here.
}

bool TerraPixelClient::ensureResolved()
{
    if (haveIp_) return true;

    uint32_t now = millis();
    if (now - lastResolveAttempt_ < 3000) return false;
    lastResolveAttempt_ = now;

    HostSpec spec;
    if (!hostSpecParse(Config::get().terraPixelHost, spec))
    {
        status_.link = TerraPixelLink::NotFound;
        return false;
    }
    IPAddress ip = hostSpecResolve(spec, 1500);
    if (ip == IPAddress((uint32_t)0))
    {
        status_.link = TerraPixelLink::NotFound;
        return false;
    }

    resolvedIp_ = ip;
    resolvedPort_ = spec.port ? spec.port : 80;
    haveIp_ = true;
    Serial.printf("[terrapixel] %s -> %s:%u\n", Config::get().terraPixelHost, ip.toString().c_str(), resolvedPort_);
    return true;
}

String TerraPixelClient::baseUrl()
{
    return "http://" + resolvedIp_.toString() + ":" + String(resolvedPort_);
}

bool TerraPixelClient::refreshStatusNow()
{
    if (WiFi.status() != WL_CONNECTED) return false;
    if (!ensureResolved())
    {
        status_.reachable = false;
        return false;
    }

    HTTPClient http;
    http.setConnectTimeout(REQUEST_TIMEOUT_MS);
    http.setTimeout(REQUEST_TIMEOUT_MS);
    if (!http.begin(baseUrl() + "/status"))
    {
        status_.reachable = false;
        status_.link = TerraPixelLink::NotFound;
        return false;
    }

    // A negative code is the transport failing -- nothing answered. Any
    // HTTP status at all means something is listening there, and anything
    // but 200 with terraPixel's JSON means it isn't terraPixel: most likely
    // the plotter itself, whose host is easy to type here by mistake.
    int code = http.GET();
    if (code != 200)
    {
        http.end();
        status_.reachable = false;
        status_.link = code < 0 ? TerraPixelLink::NotFound : TerraPixelLink::WrongDevice;
        haveIp_ = false; // in case terraPixel's IP changed, re-resolve next time
        return false;
    }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, http.getStream());
    http.end();
    // A 200 that isn't a JSON object with any of the fields read below (a
    // web page, someone else's JSON) is another device. Any one of them
    // will do: older terraPixel builds don't send every field.
    bool looksLikeTerraPixel = doc["mode"].is<const char *>() || doc["brightness"].is<int>() ||
                               doc["filmMode"].is<bool>() || doc["party"].is<bool>();
    if (err || !looksLikeTerraPixel)
    {
        status_.reachable = false;
        status_.link = TerraPixelLink::WrongDevice;
        return false;
    }

    status_.filmMode = doc["filmMode"] | false;
    status_.brightness = doc["brightness"] | status_.brightness;
    status_.radius = doc["radius"] | status_.radius;
    status_.party = doc["party"] | false;
    const char *mode = doc["mode"] | "IDLE";
    strncpy(status_.mode, mode, sizeof(status_.mode) - 1);
    status_.mode[sizeof(status_.mode) - 1] = '\0';
    status_.reachable = true;
    status_.link = TerraPixelLink::Connected;

    // The UI's controls start from whatever terraPixel already had, rather
    // than from this firmware's defaults -- otherwise the first slider touch
    // would push our guesses onto a device that was already configured.
    if (!seededDesired_)
    {
        desiredFilm_ = status_.filmMode;
        desiredBrightness_ = status_.brightness;
        desiredRadius_ = status_.radius;
        seededDesired_ = true;
    }
    return true;
}

bool TerraPixelClient::postForm(const String &path, const String &body)
{
    if (WiFi.status() != WL_CONNECTED) return false;
    if (!ensureResolved()) return false;

    HTTPClient http;
    http.setConnectTimeout(REQUEST_TIMEOUT_MS);
    http.setTimeout(REQUEST_TIMEOUT_MS);
    http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS); // /set replies 303; we only care it was accepted
    if (!http.begin(baseUrl() + path)) return false;
    http.addHeader("Content-Type", "application/x-www-form-urlencoded");

    int code = http.POST(body);
    http.end();

    if (code < 200 || code >= 400)
    {
        haveIp_ = false;
        return false;
    }
    return true;
}

bool TerraPixelClient::applySetNow()
{
    // terraPixel's /set treats "film" as an HTML checkbox: its mere presence
    // means on, and -- critically -- its ABSENCE always resets filmMode to
    // false (server.hasArg("film") is unconditional, unlike bright/radius
    // which are only touched when present). So every /set must carry all
    // three fields or it silently clobbers the others.
    bool film = desiredFilm_;
    uint8_t bright = desiredBrightness_;
    float radius = desiredRadius_;

    String body = "bright=" + String(bright) + "&radius=" + String(radius, 1);
    if (film) body += "&film=on";
    if (!postForm("/set", body)) return false;

    status_.filmMode = film;
    status_.brightness = bright;
    status_.radius = radius;
    return true;
}

bool TerraPixelClient::togglePartyNow()
{
    if (WiFi.status() != WL_CONNECTED) return false;
    if (!ensureResolved()) return false;

    HTTPClient http;
    http.setConnectTimeout(REQUEST_TIMEOUT_MS);
    http.setTimeout(REQUEST_TIMEOUT_MS);
    if (!http.begin(baseUrl() + "/party")) return false;

    int code = http.POST("");
    if (code != 200)
    {
        http.end();
        haveIp_ = false;
        return false;
    }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, http.getStream());
    http.end();
    if (err) return false;

    status_.party = doc["party"] | status_.party;
    return true;
}

// ---- UI-thread side: record intent only, never touch the network ----

void TerraPixelClient::setFilmMode(bool on)
{
    desiredFilm_ = on;
    setDirty_ = true;
}

void TerraPixelClient::setBrightness(uint8_t value)
{
    desiredBrightness_ = value;
    setDirty_ = true;
}

void TerraPixelClient::setRadius(float radiusLeds)
{
    desiredRadius_ = radiusLeds;
    setDirty_ = true;
}

void TerraPixelClient::toggleParty() { partyPending_ = true; }
void TerraPixelClient::requestRefresh() { refreshPending_ = true; }

// ---- lightsTask side: everything that can block ----

// Demo mode: the settings the Lights screen asks for are simply taken as
// the lights' state, which is all the screen can see of the real ones.
void TerraPixelClient::demoUpdate()
{
    if (setDirty_)
    {
        setDirty_ = false;
        status_.filmMode = desiredFilm_;
        status_.brightness = desiredBrightness_;
        status_.radius = desiredRadius_;
    }
    if (partyPending_)
    {
        partyPending_ = false;
        status_.party = !status_.party;
    }
    refreshPending_ = false;
    const char *mode = status_.party ? "PARTY" : status_.filmMode ? "FILM" : "FOLLOW";
    strncpy(status_.mode, mode, sizeof(status_.mode) - 1);
    status_.mode[sizeof(status_.mode) - 1] = '\0';
}

void TerraPixelClient::update()
{
    bool demo = Demo::isOn();
    if (demo != demoActive_)
    {
        demoActive_ = demo;
        status_ = TerraPixelStatus();
        if (demo)
        {
            status_.link = TerraPixelLink::Connected;
            status_.reachable = true;
            status_.filmMode = desiredFilm_;
            status_.brightness = desiredBrightness_;
            status_.radius = desiredRadius_;
        }
        else
        {
            refreshPending_ = true; // read the real lights back
        }
        return;
    }
    if (demoActive_)
    {
        demoUpdate();
        return;
    }

    // Switched off: forget everything, so switching back on starts clean
    // rather than showing a stale "Connected" -- and contact nothing, not
    // even the mDNS lookup a missing terraPixel would cost every 3s.
    if (!Config::get().terraPixelEnabled)
    {
        if (status_.link != TerraPixelLink::Off)
        {
            status_ = TerraPixelStatus();
            haveIp_ = false;
            seededDesired_ = false;
            setDirty_ = false;
            partyPending_ = false;
        }
        return;
    }
    if (status_.link == TerraPixelLink::Off)
    {
        status_.link = TerraPixelLink::Searching;
        lastResolveAttempt_ = 0;
        refreshPending_ = true;
    }

    if (WiFi.status() != WL_CONNECTED) return;

    // Settings first, so a slider drag lands before the next status poll
    // reads back a stale value.
    if (setDirty_)
    {
        setDirty_ = false;
        applySetNow();
    }

    if (partyPending_)
    {
        partyPending_ = false;
        togglePartyNow();
    }

    uint32_t now = millis();
    if (refreshPending_ || now - lastRefreshAt_ >= REFRESH_MS)
    {
        refreshPending_ = false;
        lastRefreshAt_ = now;
        refreshStatusNow();
    }
}
