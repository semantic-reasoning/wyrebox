#pragma once

#include <glib.h>

/* *INDENT-OFF* */
G_BEGIN_DECLS

/*
 * Maps a fatal startup catch-up or catalog preparation failure to a sysexits
 * code. G_IO_ERROR_INVALID_DATA and G_IO_ERROR_NOT_SUPPORTED are permanent and
 * map to EX_DATAERR; every other error, and a NULL @error, maps to
 * EX_TEMPFAIL so a supervisor may restart the daemon.
 *
 * @error: (nullable) (transfer none): the startup failure.
 */
int wyrebox_daemon_exit_code_for_startup_error (const GError *error);

G_END_DECLS
/* *INDENT-ON* */
