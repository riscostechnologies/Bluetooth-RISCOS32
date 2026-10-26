/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 RISC OS Technologies */
#include "check.h"
int check_run, check_failed;
int check_summary(const char *suite)
{
    printf("%s: %d checks, %d failures\n", suite, check_run, check_failed);
    return check_failed != 0;
}
