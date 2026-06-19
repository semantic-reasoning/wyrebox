#include "wyrebox-daemon-mail-event-stream-service.h"
#include "wyrebox-daemon-client-identity.h"
#include "wyrebox-journal-event.h"
#include "wyrebox-journal-reader.h"

#include <gio/gio.h>

typedef struct
{
  char *journal_root_dir;
} WyreboxDaemonMailEventStreamJournalSource;

struct _WyreboxDaemonMailEventStreamService
{
  GObject parent_instance;

  WyreboxDaemonMailEventStreamServiceFunc func;
  gpointer user_data;
  GDestroyNotify user_data_destroy;
};

G_DEFINE_TYPE (WyreboxDaemonMailEventStreamService,
    wyrebox_daemon_mail_event_stream_service, G_TYPE_OBJECT);

static gboolean
authorize_mail_event_stream_identity (const WyreboxDaemonRequestIdentity
    *identity, const WyreboxDaemonMailEventStreamRequest *request,
    GError **error)
{
  WyreboxDaemonClientIdentityClass identity_class =
      wyrebox_daemon_client_identity_classify_request (identity);

  if (!wyrebox_daemon_client_identity_can_read_mail_events (identity_class)) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_PERMISSION_DENIED,
        "caller is not authorized to read mail events");
    return FALSE;
  }

  if (identity->account_identity == NULL ||
      g_strcmp0 (identity->account_identity, request->account_identity) != 0) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_PERMISSION_DENIED,
        "caller is not authorized for mail event stream account scope");
    return FALSE;
  }

  return TRUE;
}

static gboolean
validate_mail_event_stream_chunk (const WyreboxDaemonRequestIdentity *identity,
    const WyreboxDaemonStreamChunkFrame *chunk, GError **error)
{
  if (chunk->request_id == NULL || *chunk->request_id == '\0') {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "mail event stream chunk request_id is required");
    return FALSE;
  }

  if (g_strcmp0 (chunk->request_id, identity->request_id) != 0) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "mail event stream chunk request_id must match request envelope");
    return FALSE;
  }

  if (chunk->message_id != NULL && *chunk->message_id != '\0') {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "mail event stream chunk must not contain message_id");
    return FALSE;
  }

  if (chunk->query_id != NULL && *chunk->query_id != '\0' &&
      g_strcmp0 (chunk->query_id, "mail-event-stream") != 0) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "mail event stream chunk query_id must match mail-event-stream");
    return FALSE;
  }

  if (chunk->correlation_id != NULL
      && g_strcmp0 (chunk->correlation_id, identity->correlation_id) != 0) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "mail event stream chunk correlation_id must match request envelope");
    return FALSE;
  }

  return TRUE;
}

static void
    wyrebox_daemon_mail_event_stream_journal_source_free
    (WyreboxDaemonMailEventStreamJournalSource * source)
{
  if (source == NULL)
    return;

  g_clear_pointer (&source->journal_root_dir, g_free);
  g_free (source);
}

static gboolean
journal_event_matches_request (const WyreboxJournalRecord *record,
    const WyreboxDaemonMailEventStreamRequest *request)
{
  if (record->offset < request->after_journal_offset)
    return FALSE;

  if (request->after_journal_offset != 0 &&
      record->offset == request->after_journal_offset &&
      record->sequence <= request->after_journal_sequence)
    return FALSE;

  if (request->event_type != NULL &&
      g_strcmp0 (request->event_type,
          wyrebox_journal_event_type_to_string (record->event_type)) != 0)
    return FALSE;

  return TRUE;
}

static gboolean
read_next_matching_journal_event (const WyreboxDaemonRequestIdentity *identity,
    const WyreboxDaemonMailEventStreamRequest *request,
    WyreboxDaemonStreamChunkFrame *out_chunk,
    gpointer user_data, GError **error)
{
  WyreboxDaemonMailEventStreamJournalSource *source = user_data;
  g_autoptr (WyreboxJournalReader) reader = NULL;
  g_auto (WyreboxJournalRecord) record = { 0 };
  g_autoptr (GError) local_error = NULL;
  gboolean eof = FALSE;

  reader = wyrebox_journal_reader_new (source->journal_root_dir, error);
  if (reader == NULL)
    return FALSE;

  while (wyrebox_journal_reader_read_next (reader, &record, &eof, &local_error)) {
    if (!journal_event_matches_request (&record, request)) {
      wyrebox_journal_record_clear (&record);
      if (eof)
        break;
      continue;
    }

    if (record.payload == NULL) {
      g_set_error (error,
          G_IO_ERROR,
          G_IO_ERROR_INVALID_DATA,
          "mail event stream record payload is missing");
      return FALSE;
    }

    g_bytes_ref (record.payload);
    return wyrebox_daemon_stream_chunk_frame_init (out_chunk,
        identity->request_id,
        NULL,
        "mail-event-stream",
        identity->correlation_id, record.offset, record.payload, eof, error);
  }

  if (local_error != NULL) {
    g_propagate_error (error, g_steal_pointer (&local_error));
    return FALSE;
  }

  return wyrebox_daemon_stream_chunk_frame_init (out_chunk,
      identity->request_id,
      NULL,
      "mail-event-stream", identity->correlation_id, 0, NULL, TRUE, error);
}

static void
wyrebox_daemon_mail_event_stream_service_finalize (GObject *object)
{
  WyreboxDaemonMailEventStreamService *self =
      WYREBOX_DAEMON_MAIL_EVENT_STREAM_SERVICE (object);

  if (self->user_data_destroy != NULL && self->user_data != NULL)
    self->user_data_destroy (self->user_data);

  G_OBJECT_CLASS
      (wyrebox_daemon_mail_event_stream_service_parent_class)->finalize
      (object);
}

static void
    wyrebox_daemon_mail_event_stream_service_class_init
    (WyreboxDaemonMailEventStreamServiceClass * klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = wyrebox_daemon_mail_event_stream_service_finalize;
}

static void
    wyrebox_daemon_mail_event_stream_service_init
    (WyreboxDaemonMailEventStreamService * self)
{
  (void) self;
}

WyreboxDaemonMailEventStreamService
    * wyrebox_daemon_mail_event_stream_service_new
    (WyreboxDaemonMailEventStreamServiceFunc func, gpointer user_data,
    GDestroyNotify user_data_destroy) {
  g_return_val_if_fail (func != NULL, NULL);

  WyreboxDaemonMailEventStreamService *self =
      g_object_new (WYREBOX_TYPE_DAEMON_MAIL_EVENT_STREAM_SERVICE, NULL);

  self->func = func;
  self->user_data = user_data;
  self->user_data_destroy = user_data_destroy;

  return self;
}

WyreboxDaemonMailEventStreamService *
wyrebox_daemon_mail_event_stream_service_new_from_journal_root (const char
    *journal_root_dir, GError **error)
{
  WyreboxDaemonMailEventStreamJournalSource *source = NULL;

  g_return_val_if_fail (error == NULL || *error == NULL, NULL);

  if (journal_root_dir == NULL || *journal_root_dir == '\0') {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "mail event stream journal root is required");
    return NULL;
  }

  source = g_new0 (WyreboxDaemonMailEventStreamJournalSource, 1);
  source->journal_root_dir = g_strdup (journal_root_dir);

  WyreboxDaemonMailEventStreamService *service =
      wyrebox_daemon_mail_event_stream_service_new
      (read_next_matching_journal_event, source,
      (GDestroyNotify) wyrebox_daemon_mail_event_stream_journal_source_free);
  source = NULL;
  return service;
}

gboolean
    wyrebox_daemon_mail_event_stream_service_handle_identity
    (WyreboxDaemonMailEventStreamService * self,
    const WyreboxDaemonRequestIdentity * identity,
    const WyreboxDaemonMailEventStreamRequest * request,
    WyreboxDaemonResponseFrame * out_frame, GError ** error)
{
  g_auto (WyreboxDaemonStreamChunkFrame) chunk = { 0 };
  g_auto (WyreboxDaemonStreamChunkFrame) response_chunk = { 0 };
  g_autoptr (GError) local_error = NULL;

  g_return_val_if_fail (WYREBOX_IS_DAEMON_MAIL_EVENT_STREAM_SERVICE (self),
      FALSE);
  g_return_val_if_fail (identity != NULL, FALSE);
  g_return_val_if_fail (request != NULL, FALSE);
  g_return_val_if_fail (out_frame != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (!authorize_mail_event_stream_identity (identity, request, error))
    return FALSE;

  if (!self->func (identity, request, &chunk, self->user_data, &local_error)) {
    if (local_error == NULL) {
      g_set_error (&local_error,
          G_IO_ERROR,
          G_IO_ERROR_FAILED,
          "mail event stream service failed without error detail");
    }

    g_propagate_error (error, g_steal_pointer (&local_error));
    return FALSE;
  }

  if (!validate_mail_event_stream_chunk (identity, &chunk, error))
    return FALSE;

  if (!wyrebox_daemon_stream_chunk_frame_init (&response_chunk,
          identity->request_id,
          NULL,
          "mail-event-stream",
          identity->correlation_id,
          chunk.chunk_index, chunk.bytes, chunk.end_of_stream, error))
    return FALSE;

  return wyrebox_daemon_response_frame_init_stream_chunk (out_frame,
      &response_chunk, error);
}
