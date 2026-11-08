/*
 * Copyright (c) 2026 Emilio Schinina' <emilioschi@gmail.com>
 *
 * This file is part of libterae.
 *
 * libterae is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * libterae is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with libterae.  If not, see <https://www.gnu.org/licenses/>.
 */

/**
 * @file posix_probe.c
 * @brief What the tests and the benchmarks need from the operating system, and
 * a server does not.
 */

#include <signal.h>
#include <stddef.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "base_platform.h"

/** @brief Microseconds in a second. */
#define PROBE_SECOND_US 1000000U

/**
 * @brief Do nothing, so that the alarm only interrupts.
 * @param[in] number The signal.
 */
static void probe_on_alarm(int number)
{
    (void)number;
}

uint64_t mqtt_platform_cpu_us(void)
{
    /** @brief The processor time, in the clock's own units. */
    const clock_t used = clock();
    /** @brief The answer. */
    uint64_t micros = 0U;

    if (used != (clock_t)(-1)) {
        micros = ((uint64_t)used * PROBE_SECOND_US) / (uint64_t)CLOCKS_PER_SEC;
    }
    return micros;
}

bool mqtt_platform_interrupt_after_s(uint32_t seconds)
{
    /** @brief The action, which does not restart the call it interrupts. */
    struct sigaction action;

    (void)memset(&action, 0, sizeof(action));
    action.sa_handler = probe_on_alarm;
    (void)sigemptyset(&action.sa_mask);
    (void)sigaction(SIGALRM, &action, NULL);
    (void)alarm((unsigned int)seconds);
    return true;
}

mqtt_status mqtt_platform_fork(bool *child)
{
    /** @brief The outcome. */
    mqtt_status status = MQTT_OK;
    /** @brief The new process, or 0 in it. */
    const pid_t process = fork();

    if (process < 0) {
        status = MQTT_INVALID_STATE;
    } else {
        *child = (process == 0);
    }
    return status;
}

void mqtt_platform_child_exit(int32_t code)
{
    _exit((int)code);
}
