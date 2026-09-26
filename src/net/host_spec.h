#pragma once
#include <Arduino.h>
#include <ESPmDNS.h>
#include <IPAddress.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

// Turns a host as a person would type it into the Settings screen into
// something to connect to. Accepts all of:
//
//   fluidnc397          bare mDNS name (the original, and only, format)
//   fluidnc397.local    the same name as it appears in a browser
//   192.168.1.40        a plain IP -- no mDNS involved
//   ws://192.168.1.40:80/   a pasted URL, with or without scheme/port/path
//
// Only accepting the bare name was a trap: ".local" got looked up as
// "fluidnc397.local.local", and an IP was sent to mDNS as if it were a
// name, so every natural way of typing the machine's address failed with
// nothing on screen to say why.
struct HostSpec
{
    char name[64] = "";  // mDNS name without ".local", or the IP as text
    uint16_t port = 0;   // 0 = caller's default
    bool isIp = false;
    IPAddress ip;        // valid when isIp
};

inline bool hostSpecParse(const char *in, HostSpec &out)
{
    out = HostSpec();
    if (!in) return false;
    while (*in == ' ') in++;

    // Scheme, if pasted as a URL.
    const char *sep = strstr(in, "://");
    if (sep) in = sep + 3;

    // host[:port][/path]
    size_t n = 0;
    while (in[n] && in[n] != ':' && in[n] != '/' && in[n] != ' ' && n < sizeof(out.name) - 1)
    {
        out.name[n] = in[n];
        n++;
    }
    out.name[n] = '\0';
    if (in[n] == ':')
    {
        long p = strtol(in + n + 1, nullptr, 10);
        if (p > 0 && p <= 65535) out.port = (uint16_t)p;
    }

    // Trailing dot, then ".local": mDNS lookups take the bare name.
    if (n > 0 && out.name[n - 1] == '.') out.name[--n] = '\0';
    if (n > 6 && strcasecmp(out.name + n - 6, ".local") == 0) out.name[n - 6] = '\0';

    if (out.name[0] == '\0') return false;
    out.isIp = out.ip.fromString(out.name);
    return true;
}

// The address to connect to: the IP as given, or an mDNS lookup of the
// name. Returns 0.0.0.0 on failure. Blocks for up to timeoutMs on a name.
inline IPAddress hostSpecResolve(const HostSpec &spec, uint32_t timeoutMs)
{
    if (spec.isIp) return spec.ip;
    return MDNS.queryHost(spec.name, timeoutMs);
}
