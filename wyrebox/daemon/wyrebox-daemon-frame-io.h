#pragma once

#include <gio/gio.h>

#include <glib.h>

#define WYREBOX_DAEMON_MAX_FRAME_SIZE_BYTES  (4 * 1024 * 1024)

/* *INDENT-OFF* */
G_BEGIN_DECLS

/*
 * Returns: (transfer full) on success: caller-owned payload bytes.
 * Returns NULL on failure and sets @error.
 */
GBytes *wyrebox_daemon_frame_io_read_payload (GInputStream *stream,
    GError **error);

/*
 * Returns TRUE on success, FALSE on failure.
 */
gboolean wyrebox_daemon_frame_io_write_payload (GOutputStream *stream,
    const guint8 *payload, gsize payload_size, GError **error);

G_END_DECLS
/* *INDENT-ON* */
