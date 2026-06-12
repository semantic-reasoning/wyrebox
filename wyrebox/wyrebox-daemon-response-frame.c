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
wyrebox_daemon_response_frame_init_fact_mutation_success (
    WyreboxDaemonResponseFrame *frame,
    const char *request_id,
    const char *correlation_id,
    const WyreboxDaemonFactMutationRequest *request,
    guint64 journal_offset,
    guint64 journal_sequence,
    GError **error)
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
