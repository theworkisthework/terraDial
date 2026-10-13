#pragma once
#include <lvgl.h>

// QR codes in terraPen's colours rather than LVGL's stock black squares.
//
// The modules are drawn here, pixel by pixel, from qrcodegen's matrix (the
// encoder LVGL's own lv_qrcode wraps): a navy-to-plum gradient on a warm
// off-white card with rounded corners, rounded corner "eyes" with a red
// pupil, and an accent rim. Dark-on-light throughout, with the darkest
// colour kept well clear of the light one -- phones read that reliably;
// light-on-dark and low contrast are where styled codes stop scanning.
//
// Two sizes:
//  - create(): the small code on the About card. Plain square modules: at
//    two or three pixels a module, anything fancier only blurs.
//  - showFullscreen(): the big one, opened by tapping a small one. Room to
//    go further: the modules are dots joined into strokes, grown in from
//    the centre in a wave, and then the terraPen mark draws itself into a
//    gap in the middle, pen-style. The gap is paid for with QR's highest
//    error correction, which can rebuild far more than the logo hides.
namespace FancyQr
{
    // A small code for `url`, `side` px square, styled to match. Tapping it
    // opens the big one with `caption` under it. Both strings must outlive
    // the object (string literals do). Returns nullptr if url won't encode.
    lv_obj_t *create(lv_obj_t *parent, const char *url, const char *caption, lv_coord_t side);

    // The big code, over everything; a tap or any knob input closes it.
    void showFullscreen(const char *url, const char *caption);
    bool isOpen();
    void close();
}
