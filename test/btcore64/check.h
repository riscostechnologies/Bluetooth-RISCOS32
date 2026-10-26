/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 RISC OS Technologies */
#ifndef CHECK_H
#define CHECK_H
#include <stdio.h>
#include <string.h>
extern int check_run, check_failed;
#define CHECK(name, cond) do { ++check_run; if (!(cond)) { ++check_failed; printf("FAIL %s:%d %s\n", __FILE__, __LINE__, (name)); } } while (0)
#define CHECK_TEXT(name, got, want) do { ++check_run; if (strcmp((got), (want)) != 0) { ++check_failed; printf("FAIL %s:%d %s: got '%s' want '%s'\n", __FILE__, __LINE__, (name), (got), (want)); } } while (0)
int check_summary(const char *suite);
#endif
