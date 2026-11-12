#pragma once

#include <gio/gio.h>

#include <glib.h>

/* *INDENT-OFF* */
G_BEGIN_DECLS

#define WYREBOX_DAEMON_UDS_CLIENT_DEFAULT_TIMEOUT_SEC ((guint)30)
#define WYREBOX_DAEMON_UDS_TIMEOUT_ENV "WYREBOX_DAEMON_UDS_TIMEOUT_SEC"

/*
 * Idle I/O timeout in seconds for daemon UDS calls. Unset, empty, zero, or
 * non-numeric WYREBOX_DAEMON_UDS_TIMEOUT_SEC values fall back to
 * WYREBOX_DAEMON_UDS_CLIENT_DEFAULT_TIMEOUT_SEC. Zero (disable) is rejected.
 */
guint wyrebox_daemon_uds_client_resolve_timeout_sec (void);

/*
 * Sends exactly one already-serialized daemon request payload over @socket_path
 * and returns exactly one response payload.
 *
 * Connect, send, and receive use an idle timeout from
 * wyrebox_daemon_uds_client_resolve_timeout_sec(). A stalled peer surfaces as
 * G_IO_ERROR_TIMED_OUT.
 *
 * @request_payload is borrowed for the duration of the call.
 *
 * Returns: (transfer full) on success: caller-owned response payload bytes.
 * Returns NULL on validation, I/O, framing, or timeout failure and sets
 * @error.
 */
GBytes *wyrebox_daemon_uds_client_send_request (
    const char *socket_path,
    GBytes *request_payload,
    GError **error);

G_END_DECLS
/* *INDENT-ON* */
