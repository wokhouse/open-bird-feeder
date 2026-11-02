// Zone tuning knobs, shared by the firmware and the host test harness.
//
// Zone is in percent of the probe frame so it survives a resolution change.
// Current feeder framing (open-bird-feeder hero photo): the tray rim spans the
// 65-78% band and the seed pile fills the bottom quarter, so the zone starts
// just above the rim and runs to the bottom edge. If the camera gets rotated
// or re-aimed, change these numbers and nothing else.
//
// Every knob is #ifndef-guarded so the host test can sweep values with -D.

#pragma once

#ifndef ZONE_X0_PCT
#define ZONE_X0_PCT 0
#endif
#ifndef ZONE_X1_PCT
#define ZONE_X1_PCT 100
#endif
#ifndef ZONE_Y0_PCT
#define ZONE_Y0_PCT 62
#endif
#ifndef ZONE_Y1_PCT
#define ZONE_Y1_PCT 100
#endif

#ifndef ZONE_CELL
#define ZONE_CELL 4 /* cell side in probe pixels */
#endif
#ifndef ZONE_CELL_THRESH
#define ZONE_CELL_THRESH 14 /* mean |diff| (0-255) for a cell to count as hot */
#endif
#ifndef ZONE_MIN_HOT_CELLS
#define ZONE_MIN_HOT_CELLS 6 /* hot cells for a frame to count as motion */
#endif
#ifndef ZONE_TRIGGER_FRAMES
#define ZONE_TRIGGER_FRAMES 2 /* consecutive motion frames before a capture */
#endif
#ifndef ZONE_LIGHT_REJECT
#define ZONE_LIGHT_REJECT 1 /* subtract the zone-wide mean diff (clouds, AEC) */
#endif
