/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#pragma once

/*
 * Test-only request counters. When @env_name is unset or empty, no-op.
 * Used by real-imap tests to bound daemon round trips.
 */
void wyrebox_daemon_test_counter_increment (const char *env_name);
