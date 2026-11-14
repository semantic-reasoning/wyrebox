/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "wyrebox-test-daemon.h"

#include <gio/gunixsocketaddress.h>
#include <string.h>

static void
dump_daemon_stderr (const guint8 *stderr_data, gsize stderr_length)
{
    g_printerr ("--- wyreboxd stderr ---\n");
    if (stderr_data == NULL || stderr_length == 0)
        g_printerr ("(unavailable)\n");
    else
        g_printerr ("%.*s\n", (int)stderr_length, (const char *)stderr_data);
    g_printerr ("--- end wyreboxd stderr ---\n");
}

static guint
socket_timeout_seconds (guint timeout_seconds)
{
    return timeout_seconds == 0 ?
           WYREBOX_TEST_DAEMON_SOCKET_TIMEOUT_SECONDS : timeout_seconds;
}

static gboolean
try_connect_daemon_socket (const char *socket_path)
{
    g_autoptr (GError) error = NULL;
    g_autoptr (GSocketAddress) address = NULL;
    g_autoptr (GSocketClient) client = NULL;
    g_autoptr (GSocketConnection) connection = NULL;

    client = g_socket_client_new ();
    g_socket_client_set_timeout (client, 1);
    address = g_unix_socket_address_new (socket_path);
    connection = g_socket_client_connect (client,
            G_SOCKET_CONNECTABLE (address), NULL, &error);
    return connection != NULL;
}

gboolean
wyrebox_test_wait_for_daemon_socket (GSubprocess *subprocess,
    const char *socket_path, guint timeout_seconds)
{
    gint64 deadline;

    g_return_val_if_fail (G_IS_SUBPROCESS (subprocess), FALSE);
    g_return_val_if_fail (socket_path != NULL, FALSE);

    deadline = g_get_monotonic_time ()
        + (gint64)socket_timeout_seconds (timeout_seconds)
        * G_TIME_SPAN_SECOND;

    while (g_get_monotonic_time () < deadline) {
        if (g_file_test (socket_path, G_FILE_TEST_EXISTS)
            && try_connect_daemon_socket (socket_path))
            return TRUE;
        if (g_subprocess_get_identifier (subprocess) == NULL)
            return FALSE;
        g_usleep (10 * 1000);
    }

    return FALSE;
}

void
wyrebox_test_fail_daemon_socket_wait (GSubprocess *subprocess,
    const guint8 *stderr_data, gsize stderr_length, const char *reason)
{
    g_autoptr (GError) error = NULL;

    g_assert_nonnull (reason);

    if (subprocess != NULL && g_subprocess_get_identifier (subprocess) != NULL)
        g_subprocess_force_exit (subprocess);
    if (subprocess != NULL)
        (void)g_subprocess_wait (subprocess, NULL, &error);

    dump_daemon_stderr (stderr_data, stderr_length);
    g_error ("%s", reason);
}

gboolean
wyrebox_test_wait_subprocess (GSubprocess *subprocess, guint timeout_seconds)
{
    g_autoptr (GError) error = NULL;
    gint64 deadline = g_get_monotonic_time ()
        + (gint64)socket_timeout_seconds (timeout_seconds) * G_TIME_SPAN_SECOND;
    gboolean timed_out = FALSE;

    g_return_val_if_fail (G_IS_SUBPROCESS (subprocess), FALSE);

    while (g_subprocess_get_identifier (subprocess) != NULL) {
        if (g_get_monotonic_time () >= deadline) {
            timed_out = TRUE;
            g_subprocess_force_exit (subprocess);
            break;
        }
        g_usleep (10 * 1000);
    }

    if (!g_subprocess_wait (subprocess, NULL, &error))
        return FALSE;
    return !timed_out;
}

void
wyrebox_test_assert_daemon_exited_cleanly (GSubprocess *subprocess,
    const guint8 *stderr_data, gsize stderr_length)
{
    int status = 0;

    g_return_if_fail (G_IS_SUBPROCESS (subprocess));

    if (g_subprocess_get_if_exited (subprocess)
        && g_subprocess_get_exit_status (subprocess) == 0)
        return;

    dump_daemon_stderr (stderr_data, stderr_length);

    if (!g_subprocess_get_if_exited (subprocess)) {
        int signal_number = g_subprocess_get_if_signaled (subprocess) ?
            g_subprocess_get_term_sig (subprocess) : 0;

        g_error ("wyreboxd terminated by signal %d (%s)", signal_number,
            g_strsignal (signal_number));
    }

    status = g_subprocess_get_exit_status (subprocess);
    g_error ("wyreboxd exited with status %d", status);
}

gboolean
wyrebox_test_check_daemon_exited_cleanly (GSubprocess *subprocess,
    const guint8 *stderr_data, gsize stderr_length)
{
    g_return_val_if_fail (G_IS_SUBPROCESS (subprocess), FALSE);

    if (g_subprocess_get_if_exited (subprocess)
        && g_subprocess_get_exit_status (subprocess) == 0)
        return TRUE;

    dump_daemon_stderr (stderr_data, stderr_length);

    if (!g_subprocess_get_if_exited (subprocess)) {
        int signal_number = g_subprocess_get_if_signaled (subprocess) ?
            g_subprocess_get_term_sig (subprocess) : 0;

        g_test_message ("wyreboxd terminated by signal %d (%s)",
            signal_number, g_strsignal (signal_number));
        g_test_fail ();
        return FALSE;
    }

    g_test_message ("wyreboxd exited with status %d",
        g_subprocess_get_exit_status (subprocess));
    g_test_fail ();
    return FALSE;
}
