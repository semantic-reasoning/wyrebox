#include "wyrebox-daemon-message-fetch-service.h"

#include <gio/gio.h>

struct _WyreboxDaemonMessageFetchService
{
  GObject parent_instance;

  WyreboxDaemonMessageFetchServiceFunc func;
  gpointer user_data;
  GDestroyNotify user_data_destroy;
};

G_DEFINE_TYPE (WyreboxDaemonMessageFetchService,
    wyrebox_daemon_message_fetch_service, G_TYPE_OBJECT);

static gboolean
caller_can_fetch_message (const char *caller_identity)
{
  return g_strcmp0 (caller_identity, "dovecot") == 0;
}

static gboolean
authorize_message_fetch_identity (const WyreboxDaemonRequestIdentity *identity,
    const WyreboxDaemonMessageFetchRequest *request, GError **error)
{
  if (!caller_can_fetch_message (identity->caller_identity)) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_PERMISSION_DENIED,
        "caller is not authorized to fetch messages");
    return FALSE;
  }

  if (identity->account_identity == NULL ||
      g_strcmp0 (identity->account_identity, request->account_identity) != 0) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_PERMISSION_DENIED,
        "caller is not authorized for message fetch account scope");
    return FALSE;
  }

  return TRUE;
}

static gboolean
validate_message_fetch_chunk (const WyreboxDaemonRequestIdentity *identity,
    const WyreboxDaemonStreamChunkFrame *chunk, GError **error)
{
  if (chunk->request_id == NULL ||
      g_strcmp0 (chunk->request_id, identity->request_id) != 0) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "message FETCH stream chunk request_id must match request envelope");
    return FALSE;
  }

  if (chunk->message_id == NULL || *chunk->message_id == '\0') {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "message FETCH stream chunk requires message_id");
    return FALSE;
  }

  if (chunk->query_id != NULL && *chunk->query_id != '\0') {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "message FETCH stream chunk must not contain query_id");
    return FALSE;
  }

  if (chunk->correlation_id != NULL &&
      g_strcmp0 (chunk->correlation_id, identity->correlation_id) != 0) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "message FETCH stream chunk correlation_id must match request envelope");
    return FALSE;
  }

  return TRUE;
}

static void
wyrebox_daemon_message_fetch_service_finalize (GObject *object)
{
  WyreboxDaemonMessageFetchService *self =
      WYREBOX_DAEMON_MESSAGE_FETCH_SERVICE (object);

  if (self->user_data_destroy != NULL && self->user_data != NULL)
    self->user_data_destroy (self->user_data);

  G_OBJECT_CLASS (wyrebox_daemon_message_fetch_service_parent_class)->finalize
      (object);
}

static void
    wyrebox_daemon_message_fetch_service_class_init
    (WyreboxDaemonMessageFetchServiceClass * klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = wyrebox_daemon_message_fetch_service_finalize;
}

static void
wyrebox_daemon_message_fetch_service_init (WyreboxDaemonMessageFetchService
    *self)
{
  (void) self;
}

WyreboxDaemonMessageFetchService *
wyrebox_daemon_message_fetch_service_new (WyreboxDaemonMessageFetchServiceFunc
    func, gpointer user_data, GDestroyNotify user_data_destroy)
{
  g_return_val_if_fail (func != NULL, NULL);

  WyreboxDaemonMessageFetchService *self =
      g_object_new (WYREBOX_TYPE_DAEMON_MESSAGE_FETCH_SERVICE, NULL);

  self->func = func;
  self->user_data = user_data;
  self->user_data_destroy = user_data_destroy;

  return self;
}

gboolean
    wyrebox_daemon_message_fetch_service_handle_identity
    (WyreboxDaemonMessageFetchService * self,
    const WyreboxDaemonRequestIdentity * identity,
    const WyreboxDaemonMessageFetchRequest * request,
    WyreboxDaemonResponseFrame * out_frame, GError ** error)
{
  g_auto (WyreboxDaemonStreamChunkFrame) chunk = { 0 };
  g_auto (WyreboxDaemonStreamChunkFrame) response_chunk = { 0 };
  g_autoptr (GError) local_error = NULL;

  g_return_val_if_fail (WYREBOX_IS_DAEMON_MESSAGE_FETCH_SERVICE (self), FALSE);
  g_return_val_if_fail (identity != NULL, FALSE);
  g_return_val_if_fail (request != NULL, FALSE);
  g_return_val_if_fail (out_frame != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (!authorize_message_fetch_identity (identity, request, error))
    return FALSE;

  if (!self->func (identity, request, &chunk, self->user_data, &local_error)) {
    if (local_error == NULL) {
      g_set_error (&local_error,
          G_IO_ERROR,
          G_IO_ERROR_FAILED,
          "message FETCH service failed without error detail");
    }
    g_propagate_error (error, g_steal_pointer (&local_error));
    return FALSE;
  }

  if (!validate_message_fetch_chunk (identity, &chunk, error))
    return FALSE;

  if (!wyrebox_daemon_stream_chunk_frame_init (&response_chunk,
          identity->request_id,
          chunk.message_id,
          NULL,
          identity->correlation_id,
          chunk.chunk_index, chunk.bytes, chunk.end_of_stream, error))
    return FALSE;

  return wyrebox_daemon_response_frame_init_stream_chunk (out_frame,
      &response_chunk, error);
}
