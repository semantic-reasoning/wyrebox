#include "wyrebox-daemon-connection-session.h"
#include "wyrebox-daemon-frame-io.h"

#include <gio/gio.h>

struct _WyreboxDaemonConnectionSession
{
  GObject parent_instance;

  GSocketConnection *connection;
  WyreboxDaemonPeerCredentials peer_credentials;
  WyreboxDaemonConnectionSessionPayloadHandler payload_handler;
  gpointer payload_handler_data;
  GDestroyNotify payload_handler_destroy_notify;
};

G_DEFINE_TYPE (WyreboxDaemonConnectionSession,
    wyrebox_daemon_connection_session, G_TYPE_OBJECT);

static void
wyrebox_daemon_connection_session_finalize (GObject *object)
{
  WyreboxDaemonConnectionSession *self =
      WYREBOX_DAEMON_CONNECTION_SESSION (object);

  g_clear_object (&self->connection);
  if (self->payload_handler_destroy_notify != NULL)
    self->payload_handler_destroy_notify (self->payload_handler_data);

  G_OBJECT_CLASS (wyrebox_daemon_connection_session_parent_class)->finalize
      (object);
}

static void
    wyrebox_daemon_connection_session_class_init
    (WyreboxDaemonConnectionSessionClass * klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = wyrebox_daemon_connection_session_finalize;
}

static void
wyrebox_daemon_connection_session_init (WyreboxDaemonConnectionSession *self)
{
  (void) self;
}

WyreboxDaemonConnectionSession *
wyrebox_daemon_connection_session_new (GSocketConnection *connection,
    const WyreboxDaemonPeerCredentials *peer_credentials,
    WyreboxDaemonConnectionSessionPayloadHandler payload_handler,
    gpointer payload_handler_data,
    GDestroyNotify payload_handler_destroy_notify)
{
  g_return_val_if_fail (connection != NULL, NULL);
  g_return_val_if_fail (peer_credentials != NULL, NULL);
  g_return_val_if_fail (payload_handler != NULL, NULL);

  WyreboxDaemonConnectionSession *self =
      g_object_new (WYREBOX_TYPE_DAEMON_CONNECTION_SESSION, NULL);
  self->connection = g_object_ref (connection);
  self->peer_credentials = *peer_credentials;
  self->payload_handler = payload_handler;
  self->payload_handler_data = payload_handler_data;
  self->payload_handler_destroy_notify = payload_handler_destroy_notify;

  return self;
}

gboolean
    wyrebox_daemon_connection_session_process_payloads
    (WyreboxDaemonConnectionSession * self, GError ** error) {
  GIOStream *stream = NULL;
  GInputStream *input = NULL;
  GOutputStream *output = NULL;
  g_autoptr (GError) payload_error = NULL;

  g_return_val_if_fail (WYREBOX_IS_DAEMON_CONNECTION_SESSION (self), FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  stream = G_IO_STREAM (self->connection);
  input = g_io_stream_get_input_stream (stream);
  output = g_io_stream_get_output_stream (stream);
  if (input == NULL || output == NULL) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_CLOSED,
        "daemon connection session requires open input/output streams");
    return FALSE;
  }

  while (TRUE) {
    g_autoptr (GBytes) request = NULL;
    g_autoptr (GBytes) response = NULL;
    g_autoptr (GError) callback_error = NULL;
    gboolean eof = FALSE;

    if (!wyrebox_daemon_frame_io_read_payload_or_eof (input, &request, &eof,
            &payload_error))
      goto read_failed;

    if (eof)
      return TRUE;

    response = self->payload_handler (&self->peer_credentials, request,
        self->payload_handler_data, &callback_error);
    if (response == NULL) {
      if (callback_error == NULL) {
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
            "daemon connection session callback failed without details");
      } else {
        g_propagate_error (error, g_steal_pointer (&callback_error));
      }
      return FALSE;
    }

    gsize payload_size = 0;
    const guint8 *payload_data = g_bytes_get_data (response, &payload_size);
    if (!wyrebox_daemon_frame_io_write_payload (output, payload_data,
            payload_size, &callback_error)) {
      g_propagate_error (error, g_steal_pointer (&callback_error));
      return FALSE;
    }
  }

read_failed:
  g_propagate_error (error, g_steal_pointer (&payload_error));
  return FALSE;
}
