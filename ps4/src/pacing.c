// VITA4 - "Low lag" draw timing. See pacing.h.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "pacing.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static int cmp_double(const void *a, const void *b)
{
    const double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : (x > y ? 1 : 0);
}

void vd_latch_choose(const double *phases, int n, double refresh, double budget,
                     VdLatchChoice *out)
{
    memset(out, 0, sizeof(*out));
    if (!phases || n < 8 || refresh <= 0)
        return;
    if (n > 256)
        n = 256;
    // Circular mean: phases wrap at the refresh.
    const double k = 6.283185307179586 / refresh;
    double sx = 0, sy = 0;
    for (int i = 0; i < n; ++i)
    {
        sx += cos(phases[i] * k);
        sy += sin(phases[i] * k);
    }
    double centre = atan2(sy, sx) / k;
    if (centre < 0)
        centre += refresh;
    // Signed distance of each arrival from the centre, in (-R/2, R/2].
    double d[256];
    for (int i = 0; i < n; ++i)
    {
        double v = phases[i] - centre;
        while (v > refresh / 2)
            v -= refresh;
        while (v <= -refresh / 2)
            v += refresh;
        d[i] = v;
    }
    qsort(d, (size_t)n, sizeof(double), cmp_double);
    const double lo = d[(n * 5) / 100], hi = d[(n * 95) / 100 < n ? (n * 95) / 100 : n - 1];
    out->centre = centre;
    out->spread = hi - lo;
    if (out->spread > VD_LATCH_MAX_SPREAD_S)
        return; // too irregular: 1.21 timing
    double latch = centre + hi + VD_LATCH_MARGIN_S;
    while (latch >= refresh)
        latch -= refresh;
    while (latch < 0)
        latch += refresh;
    // The draw must finish before the next refresh. Arrivals that late in
    // the refresh are best caught right after the flip anyway (1.21).
    if (latch + budget > refresh)
        return;
    // (If it wrapped past the refresh, the arrivals sit at the very end of
    // the refresh and the latch lands just after the flip that follows them:
    // still after every arrival, so still a clean fixed moment.)
    out->latch = latch;
    out->usable = 1;
}
