#include "wyrebox-daemon-response-frame.h"

#include <string.h>

#include <gio/gio.h>

void
wyrebox_daemon_response_frame_clear (WyreboxDaemonResponseFrame *frame)
{
  if (frame == NULL)
    return;

  g_clear_pointer (&frame->request_id, g_free);
  g_clear_pointer (&frame->correlation_id, g_free);
  frame->kind = WYREBOX_DAEMON_RESPONSE_FRAME_NONE;
  wyrebox_daemon_success_receipt_clear (&frame->success);
  wyrebox_daemon_error_frame_clear (&frame->error);
  wyrebox_daemon_stream_chunk_frame_clear (&frame->stream_chunk);
  wyrebox_daemon_mailbox_list_result_clear (&frame->mailbox_list);
  wyrebox_daemon_mailbox_select_result_clear (&frame->mailbox_select);
}

static gboolean
copy_success_receipt (WyreboxDaemonSuccessReceipt *dest,
    const WyreboxDaemonSuccessReceipt *src, GError **error)
{
  g_auto (WyreboxDaemonSuccessReceipt) next = { 0 };

  if (src->request_id == NULL || *src->request_id == '\0') {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT, "success response requires request_id");
    return FALSE;
  }

  if (src->durable_marker == NULL || *src->durable_marker == '\0') {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "success response requires durable marker");
    return FALSE;
  }

  if (src->journal_sequence == 0) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "success response requires durable journal sequence");
    return FALSE;
  }

  if (src->summary == NULL || *src->summary == '\0') {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT, "success response requires summary");
    return FALSE;
  }

  next.request_id = g_strdup (src->request_id);
  next.durable_marker = g_strdup (src->durable_marker);
  next.journal_offset = src->journal_offset;
  next.journal_sequence = src->journal_sequence;
  next.summary = g_strdup (src->summary);

  wyrebox_daemon_success_receipt_clear (dest);
  *dest = next;
  next.request_id = NULL;
  next.durable_marker = NULL;
  next.summary = NULL;

  return TRUE;
}

static gboolean
copy_error_frame (WyreboxDaemonErrorFrame *dest,
    const WyreboxDaemonErrorFrame *src, GError **error)
{
  return wyrebox_daemon_error_frame_init (dest,
      src->request_id, src->error_class, src->message, src->retry_hint, error);
}

static gboolean
copy_stream_chunk_frame (WyreboxDaemonStreamChunkFrame *dest,
    const WyreboxDaemonStreamChunkFrame *src, GError **error)
{
  return wyrebox_daemon_stream_chunk_frame_init (dest,
      src->request_id, src->message_id, src->query_id, src->correlation_id,
      src->chunk_index, src->bytes, src->end_of_stream, error);
}

static gboolean
copy_mailbox_list_result (WyreboxDaemonMailboxListResult *dest,
    const WyreboxDaemonMailboxListResult *src, GError **error)
{
  guint n_entries = 0;

  if (src->entries == NULL) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "mailbox LIST response requires initialized result");
    return FALSE;
  }

  wyrebox_daemon_mailbox_list_result_init_empty (dest);
  n_entries = wyrebox_daemon_mailbox_list_result_get_n_entries (src);

  for (guint i = 0; i < n_entries; i++) {
    const WyreboxDaemonMailboxListEntry *entry =
        wyrebox_daemon_mailbox_list_result_get_entry (src, i);

    if (entry == NULL) {
      g_set_error (error,
          G_IO_ERROR,
          G_IO_ERROR_INVALID_ARGUMENT,
          "mailbox LIST response contains invalid entry");
      return FALSE;
    }

    if (!wyrebox_daemon_mailbox_list_result_append_entry (dest,
            entry->kind, entry->mailbox_id, entry->mailbox_name,
            entry->hierarchy_delimiter, entry->special_use,
            entry->is_selectable, entry->child_state, error))
      return FALSE;
  }

  return TRUE;
}

static gboolean
copy_mailbox_select_result (WyreboxDaemonMailboxSelectResult *dest,
    const WyreboxDaemonMailboxSelectResult *src, GError **error)
{
  return wyrebox_daemon_mailbox_select_result_init (dest,
      src->kind, src->mailbox_id, src->mailbox_name, src->uid_validity,
      src->uid_next, error);
}

static gboolean
validate_required_request_id (const char *request_id, GError **error)
{
  if (request_id == NULL || *request_id == '\0') {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT, "response frame requires request_id");
    return FALSE;
  }

  return TRUE;
}

static void
copy_optional_string (char **dest, const char *src)
{
  if (src != NULL && *src != '\0')
    *dest = g_strdup (src);
}

gboolean
wyrebox_daemon_response_frame_init_success (WyreboxDaemonResponseFrame *frame,
    const WyreboxDaemonSuccessReceipt *success,
    const char *correlation_id, GError **error)
{
  g_auto (WyreboxDaemonResponseFrame) next = { 0 };

  g_return_val_if_fail (frame != NULL, FALSE);
  g_return_val_if_fail (success != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (!copy_success_receipt (&next.success, success, error))
    return FALSE;

  next.request_id = g_strdup (success->request_id);
  copy_optional_string (&next.correlation_id, correlation_id);
  next.kind = WYREBOX_DAEMON_RESPONSE_FRAME_SUCCESS;

  wyrebox_daemon_response_frame_clear (frame);
  *frame = next;
  next.request_id = NULL;
  next.correlation_id = NULL;
  next.kind = WYREBOX_DAEMON_RESPONSE_FRAME_NONE;
  memset (&next.success, 0, sizeof (next.success));

  return TRUE;
}

gboolean
wyrebox_daemon_response_frame_init_error (WyreboxDaemonResponseFrame *frame,
    const WyreboxDaemonErrorFrame *error_frame,
    const char *correlation_id, GError **error)
{
  g_auto (WyreboxDaemonResponseFrame) next = { 0 };

  g_return_val_if_fail (frame != NULL, FALSE);
  g_return_val_if_fail (error_frame != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (!copy_error_frame (&next.error, error_frame, error))
    return FALSE;

  next.request_id = g_strdup (error_frame->request_id);
  copy_optional_string (&next.correlation_id, correlation_id);
  next.kind = WYREBOX_DAEMON_RESPONSE_FRAME_ERROR;

  wyrebox_daemon_response_frame_clear (frame);
  *frame = next;
  next.request_id = NULL;
  next.correlation_id = NULL;
  next.kind = WYREBOX_DAEMON_RESPONSE_FRAME_NONE;
  memset (&next.error, 0, sizeof (next.error));

  return TRUE;
}

gboolean
wyrebox_daemon_response_frame_init_stream_chunk (WyreboxDaemonResponseFrame
    *frame, const WyreboxDaemonStreamChunkFrame *stream_chunk, GError **error)
{
  g_auto (WyreboxDaemonResponseFrame) next = { 0 };

  g_return_val_if_fail (frame != NULL, FALSE);
  g_return_val_if_fail (stream_chunk != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (!copy_stream_chunk_frame (&next.stream_chunk, stream_chunk, error))
    return FALSE;

  next.request_id = g_strdup (stream_chunk->request_id);
  copy_optional_string (&next.correlation_id, stream_chunk->correlation_id);
  next.kind = WYREBOX_DAEMON_RESPONSE_FRAME_STREAM_CHUNK;

  wyrebox_daemon_response_frame_clear (frame);
  *frame = next;
  next.request_id = NULL;
  next.correlation_id = NULL;
  next.kind = WYREBOX_DAEMON_RESPONSE_FRAME_NONE;
  memset (&next.stream_chunk, 0, sizeof (next.stream_chunk));

  return TRUE;
}

gboolean
wyrebox_daemon_response_frame_init_mailbox_list (WyreboxDaemonResponseFrame
    *frame, const char *request_id, const char *correlation_id,
    const WyreboxDaemonMailboxListResult *mailbox_list, GError **error)
{
  g_auto (WyreboxDaemonResponseFrame) next = { 0 };

  g_return_val_if_fail (frame != NULL, FALSE);
  g_return_val_if_fail (mailbox_list != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (!validate_required_request_id (request_id, error))
    return FALSE;

  if (!copy_mailbox_list_result (&next.mailbox_list, mailbox_list, error))
    return FALSE;

  next.request_id = g_strdup (request_id);
  copy_optional_string (&next.correlation_id, correlation_id);
  next.kind = WYREBOX_DAEMON_RESPONSE_FRAME_MAILBOX_LIST;

  wyrebox_daemon_response_frame_clear (frame);
  *frame = next;
  next.request_id = NULL;
  next.correlation_id = NULL;
  next.kind = WYREBOX_DAEMON_RESPONSE_FRAME_NONE;
  memset (&next.mailbox_list, 0, sizeof (next.mailbox_list));

  return TRUE;
}

gboolean
wyrebox_daemon_response_frame_init_mailbox_select (WyreboxDaemonResponseFrame
    *frame, const char *request_id, const char *correlation_id,
    const WyreboxDaemonMailboxSelectResult *mailbox_select, GError **error)
{
  g_auto (WyreboxDaemonResponseFrame) next = { 0 };

  g_return_val_if_fail (frame != NULL, FALSE);
  g_return_val_if_fail (mailbox_select != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (!validate_required_request_id (request_id, error))
    return FALSE;

  if (!copy_mailbox_select_result (&next.mailbox_select, mailbox_select, error))
    return FALSE;

  next.request_id = g_strdup (request_id);
  copy_optional_string (&next.correlation_id, correlation_id);
  next.kind = WYREBOX_DAEMON_RESPONSE_FRAME_MAILBOX_SELECT;

  wyrebox_daemon_response_frame_clear (frame);
  *frame = next;
  next.request_id = NULL;
  next.correlation_id = NULL;
  next.kind = WYREBOX_DAEMON_RESPONSE_FRAME_NONE;
  memset (&next.mailbox_select, 0, sizeof (next.mailbox_select));

  return TRUE;
}

gboolean
    wyrebox_daemon_response_frame_init_fact_mutation_success
    (WyreboxDaemonResponseFrame * frame, const char *request_id,
    const char *correlation_id,
    const WyreboxDaemonFactMutationRequest * request, guint64 journal_offset,
    guint64 journal_sequence, GError ** error)
{
  g_auto (WyreboxDaemonSuccessReceipt) receipt = { 0 };

  g_return_val_if_fail (frame != NULL, FALSE);
  g_return_val_if_fail (request != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (!wyrebox_daemon_success_receipt_init_fact_mutation (&receipt,
          request_id, request, journal_offset, journal_sequence, error))
    return FALSE;

  return wyrebox_daemon_response_frame_init_success (frame,
      &receipt, correlation_id, error);
}
