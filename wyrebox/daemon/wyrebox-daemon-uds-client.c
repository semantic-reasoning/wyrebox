#include "wyrebox-daemon-uds-client.h"

#include "wyrebox-daemon-frame-io.h"

#include <gio/gio.h>
#include <gio/gunixsocketaddress.h>

guint
wyrebox_daemon_uds_client_resolve_timeout_sec (void)
{
    const char *value = g_getenv (WYREBOX_DAEMON_UDS_TIMEOUT_ENV);
    gchar *end = NULL;
    gint64 parsed = 0;

    if (value == NULL || value[0] == '\0')
        return WYREBOX_DAEMON_UDS_CLIENT_DEFAULT_TIMEOUT_SEC;

    parsed = g_ascii_strtoll (value, &end, 10);
    if (end == value || *end != '\0' || parsed <= 0 || parsed > G_MAXUINT)
        return WYREBOX_DAEMON_UDS_CLIENT_DEFAULT_TIMEOUT_SEC;

    return (guint)parsed;
}

GBytes *
wyrebox_daemon_uds_client_send_request (const char *socket_path,
    GBytes *request_payload, GError **error)
{
    g_autoptr (GSocketClient) client = NULL;
    g_autoptr (GSocketAddress) address = NULL;
    g_autoptr (GSocketConnection) connection = NULL;
    GInputStream *input = NULL;
    GOutputStream *output = NULL;
    GSocket *socket = NULL;
    const guint8 *request_data = NULL;
    gsize request_size = 0;
    g_autoptr (GBytes) response = NULL;
    guint timeout_sec = wyrebox_daemon_uds_client_resolve_timeout_sec ();

    g_return_val_if_fail (error == NULL || *error == NULL, NULL);

    if (socket_path == NULL || socket_path[0] == '\0') {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_INVALID_ARGUMENT,
            "daemon UDS client requires a non-empty socket path");
        return NULL;
    }

    if (request_payload == NULL) {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_INVALID_ARGUMENT,
            "daemon UDS client request payload is required");
        return NULL;
    }

    request_data = g_bytes_get_data (request_payload, &request_size);
    if (request_size == 0) {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_INVALID_ARGUMENT,
            "daemon UDS client request payload must be non-empty");
        return NULL;
    }

    client = g_socket_client_new ();
    g_socket_client_set_timeout (client, timeout_sec);
    address = g_unix_socket_address_new (socket_path);
    connection = g_socket_client_connect (client,
            G_SOCKET_CONNECTABLE (address), NULL, error);
    if (connection == NULL)
        return NULL;

    input = g_io_stream_get_input_stream (G_IO_STREAM (connection));
    output = g_io_stream_get_output_stream (G_IO_STREAM (connection));
    if (input == NULL || output == NULL) {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_CLOSED, "daemon UDS client connection is not open");
        return NULL;
    }

    socket = g_socket_connection_get_socket (connection);
    if (socket == NULL) {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_CLOSED, "daemon UDS client connection has no socket");
        return NULL;
    }
    g_socket_set_timeout (socket, timeout_sec);

    if (!wyrebox_daemon_frame_io_write_payload (output, request_data,
        request_size, error))
        return NULL;

    if (!g_socket_shutdown (socket, FALSE, TRUE, error))
        return NULL;

    response = wyrebox_daemon_frame_io_read_payload (input, error);
    if (response == NULL)
        return NULL;

    if (!g_io_stream_close (G_IO_STREAM (connection), NULL, error))
        return NULL;

    return g_steal_pointer (&response);
}
