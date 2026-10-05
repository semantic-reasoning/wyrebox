#include "wyrebox-daemon-exit-code.h"

#include <gio/gio.h>
#include <sysexits.h>

int
wyrebox_daemon_exit_code_for_startup_error (const GError *error)
{
    if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA) ||
        g_error_matches (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED))
        return EX_DATAERR;

    return EX_TEMPFAIL;
}
