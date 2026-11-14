/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#pragma once

#include <gio/gio.h>
#include <glib.h>

G_BEGIN_DECLS

#define WYREBOX_TEST_DAEMON_SOCKET_TIMEOUT_SECONDS 30

/*
 * Waits until @socket_path accepts a connect, or until @subprocess exits, or
 * until @timeout_seconds elapse (0 uses the default). Path existence alone is
 * not readiness.
 */
gboolean wyrebox_test_wait_for_daemon_socket (GSubprocess *subprocess,
    const char *socket_path,
    guint timeout_seconds);

/*
 * Reaps @subprocess if it is still running, dumps @stderr_data, and fails the
 * test. @stderr_data may be NULL.
 */
G_NORETURN void wyrebox_test_fail_daemon_socket_wait (GSubprocess *subprocess,
    const guint8 *stderr_data,
    gsize stderr_length,
    const char *reason);

/*
 * Waits for @subprocess up to @timeout_seconds (0 uses the default). On
 * timeout, force-exits and waits once more. Returns whether wait succeeded.
 */
gboolean wyrebox_test_wait_subprocess (GSubprocess *subprocess,
    guint timeout_seconds);

/*
 * Fails the test unless @subprocess exited with status 0. Dumps @stderr_data
 * first so TAP does not fold a sanitizer report onto one line.
 */
void wyrebox_test_assert_daemon_exited_cleanly (GSubprocess *subprocess,
    const guint8 *stderr_data,
    gsize stderr_length);

/*
 * Like wyrebox_test_assert_daemon_exited_cleanly, but records failure with
 * g_test_fail() so fixture teardown can still clean up. Returns TRUE when
 * the exit was clean.
 */
gboolean wyrebox_test_check_daemon_exited_cleanly (GSubprocess *subprocess,
    const guint8 *stderr_data,
    gsize stderr_length);

G_END_DECLS
