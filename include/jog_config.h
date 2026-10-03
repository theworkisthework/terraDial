#pragma once

// Shared jog step/feed presets -- used by both the Jog screen (X/Y/Z, knob
// rotate) and the Pen screen's Z-lift button, so a "step" moves identically
// wherever it's triggered from. All motion is via steppers.
struct JogStep
{
    float mm;
    float feedMmMin;
};

static const JogStep JOG_STEPS[] = {
    {0.1f, 200.0f},
    {1.0f, 800.0f},
    {10.0f, 2000.0f},
    {100.0f, 3000.0f},
};
static const int JOG_STEP_COUNT = 4;

// Largest step offered on Z. Z is the pen lift -- a few mm of travel between
// the bed and the top stop -- so the X/Y steps above this would drive the pen
// into one or the other.
//
// Deliberately a DISTANCE, not an index. This was "hide the last chip in the
// table", which silently meant "10mm" only for as long as 10mm happened to be
// last: adding the 100mm step below it would have quietly promoted 10mm to a
// legal Z move without a line of this file appearing to change.
static const float JOG_MAX_Z_MM = 1.0f;

// Pen up/down: the G-code line the Pen screen sends for each state, editable
// under Settings > Machine. The same Z0/Z5 as terraForge's terraPen machine
// config, so a plot started from terraForge and a pen lifted from the panel
// agree on where "up" is. Absolute rather than a relative jog: sending one
// twice is harmless, so the panel's idea of the pen state can't drift away
// from the machine's.
//
// Unlike terraForge's bare "G0Z5", each sets G90 (absolute) and G21 (mm)
// first. terraForge sends its pen moves inside a job it wrote, so it knows
// the modes in force; the panel sends them between jobs, into whatever modes
// the last one left. A job ending in G91 would make "G0Z0" a no-op and
// "G0Z5" a 5mm climb from wherever the pen was; one ending in G20 would make
// the lift five inches.
#define PEN_UP_CMD "G90 G21 G0 Z5"
#define PEN_DOWN_CMD "G90 G21 G0 Z0"
