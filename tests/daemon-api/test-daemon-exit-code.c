#include "wyrebox-daemon-exit-code.h"

#include <gio/gio.h>
#include <glib.h>
#include <sysexits.h>

static void
assert_exit_code (GQuark domain, gint code, int expected)
{
    g_autoptr (GError) error = g_error_new_literal (domain, code, "failure");

    g_assert_cmpint (wyrebox_daemon_exit_code_for_startup_error (error), ==,
        expected);
}

static void
test_permanent_errors_exit_dataerr (void)
{
    assert_exit_code (G_IO_ERROR, G_IO_ERROR_INVALID_DATA, EX_DATAERR);
    assert_exit_code (G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, EX_DATAERR);
}

static void
test_other_errors_exit_tempfail (void)
{
    assert_exit_code (G_IO_ERROR, G_IO_ERROR_FAILED, EX_TEMPFAIL);
    assert_exit_code (G_IO_ERROR, G_IO_ERROR_BUSY, EX_TEMPFAIL);
    assert_exit_code (G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED, EX_TEMPFAIL);
    assert_exit_code (G_FILE_ERROR, G_FILE_ERROR_NOENT, EX_TEMPFAIL);
    g_assert_cmpint (wyrebox_daemon_exit_code_for_startup_error (NULL), ==,
        EX_TEMPFAIL);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);

    g_test_add_func ("/daemon/exit-code/permanent-errors-exit-dataerr",
        test_permanent_errors_exit_dataerr);
    g_test_add_func ("/daemon/exit-code/other-errors-exit-tempfail",
        test_other_errors_exit_tempfail);

    return g_test_run ();
}
