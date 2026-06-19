#include "wyrebox-daemon-mail-event-stream-request.h"

#include <gio/gio.h>

static gboolean
validate_required_text (const char *value,
    const char *field_name, GError **error)
{
  if (value == NULL || *value == '\0') {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "mail event stream %s is required", field_name);
    return FALSE;
  }

  for (const char *cursor = value; *cursor != '\0'; cursor++) {
    if (g_ascii_iscntrl (*cursor)) {
      g_set_error (error,
          G_IO_ERROR,
          G_IO_ERROR_INVALID_ARGUMENT,
          "mail event stream %s must not contain control characters",
          field_name);
      return FALSE;
    }
  }

  return TRUE;
}

static gboolean
validate_journal_cursor (guint64 after_journal_offset,
    guint64 after_journal_sequence, GError **error)
{
  if (after_journal_offset != 0 && after_journal_sequence == 0) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "mail event stream cursor requires a non-zero afterJournalSequence "
        "when afterJournalOffset is non-zero");
    return FALSE;
  }

  return TRUE;
}

void wyrebox_daemon_mail_event_stream_request_clear
    (WyreboxDaemonMailEventStreamRequest * request)
{
  if (request == NULL)
    return;

  g_clear_pointer (&request->account_identity, g_free);
  request->after_journal_offset = 0;
  request->after_journal_sequence = 0;
}

gboolean
    wyrebox_daemon_mail_event_stream_request_init
    (WyreboxDaemonMailEventStreamRequest * request,
    const char *account_identity, guint64 after_journal_offset,
    guint64 after_journal_sequence, GError ** error)
{
  g_auto (WyreboxDaemonMailEventStreamRequest) next = { 0 };

  g_return_val_if_fail (request != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (!validate_required_text (account_identity, "account_identity", error))
    return FALSE;

  if (!validate_journal_cursor (after_journal_offset, after_journal_sequence,
          error))
    return FALSE;

  next.account_identity = g_strdup (account_identity);
  next.after_journal_offset = after_journal_offset;
  next.after_journal_sequence = after_journal_sequence;

  wyrebox_daemon_mail_event_stream_request_clear (request);
  *request = next;
  next.account_identity = NULL;

  return TRUE;
}
