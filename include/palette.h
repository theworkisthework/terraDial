#pragma once
#include <lvgl.h>

// terraForge's dark and light themes, ported 1:1 from that project's design tokens
// (theworkisthework/terraForge, src/renderer/src/index.css -- the `:root`
// block is dark/default; `:root.light` is the light theme). Values are copied exactly rather
// than re-derived so the plotter's desk panel and its desktop app read as
// one product: deep navy/sea-blue surfaces with a single hot red-magenta
// accent.
//
// This replaced the earlier "TerraPen Dial UI" mockup skin (a muted
// graphite-lavender scheme). The layout still follows that mockup -- only
// the colours changed.
//
// Exact fidelity depends on the real IPS panel, not a monitor -- nudge
// these after flashing if something looks off, rather than treating the hex
// values as gospel.
namespace Palette
{
    // Dark (the default) or light, picked once at boot from Settings >
    // Display (Config::get().lightTheme, applied in setup()). The UI builds
    // its styles from these functions as it is created, so changing theme
    // takes effect on the next boot -- the setting reboots the panel.
    inline bool &lightRef() { static bool light = false; return light; }
    inline bool isLight() { return lightRef(); }
    inline void setLight(bool light) { lightRef() = light; }

    // -- Surfaces, darkest to lightest --
    inline lv_color_t bgTerminal() { return isLight() ? lv_color_hex(0xf6f8fa) : lv_color_hex(0x0d1117); }  // --tf-bg-terminal, deepest (log/console style panes)
    inline lv_color_t bgApp() { return isLight() ? lv_color_hex(0xc9d1e6) : lv_color_hex(0x1a1a2e); }       // --tf-bg-app, the screen background
    inline lv_color_t bgPanel() { return isLight() ? lv_color_hex(0xffffff) : lv_color_hex(0x16213e); }     // --tf-bg-panel
    inline lv_color_t bgSecondary() { return isLight() ? lv_color_hex(0xdde4f5) : lv_color_hex(0x0f3460); } // --tf-bg-secondary, the "dark sea blue" raised surface: cards, chips, buttons
    inline lv_color_t bgSecondaryHover() { return isLight() ? lv_color_hex(0xc5d0ec) : lv_color_hex(0x1a4a8a); } // --tf-bg-secondary-hover

    // -- Lines and text --
    inline lv_color_t border() { return isLight() ? lv_color_hex(0x7584a8) : lv_color_hex(0x5c7a9e); }    // --tf-border
    inline lv_color_t text() { return isLight() ? lv_color_hex(0x1a1e2e) : lv_color_hex(0xe0e0e0); }      // --tf-text (slightly off pure white)
    inline lv_color_t textMuted() { return isLight() ? lv_color_hex(0x5a6080) : lv_color_hex(0x9ca3af); } // --tf-text-muted
    inline lv_color_t textFaint() { return isLight() ? lv_color_hex(0x5d6e88) : lv_color_hex(0x818ea5); } // --tf-text-faint

    // -- Accents --
    // The red-magenta that defines terraForge's look against the navy.
    inline lv_color_t accent() { return isLight() ? lv_color_hex(0xb82d45) : lv_color_hex(0xe94560); }      // --tf-accent
    inline lv_color_t accentHover() { return isLight() ? lv_color_hex(0x9a2539) : lv_color_hex(0xc73d56); } // --tf-accent-hover
    // Foreground for text/icons on accent() fills. The accent is a
    // saturated mid-tone red, so it needs light text on top (the previous
    // lavender-pastel accent needed the opposite -- dark text).
    inline lv_color_t accentFg() { return lv_color_hex(0xffffff); }
    // Informational secondary accent -- terraForge's file-browser blue.
    // Used where something needs to stand out WITHOUT competing with the
    // red (e.g. the "pen down" status pill).
    inline lv_color_t accentSecondary() { return isLight() ? lv_color_hex(0x1d4ed8) : lv_color_hex(0x60a0ff); } // --tf-fs-blue-text

    inline lv_color_t selectedFill() { return isLight() ? lv_color_hex(0xbfcff0) : lv_color_hex(0x1a3a6e); } // --tf-file-selected, dim blue selected-row fill

    // Deliberately a deeper, heavier red than accent(): E-Stop has to read
    // as an escalation, and the accent alone is too common elsewhere in the
    // UI (selected dial item, active chips) to carry that weight by itself.
    // Run / Done green and Hold amber: literal on CNC gear in either theme,
    // but the bright dark-theme values wash out on a light background.
    inline lv_color_t ok() { return isLight() ? lv_color_hex(0x1e8e4e) : lv_color_hex(0x3ddc84); }
    inline lv_color_t warn() { return isLight() ? lv_color_hex(0xb86e00) : lv_color_hex(0xffa726); }

    inline lv_color_t alert() { return isLight() ? lv_color_hex(0xa8202f) : lv_color_hex(0xd12b3f); }
}
