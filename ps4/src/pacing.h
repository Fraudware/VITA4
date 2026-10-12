// VITA4 - "Low lag" draw timing (1.29). Pure C, host-tested.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The screen flips once per TV refresh. 1.21 draws right after each flip, so
// a Vita frame waits for the start of the next refresh to be drawn and is
// shown one refresh after that. Low lag draws at a FIXED later moment in the
// refresh - just after the Vita frames usually arrive - so most frames are
// shown at the very next refresh (up to ~16 ms sooner).
//
// The moment must stay fixed: 1.22-1.27 drew "when a frame arrives, unless
// too late", a cut-off that kept moving, so frames near it went to one
// refresh or the next at random and motion stopped and started.
//
// vd_latch_choose looks at where in the refresh recent frames arrived
// (phase, seconds after the flip) and returns the draw moment: the late
// edge of the arrival window plus a margin, if that still leaves `budget`
// seconds to draw before the next refresh; otherwise 0 (= 1.21's timing).
// It also returns 0 when arrivals are spread too widely to place it safely.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
    double latch;  // chosen draw moment after the flip, seconds (0 = 1.21)
    double centre; // circular mean arrival phase, seconds
    double spread; // 5th..95th percentile width of the arrivals, seconds
    int usable;    // 1 if `latch` > 0 was possible
} VdLatchChoice;

#define VD_LATCH_MARGIN_S 0.0015 // after the late edge of the arrivals
#define VD_LATCH_MAX_SPREAD_S 0.006 // wider than this: no fixed moment is safe

// phases: n samples in [0, refresh). budget: drawing time to leave before
// the next refresh. Needs n >= 8 (fewer: latch 0, usable 0).
void vd_latch_choose(const double *phases, int n, double refresh, double budget,
                     VdLatchChoice *out);

#ifdef __cplusplus
}
#endif
