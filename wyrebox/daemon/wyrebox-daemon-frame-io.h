#pragma once

#include <gio/gio.h>

#include <glib.h>

#define WYREBOX_DAEMON_MAX_FRAME_SIZE_BYTES  (4 * 1024 * 1024)

/* *INDENT-OFF* */
G_BEGIN_DECLS

/*
 * Returns: (transfer full) on success: caller-owned payload bytes.
 * Returns NULL on protocol failure and sets @error.
 */
GBytes *wyrebox_daemon_frame_io_read_payload (GInputStream *stream,
    GError **error);

/*
 * Returns TRUE on success, FALSE on protocol or I/O errors.
 *
 * @out_payload must point to a valid #GBytes pointer (%NULL or owned); any
 * existing value is unreffed and cleared before the read so stale output never
 * survives a failed or EOF read.
 *
 * A clean EOF before a frame is reported as successful read with @out_payload set
 * to %NULL and @out_eof set to %TRUE.
 */
gboolean wyrebox_daemon_frame_io_read_payload_or_eof (
    GInputStream *stream,
    GBytes **out_payload,
    gboolean *out_eof,
    GError **error);

/*
 * Returns TRUE on success, FALSE on failure.
 */
gboolean wyrebox_daemon_frame_io_write_payload (GOutputStream *stream,
    const guint8 *payload, gsize payload_size, GError **error);

G_END_DECLS
/* *INDENT-ON* */
