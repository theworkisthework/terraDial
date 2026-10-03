#pragma once

#include <Arduino.h>
#include <IPAddress.h>

// What the panel can tell about the terraPixel host it's pointed at, for
// Settings > Machine to show beside the field.
enum class TerraPixelLink : uint8_t
{
    Off,         // switched off in Settings -- nothing is contacted
    Searching,   // on, no answer yet either way
    NotFound,    // the name doesn't resolve, or nothing answers there
    WrongDevice, // something answers, but not with terraPixel's /status
    Connected,
};

// Snapshot of terraPixel's state, read from its /status endpoint (added to
// warderoid-ctrl/terraPixel alongside /party -- see that repo's main.cpp).
struct TerraPixelStatus
{
    TerraPixelLink link = TerraPixelLink::Off;
    bool reachable = false; // false until the first successful /status read
    bool filmMode = false;
    uint8_t brightness = 200;
    float radius = 3.5f;
    bool party = false;
    char mode[16] = "BOOT";
};

// Client for the terraPixel rail-light controller, which is a separate board
// on the network rather than part of this firmware. The two stay independent
// products: terraPixel keeps its own FluidNC connection for carriage
// position, so the lights work with no dial present at all.
//
// THREADING: every HTTP call here blocks for up to REQUEST_TIMEOUT_MS, plus
// an mDNS lookup if the host isn't resolved yet. update() therefore runs on
// main.cpp's lightsTask (core 0) -- NEVER on the UI loop, and not on
// networkTask either, where it used to stall the FluidNC connection.
//
// That split matters. These calls used to be made straight from the Lights
// screen's widget callbacks and from a 3-second poll that ran on every
// screen -- so with terraPixel switched off or off-network, the whole panel
// froze for ~1s at a time, which is a large part of what made the UI feel
// like it was "catching up" (see main.cpp's networkTask comment).
//
// The UI therefore never calls HTTP. The setters below only record intent
// and return immediately; lightsTask applies it and refreshes status(),
// which the UI reads as a plain struct.
class TerraPixelClient
{
public:
    // Call once mDNS is already up (see WifiManager).
    void begin();

    // lightsTask only. Applies whatever the UI asked for, and refreshes
    // status on its own schedule -- or, in demo mode (demo_mode.h), plays
    // pretend lights that take the settings without any network. Does
    // nothing at all while terraPixel is switched off in Settings, demo
    // aside.
    void update();
    bool inDemo() const { return demoActive_; }

    const TerraPixelStatus &status() const { return status_; }

    // -- UI-thread safe: record intent, applied by update() --
    // Film/brightness/radius coalesce into a single POST, so dragging a
    // slider can't queue up a burst of requests -- only the latest value is
    // ever sent, and terraPixel's /set takes all three together anyway.
    void setFilmMode(bool on);
    void setBrightness(uint8_t value);
    void setRadius(float radiusLeds);
    void toggleParty();
    void requestRefresh();
    // The LED host setting changed: resolve it again before the next
    // request rather than keep talking to the old address.
    void hostChanged() { haveIp_ = false; lastResolveAttempt_ = 0; refreshPending_ = true; }

private:
    TerraPixelStatus status_;
    bool demoActive_ = false;
    void demoUpdate();
    volatile bool haveIp_ = false;
    IPAddress resolvedIp_;
    uint16_t resolvedPort_ = 80;
    uint32_t lastResolveAttempt_ = 0;

    static const uint32_t REQUEST_TIMEOUT_MS = 1000;
    static const uint32_t REFRESH_MS = 3000;

    // Desired state, written by the UI task and consumed by lightsTask.
    // Plain volatile flags rather than a queue: these are last-writer-wins
    // values, not a sequence of events that must all be delivered.
    volatile bool setDirty_ = false;
    volatile bool partyPending_ = false;
    volatile bool refreshPending_ = false;
    volatile bool desiredFilm_ = false;
    volatile uint8_t desiredBrightness_ = 200;
    volatile float desiredRadius_ = 3.5f;

    bool seededDesired_ = false; // first successful /status seeds the desired values
    uint32_t lastRefreshAt_ = 0;

    bool ensureResolved();
    String baseUrl();
    bool postForm(const String &path, const String &body);
    bool refreshStatusNow();
    bool applySetNow();
    bool togglePartyNow();
};

extern TerraPixelClient terraPixel;
