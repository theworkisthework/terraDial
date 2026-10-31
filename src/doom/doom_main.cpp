// terraDial's Doom build: the doomgeneric engine (lib/doomgeneric, GPL-2.0)
// running on the Waveshare Knob, playing terrapen.wad -- our own WAD, drawn
// by tools/doom/make_wad.py. A separate firmware (env waveshare-knob-1_8-doom),
// never part of the dial's, so terraDial itself stays MIT.
//
// The engine renders 320x200 in 256 colours. Each frame is converted to
// RGB565 through a palette table and pushed 1:1 into the middle of the
// 360px round panel -- its corners just clip at the rim.
//
// Controls, with no buttons on the Knob:
//   knob                  turn left / right
//   touch, top            forward
//   touch, bottom         back
//   touch, centre         fire
//   touch, left or right  use (doors, switches)
//
// The game fills the whole circle; health, armour, ink (ammo), kills and
// the level sit in little circles round the rim, over the top of it.
#include <Arduino.h>
#include "pins.h"
#include "display/lgfx_config.h"
#include "input/encoder.h"
#include "input/haptics.h"
#include <CST816D.h>
#include <esp_heap_caps.h>
#include <climits>

// Arduino's C++ has its own `boolean` (bool); Doom's is an unsigned int.
// Doom's is renamed for these headers, and spelled out where it crosses over.
#define boolean doom_boolean
extern "C"
{
#include "doomgeneric.h"
#include "doomkeys.h"
#include "w_file.h"
#include "doomtype.h"

#include "i_video.h"
#include "d_player.h"
#include "d_items.h"
#include "doomstat.h"
    extern struct color colors[256]; // i_video.c's palette (CMAP256)
    extern unsigned int palette_changed;
}
#undef boolean

static LGFX gfx;
static CST816D touch(PIN_TOUCH_SDA, PIN_TOUCH_SCL, PIN_TOUCH_RST, PIN_TOUCH_INT);
// jogWheel is encoder.cpp's global (declared in encoder.h).

// ---------------------------------------------------------------- the WAD

// Embedded by board_build.embed_files; built by tools/doom/make_wad.py.
extern const uint8_t wad_start[] asm("_binary_doom_terrapen_wad_start");
extern const uint8_t wad_end[] asm("_binary_doom_terrapen_wad_end");
static const char *WAD_NAME = "terrapen.wad";

static wad_file_t *memOpen(char *path);
static void memClose(wad_file_t *) {}
static size_t memRead(wad_file_t *file, unsigned int offset, void *buffer, size_t len)
{
    if (offset >= file->length) return 0;
    if (len > file->length - offset) len = file->length - offset;
    memcpy(buffer, file->mapped + offset, len);
    return len;
}
static wad_file_class_t memWadClass = {memOpen, memClose, memRead};

static wad_file_t *memOpen(char *path)
{
    if (strcmp(path, WAD_NAME) != 0) return NULL;
    static wad_file_t f;
    f.file_class = &memWadClass;
    f.mapped = (byte *)wad_start; // read-only: the engine never writes a mapped lump
    f.length = wad_end - wad_start;
    return &f;
}

extern "C" wad_file_t *DG_OpenWad(char *path) { return memOpen(path); }
extern "C" unsigned int DG_FileExists(char *filename) { return strcmp(filename, WAD_NAME) == 0; }

// ---------------------------------------------------------------- video
//
// Doom's own status bar is off (screenblocks 11) and the engine renders a
// 320x254 frame -- taller than Doom's 200, so it shows more above and
// below, and 254 rows stays inside the byte-sized plane tables. That's
// scaled to fill the whole 360x360 panel, about Doom's own pixel shape.
// The HUD -- little circles round the rim, like the dial's menu ring --
// is drawn into two off-screen bands, top and bottom, and laid over the
// game as each strip is built, so it never flickers.

static const int GAME_W = DOOMGENERIC_RESX, GAME_H = DOOMGENERIC_RESY;
static const int VIEW = PANEL_RES;
static const int CHUNK_ROWS = 8; // even, for BOARD_PANEL_EVEN_WINDOWS; 5.8KB of internal RAM per strip
// Palette index -> RGB565 already in the panel's byte order (swap565), so a
// strip goes out by DMA exactly as converted, with no copy on the way.
static uint16_t lut[256];
// Two strips taking turns: one is converted while the other is still going
// out by DMA. With one, the next strip overwrote the last mid-transfer --
// the scrambled bands.
static uint16_t *strip[2] = {nullptr, nullptr};
// Nearest-neighbour scaling: which source column/row each output one shows.
static uint16_t srcX[VIEW];
static uint8_t srcY[VIEW];

static void rebuildLut()
{
    for (int i = 0; i < 256; i++)
    {
        uint16_t c = ((colors[i].r & 0xf8) << 8) | ((colors[i].g & 0xfc) << 3) | (colors[i].b >> 3);
        lut[i] = (uint16_t)((c << 8) | (c >> 8));
    }
}

// ---------------------------------------------------------------- HUD
//
// terraPen colours, as the dial's skin: ink (ammo) lime, health coral,
// armour teal along the bottom; kills and the level along the top. A band
// is redrawn only when one of its numbers changes.

static const int BAND_H = 72;
static LGFX_Sprite band[2] = {LGFX_Sprite(&gfx), LGFX_Sprite(&gfx)}; // top, bottom
static uint16_t bandKey = 0; // a band pixel equal to this lets the game through

static const uint16_t C_KEY = 0xF81F; // magenta: "not HUD"
static const uint16_t C_BG = 0x0000;
static const uint16_t C_LIME = 0xE78B;  // #E5F15C
static const uint16_t C_CORAL = 0xE9CD; // #E83B68
static const uint16_t C_TEAL = 0x6679;  // #66CCCC
static const uint16_t C_GREY = 0x8C51;
static const int RIM_R = 150; // how far out the circles sit
static const int DOT_R = 19;

struct HudSlot
{
    float angleDeg; // 0 = right, 90 = down (screen coordinates)
    uint16_t colour;
    const char *label;
    int last;
};
static HudSlot hud[] = {
    {120, C_LIME, "INK", INT32_MIN},
    {90, C_CORAL, "HP", INT32_MIN},
    {60, C_TEAL, "ARM", INT32_MIN},
    {240, C_GREY, "KILL", INT32_MIN},
    {300, C_LIME, "LVL", INT32_MIN},
};
enum { H_INK, H_HP, H_ARM, H_KILL, H_LVL, H_COUNT };

static void drawSlot(LGFX_Sprite &b, int bandY, const HudSlot &s, int value)
{
    int cx = VIEW / 2 + (int)lroundf(RIM_R * cosf(s.angleDeg * (float)M_PI / 180));
    int cy = VIEW / 2 + (int)lroundf(RIM_R * sinf(s.angleDeg * (float)M_PI / 180)) - bandY;
    b.fillCircle(cx, cy, DOT_R, C_BG);
    b.drawCircle(cx, cy, DOT_R, s.colour);
    b.drawCircle(cx, cy, DOT_R - 1, s.colour);
    b.setTextColor(s.colour);
    b.setTextDatum(lgfx::middle_center);
    char buf[8];
    if (value < 0) snprintf(buf, sizeof buf, "--");
    else snprintf(buf, sizeof buf, "%d", value);
    b.setFont(&fonts::Font2);
    b.drawString(buf, cx, cy - 3);
    b.setFont(&fonts::Font0);
    b.drawString(s.label, cx, cy + 10);
}

// Faint hints for the forward/back zones.
static void drawChevron(LGFX_Sprite &b, int tipY, int dir)
{
    const int cx = VIEW / 2;
    for (int i = 0; i < 2; i++)
    {
        b.drawLine(cx - 10, tipY - 8 * dir + i, cx, tipY + i, C_GREY);
        b.drawLine(cx, tipY + i, cx + 10, tipY - 8 * dir + i, C_GREY);
    }
}

// Redraws whichever band has a number that changed.
static void updateHud()
{
    if (gamestate != GS_LEVEL) return;
    const player_t &p = players[consoleplayer];
    int ammoType = weaponinfo[p.readyweapon].ammo;
    int values[H_COUNT] = {
        ammoType == am_noammo ? -1 : p.ammo[ammoType],
        p.health,
        p.armorpoints,
        p.killcount,
        gamemap,
    };
    bool dirty[2] = {false, false};
    for (int i = 0; i < H_COUNT; i++)
        if (values[i] != hud[i].last) dirty[i >= H_KILL ? 0 : 1] = true;

    for (int which = 0; which < 2; which++)
    {
        if (!dirty[which]) continue;
        LGFX_Sprite &b = band[which];
        int bandY = which == 0 ? 0 : VIEW - BAND_H;
        b.fillScreen(C_KEY);
        if (which == 0) drawChevron(b, 52, -1);
        else drawChevron(b, VIEW - 52 - bandY, 1);
        for (int i = 0; i < H_COUNT; i++)
        {
            if ((i >= H_KILL) != (which == 0)) continue;
            hud[i].last = values[i];
            drawSlot(b, bandY, hud[i], values[i]);
        }
    }
}

extern "C" void DG_Init()
{
    for (int x = 0; x < VIEW; x++) srcX[x] = x * GAME_W / VIEW;
    for (int y = 0; y < VIEW; y++) srcY[y] = y * GAME_H / VIEW;
    for (auto &b : band)
    {
        b.setColorDepth(16);
        b.setPsram(true);
        b.createSprite(VIEW, BAND_H);
        b.fillScreen(C_KEY);
    }
    bandKey = ((const uint16_t *)band[0].getBuffer())[0]; // C_KEY as the band stores it
}

extern "C" void DG_DrawFrame()
{
    if (palette_changed)
    {
        rebuildLut();
        palette_changed = false;
    }
    updateHud();
    const uint8_t *src = (const uint8_t *)DG_ScreenBuffer;
    const uint16_t *top = (const uint16_t *)band[0].getBuffer();
    const uint16_t *bot = (const uint16_t *)band[1].getBuffer();
    gfx.startWrite();
    int which = 0;
    for (int row = 0; row < VIEW; row += CHUNK_ROWS)
    {
        uint16_t *out = strip[which];
        for (int r = 0; r < CHUNK_ROWS; r++)
        {
            int y = row + r;
            const uint8_t *line = src + srcY[y] * GAME_W;
            const uint16_t *over = y < BAND_H ? top + y * VIEW
                                 : y >= VIEW - BAND_H ? bot + (y - (VIEW - BAND_H)) * VIEW
                                 : nullptr;
            if (!over)
                for (int x = 0; x < VIEW; x++) *out++ = lut[line[srcX[x]]];
            else
                for (int x = 0; x < VIEW; x++) *out++ = over[x] != bandKey ? over[x] : lut[line[srcX[x]]];
        }
        gfx.waitDMA(); // the previous strip is out; the window can move on
        gfx.pushImageDMA(0, row, VIEW, CHUNK_ROWS, (const lgfx::swap565_t *)strip[which]);
        which ^= 1;
    }
    gfx.waitDMA();
    gfx.endWrite();
}

// ---------------------------------------------------------------- time

extern "C" void DG_SleepMs(uint32_t ms) { delay(ms); }
extern "C" uint32_t DG_GetTicksMs() { return millis(); }
extern "C" void DG_SetWindowTitle(const char *) {}

// ---------------------------------------------------------------- input

static const int QUEUE_LEN = 32;
static uint16_t keyQueue[QUEUE_LEN]; // (pressed << 8) | key
static int qHead = 0, qTail = 0;

static void pushKey(bool pressed, uint8_t key)
{
    int next = (qTail + 1) % QUEUE_LEN;
    if (next == qHead) return; // full: drop it
    keyQueue[qTail] = (pressed ? 0x100 : 0) | key;
    qTail = next;
}

extern "C" int DG_GetKey(int *pressed, unsigned char *key)
{
    if (qHead == qTail) return 0;
    uint16_t k = keyQueue[qHead];
    qHead = (qHead + 1) % QUEUE_LEN;
    *pressed = k >> 8;
    *key = k & 0xff;
    return 1;
}

// One knob detent holds the turn key this long, with "run" held too so it
// turns at Doom's fast rate: about 25 degrees a detent.
static const uint32_t TURN_MS = 100;
static uint8_t turnKey = 0;
static uint32_t turnUntil = 0;

// The key a touch at (x, y) holds down, or 0.
static uint8_t zoneKey(int x, int y)
{
    int dx = x - PANEL_RES / 2, dy = y - PANEL_RES / 2;
    const int centreR = PANEL_RES / 5;
    if (dx * dx + dy * dy < centreR * centreR) return KEY_FIRE;
    if (abs(dy) >= abs(dx)) return dy < 0 ? KEY_UPARROW : KEY_DOWNARROW;
    return KEY_USE;
}

// Called every tick, from the game's own task.
static void pollInput()
{
    uint32_t now = millis();

    jogWheel.update();
    int32_t d = jogWheel.takeRotationDelta();
    if (d != 0)
    {
        uint8_t k = d > 0 ? KEY_RIGHTARROW : KEY_LEFTARROW;
        if (turnKey && turnKey != k) pushKey(false, turnKey);
        if (!turnKey) pushKey(true, KEY_RSHIFT);
        if (turnKey != k) pushKey(true, k);
        turnKey = k;
        // Several detents at once turn for longer, not faster.
        turnUntil = now + TURN_MS * (uint32_t)abs(d);
    }
    if (turnKey && (int32_t)(now - turnUntil) >= 0)
    {
        pushKey(false, turnKey);
        pushKey(false, KEY_RSHIFT);
        turnKey = 0;
    }

    // Touch: hold a zone's key while the finger is in it.
    static uint8_t heldKey = 0;
    uint16_t x, y;
    uint8_t gesture;
    uint8_t want = touch.getTouch(&x, &y, &gesture) ? zoneKey(x, y) : 0;
    if (want != heldKey)
    {
        if (heldKey) pushKey(false, heldKey);
        if (want) pushKey(true, want);
        heldKey = want;
    }
}

// ---------------------------------------------------------------- the game

static void doomTask(void *)
{
    static char *argv[] = {(char *)"doom", (char *)"-iwad", (char *)"terrapen.wad", (char *)"-warp",
                           (char *)"1", (char *)"-skill", (char *)"3", nullptr};
    doomgeneric_Create(7, argv);
    for (;;)
    {
        pollInput();
        doomgeneric_Tick();
    }
}

void setup()
{
    Serial.begin(115200);
    delay(300);
    Serial.printf("terraDial Doom on %s, WAD %u bytes\n", BOARD_NAME, (unsigned)(wad_end - wad_start));

    touch.begin();
    Haptics::begin(); // a buzz per knob detent, as in the dial
    gfx.init();
    gfx.setRotation(DISPLAY_ROTATION);
    gfx.initDMA();
    gfx.fillScreen(TFT_BLACK);
    backlightInit();
    jogWheel.begin();

    for (int i = 0; i < 2; i++)
        strip[i] = (uint16_t *)heap_caps_malloc(VIEW * CHUNK_ROWS * 2, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);

    // The renderer recurses down the BSP tree, so the game gets a stack of
    // its own, bigger than loop()'s 8KB. Internal RAM is the limit: the
    // engine's tables take most of it.
    if (xTaskCreatePinnedToCore(doomTask, "doom", 32 * 1024, nullptr, 1, nullptr, 1) != pdPASS)
        Serial.println("Couldn't start the game task: out of internal RAM");
    Serial.printf("internal heap left: %u\n", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
}

void loop() { vTaskDelay(portMAX_DELAY); }
