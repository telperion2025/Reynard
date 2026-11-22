/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2025 The Reynard contributors */
/**
 * @file ddatecnv.c
 * @brief Proleptic Gregorian civil <-> julian day conversions (Howard
 *        Hinnant's public-domain algorithms), used by date arithmetic
 *        (spec §7.3) and datetime fields (§8.2).
 */
#include "util/util.h"

long rx_julian(int y, int m, int d)
{
    long yy = y - (m <= 2 ? 1 : 0);
    long era = (yy >= 0 ? yy : yy - 399) / 400;
    unsigned long yoe = (unsigned long)(yy - era * 400);
    unsigned long doy = (153 * (unsigned long)(m + (m > 2 ? -3 : 9)) + 2) / 5 + (unsigned long)d - 1;
    unsigned long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097L + (long)doe - 719468L;
}

void rx_civil(long j, int *y, int *m, int *d)
{
    long z = j + 719468L;
    long era = (z >= 0 ? z : z - 146096L) / 146097L;
    unsigned long doe = (unsigned long)(z - era * 146097L);
    unsigned long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long yy = (long)yoe + era * 400;
    unsigned long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned long mp = (5 * doy + 2) / 153;
    *d = (int)(doy - (153 * mp + 2) / 5 + 1);
    *m = (int)(mp + (mp < 10 ? 3 : -9));
    *y = (int)(yy + (*m <= 2 ? 1 : 0));
}

static int is_leap(int y)
{
    return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
}

int rx_date_valid(int y, int m, int d)
{
    if (y < 1 || m < 1 || m > 12 || d < 1) {
        return 0;
    }
    static const int mdays[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    int maxd = mdays[m - 1] + (m == 2 && is_leap(y) ? 1 : 0);
    return d <= maxd;
}
