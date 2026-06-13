#pragma once

#include <gio/gio.h>

#include <glib.h>

/* *INDENT-OFF* */
G_BEGIN_DECLS

/*
 * Sends exactly one already-serialized daemon request payload over @socket_path
 * and returns exactly one response payload.
 *
 * @request_payload is borrowed for the duration of the call.
 *
 * Returns: (transfer full) on success: caller-owned response payload bytes.
 * Returns NULL on validation, I/O, or framing failure and sets @error.
 */
GBytes *wyrebox_daemon_uds_client_send_request (
    const char *socket_path,
    GBytes *request_payload,
    GError **error);

G_END_DECLS
/* *INDENT-ON* */
