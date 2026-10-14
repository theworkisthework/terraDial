#pragma once
#include <lvgl.h>
#include <stdint.h>

// terraForge's dark theme, ported 1:1 from that project's design tokens
// (theworkisthework/terraForge, src/renderer/src/index.css -- the `:root`
// block, which is its dark/default theme). Values are copied exactly rather
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
// A second skin, terraPen, follows the terraPen brand playbook
// (terrapen_identity/terraPen_brandPlaybook.pdf): its "lime / shell green"
// pairing, the one terrapen.uk itself uses. Lime is the accent on shell
// green surfaces, with white copy, as the playbook's two-colour rule allows.
// Coral, the brand's other colour, is kept for the alert alone: E-Stop has
// to read as a warning in either skin.
//
// The skin is chosen in Settings > Display and applied at boot (main.cpp):
// widgets take their colours as they're built, so a change restarts the
// panel rather than repainting every screen.
namespace Palette
{
    enum class Skin : uint8_t { TerraForge, TerraPen };
    inline Skin &skinRef() { static Skin s = Skin::TerraForge; return s; }
    inline void setSkin(Skin s) { skinRef() = s; }
    inline Skin skin() { return skinRef(); }
    // terraForge's value, then terraPen's.
    inline lv_color_t pick(uint32_t forge, uint32_t pen) { return lv_color_hex(skin() == Skin::TerraPen ? pen : forge); }

    // -- Surfaces, darkest to lightest --
    inline lv_color_t bgTerminal() { return pick(0x0d1117, 0x0e2011); }  // --tf-bg-terminal, deepest (log/console style panes)
    inline lv_color_t bgApp() { return pick(0x1a1a2e, 0x17321b); }       // --tf-bg-app, the screen background
    inline lv_color_t bgPanel() { return pick(0x16213e, 0x1d3d22); }     // --tf-bg-panel
    inline lv_color_t bgSecondary() { return pick(0x0f3460, 0x24492a); } // --tf-bg-secondary, the "dark sea blue" raised surface: cards, chips, buttons
    inline lv_color_t bgSecondaryHover() { return pick(0x1a4a8a, 0x2f5c36); } // --tf-bg-secondary-hover

    // -- Lines and text --
    inline lv_color_t border() { return pick(0x5c7a9e, 0x4f7a55); }    // --tf-border
    inline lv_color_t text() { return pick(0xe0e0e0, 0xf4f7ec); }      // --tf-text (slightly off pure white)
    inline lv_color_t textMuted() { return pick(0x9ca3af, 0xa9bfa6); } // --tf-text-muted
    inline lv_color_t textFaint() { return pick(0x818ea5, 0x8aa487); } // --tf-text-faint

    // -- Accents --
    // The red-magenta that defines terraForge's look against the navy.
    inline lv_color_t accent() { return pick(0xe94560, 0xe5f15c); }      // --tf-accent
    inline lv_color_t accentHover() { return pick(0xc73d56, 0xcbd64a); } // --tf-accent-hover
    // Foreground for text/icons on accent() fills. The accent is a
    // saturated mid-tone red, so it needs light text on top (the previous
    // lavender-pastel accent needed the opposite -- dark text).
    inline lv_color_t accentFg() { return pick(0xffffff, 0x17321b); }
    // Informational secondary accent -- terraForge's file-browser blue.
    // Used where something needs to stand out WITHOUT competing with the
    // red (e.g. the "pen down" status pill).
    // terraPen spends it on coral, its second brand colour: section titles,
    // the demo tag, the spirograph's gear.
    inline lv_color_t accentSecondary() { return pick(0x60a0ff, 0xe83b68); } // --tf-fs-blue-text

    // Icons at rest (unselected ring items, back arrows, screen headers).
    // terraForge keeps them muted grey; terraPen colours them coral.
    inline lv_color_t icon() { return pick(0x9ca3af, 0xe83b68); }
    // Status that's information, not a call to act: homing, a light rail
    // that's reachable. terraForge's blue; terraPen's teal.
    inline lv_color_t info() { return pick(0x60a0ff, 0x66cccc); }
    // The machine in ALARM. terraForge's red accent; in terraPen the accent
    // is lime, which says nothing like "alarm", so coral.
    inline lv_color_t alarmState() { return pick(0xe94560, 0xe83b68); }

    inline lv_color_t selectedFill() { return pick(0x1a3a6e, 0x2a5531); } // --tf-file-selected, dim blue selected-row fill

    // Deliberately a deeper, heavier red than accent(): E-Stop has to read
    // as an escalation, and the accent alone is too common elsewhere in the
    // UI (selected dial item, active chips) to carry that weight by itself.
    inline lv_color_t alert() { return pick(0xd12b3f, 0xe83b68); }
    // Text and icons on alert() fills. White in both skins: accentFg() is
    // dark in terraPen, to sit on lime, and would be muddy on coral.
    inline lv_color_t alertFg() { return lv_color_hex(0xffffff); }
}
