#include "wyrebox-daemon-stream-chunk-frame.h"

#include <string.h>

static gboolean
is_empty_string (const char *value)
{
  return value == NULL || *value == '\0';
}

static gboolean
validate_text_field (const char *value, const char *field_name, GError **error)
{
  if (is_empty_string (value))
    return TRUE;

  for (const char *cursor = value; *cursor != '\0'; cursor++) {
    if (g_ascii_iscntrl (*cursor)) {
      g_set_error (error,
          G_IO_ERROR,
          G_IO_ERROR_INVALID_ARGUMENT,
          "stream chunk response %s must not contain control characters",
          field_name);
      return FALSE;
    }
  }

  return TRUE;
}

static void
copy_optional_string (char **dest, const char *src)
{
  if (!is_empty_string (src))
    *dest = g_strdup (src);
}

void
wyrebox_daemon_stream_chunk_frame_clear (WyreboxDaemonStreamChunkFrame *frame)
{
  if (frame == NULL)
    return;

  g_clear_pointer (&frame->request_id, g_free);
  g_clear_pointer (&frame->message_id, g_free);
  g_clear_pointer (&frame->query_id, g_free);
  g_clear_pointer (&frame->correlation_id, g_free);
  g_clear_pointer (&frame->bytes, g_bytes_unref);
  frame->chunk_index = 0;
  frame->end_of_stream = FALSE;
}

gboolean
wyrebox_daemon_stream_chunk_frame_init (WyreboxDaemonStreamChunkFrame *frame,
    const char *request_id, const char *message_id, const char *query_id,
    const char *correlation_id, guint64 chunk_index, GBytes *bytes,
    gboolean end_of_stream, GError **error)
{
  g_auto (WyreboxDaemonStreamChunkFrame) next = { 0 };
  const guint8 *data = NULL;
  gsize size = 0;
  gboolean has_message_id = !is_empty_string (message_id);
  gboolean has_query_id = !is_empty_string (query_id);

  g_return_val_if_fail (frame != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (is_empty_string (request_id)) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "stream chunk response requires request_id");
    return FALSE;
  }

  if (has_message_id == has_query_id) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "stream chunk response requires exactly one of message_id or query_id");
    return FALSE;
  }

  if (!validate_text_field (request_id, "request_id", error))
    return FALSE;

  if (!validate_text_field (message_id, "message_id", error))
    return FALSE;

  if (!validate_text_field (query_id, "query_id", error))
    return FALSE;

  if (!validate_text_field (correlation_id, "correlation_id", error))
    return FALSE;

  if (bytes != NULL)
    data = g_bytes_get_data (bytes, &size);

  if (!end_of_stream && size == 0) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "non-final stream chunk response requires bytes");
    return FALSE;
  }

  if (data == NULL)
    data = (const guint8 *) "";

  next.request_id = g_strdup (request_id);
  copy_optional_string (&next.message_id, message_id);
  copy_optional_string (&next.query_id, query_id);
  copy_optional_string (&next.correlation_id, correlation_id);
  next.chunk_index = chunk_index;
  next.bytes = g_bytes_new (data, size);
  next.end_of_stream = end_of_stream;

  wyrebox_daemon_stream_chunk_frame_clear (frame);
  *frame = next;
  memset (&next, 0, sizeof (next));

  return TRUE;
}
