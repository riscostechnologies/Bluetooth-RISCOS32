/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (C) RISC OS Technologies 2026 */
#include "check.h"
int check_run, check_failed;
int check_summary(const char *suite)
{
    printf("%s: %d checks, %d failures\n", suite, check_run, check_failed);
    return check_failed != 0;
}
