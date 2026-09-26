#include "wifi_manager.h"
#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include "../config/settings.h"
#include "branding.h"
#include "fluidnc_client.h"
#include "terrapixel_client.h"

namespace
{
    bool ready_ = false;

    void beginWifi(const char *ssid, const char *pass)
    {
        WiFi.mode(WIFI_STA);
        WiFi.setSleep(false); // matches terraPixel's note: WiFi sleep hurts responsiveness of the status stream
        // No network configured (first boot, or Settings > Wi-Fi > Forget):
        // stay in STA mode, which the network scan needs, but don't ask the
        // radio to join a network with no name.
        if (!ssid || ssid[0] == '\0')
        {
            Serial.println("[wifi] no SSID configured, not connecting");
            return;
        }
        WiFi.begin(ssid, pass);
        Serial.println("[wifi] WiFi.begin() issued, connecting in background");
    }
}

namespace WifiManager
{
    void begin()
    {
        beginWifi(Config::get().wifiSsid, Config::get().wifiPass);
    }

    void update()
    {
        if (ready_ || WiFi.status() != WL_CONNECTED) return;

        Serial.print("[wifi] connected, IP ");
        Serial.println(WiFi.localIP());
        // Shared by both clients below -- see Branding::mdnsHostname().
        MDNS.begin(Branding::mdnsHostname());
        fluidNC.begin();
        terraPixel.begin();
        ready_ = true;
    }

    bool isReady() { return ready_; }

    void reconnect(const char *ssid, const char *pass)
    {
        ready_ = false;
        WiFi.disconnect(true);
        beginWifi(ssid, pass);
    }
}
