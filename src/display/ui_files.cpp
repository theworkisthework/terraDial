#include "ui_files.h"
#include "ui_scale.h"
#include "lucide_icons.h"
#include "../net/fluidnc_client.h"
#include "radial_ring.h"
#include "palette.h"
#include "ui_screen_shell.h"
#include "ui_widgets.h"
#include "ui_nav.h"
#include <stdio.h>
#include <string.h>

// "Jobs" -- SD-card file browsing, presented as the same radial ring the
// home dial uses (RadialRing), with the selected job's name in the centre
// hub.
//
// It was a carousel of detail cards before. The problem wasn't the paging,
// it was that the peeking cards rendered a filename and size at ~84px and
// half-faded: text you couldn't read but your eye kept trying to. Ring
// chips carry no text at all, so there's nothing to squint at -- you rotate
// and read the one place that matters, the hub. It also means Jobs and Home
// now share one browsing idiom instead of two.
//
// Folders are entries like files: opening one lists it, and back (the knob's
// long-press or the on-screen arrow) steps up a folder before it leaves the
// screen. The ring is virtual (RadialRing::setVirtual), so a folder of any
// size costs the same handful of chips -- only the entries on the arc have
// one.
namespace
{
    // Ring geometry. The arc is spread toward the top slot (see
    // RadialRing::setSpread) exactly like the home dial: on a 1.28" panel a
    // row of near-identical chips tells you nothing about which one the hub
    // is describing, so the selection gets the angular room to be visibly
    // the biggest thing on screen and the tail bunches up behind it.
    //
    // 60 at radius 80 leaves the selected chip 2px clear of the 96px hub,
    // which is what caps the near size here.
    const lv_coord_t RING_RADIUS = px(80);
    const lv_coord_t RING_SIZE_NEAR = px(60);
    const lv_coord_t RING_SIZE_FAR = px(20);
    const float RING_SPREAD = 0.55f;

    // The arc below: 30-degree pitch, +/-132 degrees. At most
    // floor(264 / 30) + 1 = 9 entries are on it at once.
    const float RING_STEP_DEG = 30.0f;
    const float RING_HALF_ARC_DEG = 132.0f;
    const int RING_POOL = 9;

    // A full path: folder, separator, FAT's longest name.
    const size_t PATH_BUF = SD_DIR_MAX + 1 + SD_NAME_MAX + 1;

    RadialRing ring;
    lv_obj_t *screenRoot = nullptr;

    lv_obj_t *hubNameLbl = nullptr;
    lv_obj_t *hubMetaLbl = nullptr;
    lv_obj_t *hubActionLbl = nullptr;

    // The folder being shown (or fetched), relative to the SD root with no
    // leading or trailing slash -- "" is the root.
    char curDir[SD_DIR_MAX] = "";
    char selectedPath[PATH_BUF] = {0};

    // dir + "/" + name, or just name at the root. False if it won't fit.
    bool joinPath(char *out, size_t outSize, const char *dir, const char *name)
    {
        int n = dir[0] ? snprintf(out, outSize, "%s/%s", dir, name) : snprintf(out, outSize, "%s", name);
        return n >= 0 && (size_t)n < outSize;
    }

    void formatSize(char *buf, size_t bufSize, int32_t size)
    {
        if (size >= 1024 * 1024) snprintf(buf, bufSize, "%.1f MB", size / 1048576.0f);
        else if (size >= 1024) snprintf(buf, bufSize, "%.1f KB", size / 1024.0f);
        else snprintf(buf, bufSize, "%ld B", (long)size);
    }

    void showHubMessage(const char *name, const char *meta)
    {
        lv_label_set_text(hubNameLbl, name);
        lv_label_set_text(hubMetaLbl, meta);
        lv_label_set_text(hubActionLbl, "");
    }

    // Nothing to select: the list is empty or didn't arrive. These used to
    // collapse into one "SD card empty", which is also what showed while
    // the list was loading and when the plotter was unreachable.
    void showEmptyHub()
    {
        if (fluidNC.fileListFailed()) showHubMessage("No list", "Can't reach plotter");
        else if (curDir[0]) showHubMessage("No jobs", "Empty folder");
        else showHubMessage("No jobs", "SD card empty");
    }

    void showLoadingHub()
    {
        showHubMessage("Loading...", curDir[0] ? curDir : "SD card");
    }

    void refreshHub(int index)
    {
        FluidNCFileEntry entry;
        if (!fluidNC.fileListEntry(index, entry))
        {
            showEmptyHub();
            return;
        }
        // Long filenames are the normal case, not an edge case -- the label
        // is width-limited and scrolls rather than wrapping the hub open.
        // Safe to set unconditionally: RadialRing only calls onSelect when
        // the selection actually changes, so this can't restart the scroll
        // animation mid-cycle the way a periodic update would.
        lv_label_set_text(hubNameLbl, entry.name);
        if (entry.isDir)
        {
            lv_label_set_text(hubMetaLbl, "Folder");
            lv_label_set_text(hubActionLbl, LUCIDE_FOLDER_OPEN " Open");
            return;
        }
        char sizeBuf[16];
        formatSize(sizeBuf, sizeof(sizeBuf), entry.size);
        lv_label_set_text(hubMetaLbl, sizeBuf);

        // Say so up front rather than let a tap do nothing: FluidNC can't
        // take a command this long (see FluidNCClient::runPathFits).
        char path[PATH_BUF];
        bool runnable = joinPath(path, sizeof(path), curDir, entry.name) && FluidNCClient::runPathFits(path);
        lv_label_set_text(hubActionLbl, runnable ? LUCIDE_PLAY " Run" : "Path too long");
    }

    void openFolder(const char *dir)
    {
        strncpy(curDir, dir, sizeof(curDir) - 1);
        curDir[sizeof(curDir) - 1] = '\0';
        // Clear the old folder's entries straight away, so nothing on
        // screen can be opened against a list that's about to be replaced.
        ring.setCount(0);
        showLoadingHub();
        fluidNC.requestFileList(curDir);
    }

    // Up one folder. False at the root, where "back" means leave the screen.
    bool goUp()
    {
        if (!curDir[0]) return false;
        char parent[SD_DIR_MAX];
        strcpy(parent, curDir);
        char *slash = strrchr(parent, '/');
        if (slash) *slash = '\0';
        else parent[0] = '\0';
        openFolder(parent);
        return true;
    }

    void backBtnCb(lv_event_t *e)
    {
        (void)e;
        if (!goUp()) UiNav::goHome();
    }

    void confirmCb(lv_event_t *e)
    {
        lv_obj_t *mbox = lv_event_get_current_target(e);
        const char *txt = lv_msgbox_get_active_btn_text(mbox);
        if (txt && !strcmp(txt, "Run")) fluidNC.runFile(selectedPath);
        else if (txt && !strcmp(txt, "Delete"))
        {
            if (fluidNC.deleteFile(selectedPath)) fluidNC.requestFileList(curDir); // show it gone
        }
        lv_msgbox_close(mbox);
    }

    void openConfirmFor(const FluidNCFileEntry &entry)
    {
        if (!joinPath(selectedPath, sizeof(selectedPath), curDir, entry.name)) return;

        static const char *btns[] = {"Run", "Delete", "Cancel", ""};
        lv_obj_t *mbox = lv_msgbox_create(NULL, "File", entry.name, btns, false);
        // One scrolling line, like the hub: a long name wrapped over
        // several lines would push the buttons off the round panel.
        lv_obj_t *text = lv_msgbox_get_text(mbox);
        lv_obj_set_width(text, px(170));
        lv_label_set_long_mode(text, LV_LABEL_LONG_SCROLL_CIRCULAR);
        lv_obj_center(mbox);
        lv_obj_add_event_cb(mbox, confirmCb, LV_EVENT_VALUE_CHANGED, NULL);
    }

    // Same colour treatment as the dial's items, so the two rings read as
    // one system: the chip warms from the raised navy surface to the red
    // accent as it reaches the top slot.
    void onItemStyle(lv_obj_t *chip, int i, float nearness)
    {
        lv_opa_t mix = (lv_opa_t)(255 * nearness);
        lv_obj_set_style_bg_color(chip, lv_color_mix(Palette::accent(), Palette::bgSecondary(), mix), 0);

        lv_obj_t *icon = lv_obj_get_child(chip, 0);
        if (!icon) return;
        lv_obj_set_style_text_color(icon, lv_color_mix(Palette::accentFg(), Palette::textMuted(), mix), 0);

        // A font change forces a label relayout, unlike the colour write
        // above -- skip it unless the size bucket actually flipped, since
        // this runs for every chip on every animation frame. The bucket is
        // kept on the chip itself (its user data): chips are reused for
        // different entries as the list scrolls, so `i` isn't a stable key.
        (void)i;
        UiRingIconSize current = (UiRingIconSize)(intptr_t)lv_obj_get_user_data(chip);
        UiRingIconSize want = uiRingIconSize(nearness, current);
        if (want != current)
        {
            lv_obj_set_user_data(chip, (void *)(intptr_t)want);
            lv_obj_set_style_text_font(icon, uiRingIconFont(want), 0);
        }
    }

    lv_obj_t *makeChip(lv_obj_t *parent)
    {
        lv_obj_t *chip = lv_obj_create(parent);
        lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(chip, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(chip, 0, 0);
        lv_obj_set_style_shadow_width(chip, px(12), 0);
        lv_obj_set_style_shadow_color(chip, lv_color_black(), 0);
        lv_obj_set_style_shadow_opa(chip, LV_OPA_30, 0);
        lv_obj_clear_flag(chip, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_pad_all(chip, 0, 0);
        lv_obj_set_ext_click_area(chip, px(10));

        lv_obj_t *icon = lv_label_create(chip);
        lv_label_set_text(icon, LUCIDE_FILE);
        // Must match the chip's recorded bucket (user data, 0 = small) --
        // onItemStyle only writes a font when the bucket CHANGES, so a
        // mismatch leaves the chip drawn at the wrong size.
        lv_obj_set_user_data(chip, (void *)(intptr_t)UiRingIconSmall);
        lv_obj_set_style_text_font(icon, uiRingIconFont(UiRingIconSmall), 0);
        lv_obj_center(icon);
        return chip;
    }

    // The ring has brought entry `index` onto the arc on this chip.
    void bindChip(lv_obj_t *chip, int index)
    {
        lv_obj_t *icon = lv_obj_get_child(chip, 0);
        if (!icon) return;
        FluidNCFileEntry entry;
        bool isDir = fluidNC.fileListEntry(index, entry) && entry.isDir;
        lv_label_set_text(icon, isDir ? LUCIDE_FOLDER : LUCIDE_FILE);
    }

    void rebuildList()
    {
        int count = fluidNC.fileListCount();
        ring.setCount(count);
        if (count == 0) showEmptyHub();
        else refreshHub(ring.selectedIndex());
    }

    void onCardOpen(int index)
    {
        FluidNCFileEntry entry;
        if (!fluidNC.fileListEntry(index, entry)) return;
        char path[PATH_BUF];
        if (!joinPath(path, sizeof(path), curDir, entry.name)) return;
        if (entry.isDir) openFolder(path);
        else fluidNC.runFile(path); // refuses a path too long -- the hub already says so
    }

    void hubTapCb(lv_event_t *e)
    {
        (void)e;
        ring.openSelected();
    }
}

lv_obj_t *uiFilesCreate()
{
    screenRoot = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(screenRoot, Palette::bgApp(), 0);

    // Centre hub: the one place a filename is readable, and the run button.
    // Slightly larger than the dial's hub because filenames need the width.
    lv_obj_t *hub = lv_obj_create(screenRoot);
    lv_obj_set_size(hub, px(96), px(96));
    lv_obj_set_style_radius(hub, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(hub, Palette::bgSecondary(), 0);
    lv_obj_set_style_bg_color(hub, Palette::accentHover(), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(hub, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(hub, Palette::border(), 0);
    lv_obj_set_style_border_width(hub, px(1), 0);
    lv_obj_set_style_pad_all(hub, 0, 0);
    lv_obj_clear_flag(hub, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(hub, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(hub, hubTapCb, LV_EVENT_CLICKED, NULL);
    lv_obj_align(hub, LV_ALIGN_CENTER, 0, 0);

    hubNameLbl = lv_label_create(hub);
    lv_obj_set_width(hubNameLbl, px(82));
    // Scrolls, rather than ellipsising. 82px holds roughly a dozen characters
    // and plotter files are routinely named by layer -- "drawing 1", "drawing
    // 2", "drawing 3" -- so the digit that tells them apart is the character
    // an ellipsis eats first. Every name here is a name you're choosing
    // between, so the end of it has to arrive on its own.
    //
    // CIRCULAR (wraps around through a gap) rather than plain SCROLL (runs to the
    // end, then reverses): the reversal reads as the text having stopped, and
    // on a name that only just overflows it can look like a twitch. LVGL only
    // animates when the text actually overflows, so short names sit still.
    lv_label_set_long_mode(hubNameLbl, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(hubNameLbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(hubNameLbl, &UI_FONT_12, 0);
    lv_obj_set_style_text_color(hubNameLbl, Palette::text(), 0);
    lv_obj_align(hubNameLbl, LV_ALIGN_CENTER, 0, px(-18));

    hubMetaLbl = lv_label_create(hub);
    lv_obj_set_style_text_font(hubMetaLbl, &UI_FONT_12, 0);
    lv_obj_set_style_text_color(hubMetaLbl, Palette::textMuted(), 0);
    lv_obj_align(hubMetaLbl, LV_ALIGN_CENTER, 0, px(4));

    hubActionLbl = lv_label_create(hub);
    lv_obj_set_style_text_font(hubActionLbl, &UI_ICONS_12, 0); // icon + word
    lv_obj_set_style_text_color(hubActionLbl, Palette::accent(), 0);
    lv_obj_align(hubActionLbl, LV_ALIGN_CENTER, 0, px(24));

    // Arc layout rather than a full circle: 30-degree pitch, and nothing
    // drawn past +/-132 degrees. That empties the 5/6/7 o'clock arc so the
    // back button below can't be mistaken for a job chip -- mis-tapping one
    // starts a plot, which is not a cheap mistake -- and it lets the list
    // scroll instead of squeezing every file onto one circle.
    // opaFar 0 makes chips fade right out as they reach the arc edge.
    //
    // The pitch stays 30 degrees; the spread is what redistributes it, so
    // the chips either side of the selection sit ~45 degrees out and the
    // tail past them closes up toward the arc edge where it's fading out
    // anyway. Same number of files on screen, far more legible ordering.
    ring.create(screenRoot, RING_RADIUS, RING_SIZE_NEAR, RING_SIZE_FAR, LV_OPA_COVER, LV_OPA_TRANSP);
    ring.setArcLayout(RING_STEP_DEG, RING_HALF_ARC_DEG);
    ring.setVirtual(RING_POOL, makeChip, bindChip);
    ring.setSpread(RING_SPREAD);
    ring.setOnOpen(onCardOpen);
    ring.setOnItemStyle(onItemStyle);
    ring.setOnSelect(refreshHub);

    lv_obj_move_foreground(hub);
    showLoadingHub();

    addBackButton(screenRoot, backBtnCb);
    return screenRoot;
}

void uiFilesSetFocused(bool focused)
{
    // Refreshes whichever folder you were last in, rather than dropping
    // you back at the root every time you look away.
    if (!focused) return;
    fluidNC.requestFileList(curDir);
    // The fetch waits for a connection; until then, say why nothing's
    // coming rather than show "Loading..." indefinitely.
    if (!fluidNC.status().connected && ring.count() == 0) showHubMessage("Offline", "Can't reach plotter");
}

bool uiFilesHandleBack()
{
    return goUp();
}

void uiFilesHandleRotate(int32_t delta)
{
    if (delta == 0) return;
    for (int32_t i = 0; i < delta; i++) ring.selectNext();
    for (int32_t i = 0; i < -delta; i++) ring.selectPrev();
}

void uiFilesHandleSelect()
{
    ring.openSelected();
}

void uiFilesHandleDoubleClick()
{
    FluidNCFileEntry entry;
    if (!fluidNC.fileListEntry(ring.selectedIndex(), entry)) return;
    if (entry.isDir) return; // a folder has no Run/Delete
    openConfirmFor(entry);
}

void uiFilesUpdate()
{
    if (!fluidNC.fileListReady()) return;
    fluidNC.clearFileListReady();

    // A list for a folder we've since left (opened, then backed out before
    // it arrived) -- the one we're in now is still on its way.
    char dir[SD_DIR_MAX];
    fluidNC.fileListDir(dir, sizeof(dir));
    if (strcmp(dir, curDir) != 0) return;

    rebuildList();
}
