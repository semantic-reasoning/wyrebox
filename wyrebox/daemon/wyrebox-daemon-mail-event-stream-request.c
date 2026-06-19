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
  g_clear_pointer (&request->mailbox_identity, g_free);
  g_clear_pointer (&request->message_id, g_free);
  g_clear_pointer (&request->event_type, g_free);
  request->after_journal_offset = 0;
  request->after_journal_sequence = 0;
  request->after_unix_us = 0;
  request->before_unix_us = 0;
}

gboolean
    wyrebox_daemon_mail_event_stream_request_init
    (WyreboxDaemonMailEventStreamRequest * request,
    const char *account_identity, const char *mailbox_identity,
    const char *message_id, const char *event_type,
    guint64 after_journal_offset, guint64 after_journal_sequence,
    guint64 after_unix_us, guint64 before_unix_us, GError ** error)
{
  g_auto (WyreboxDaemonMailEventStreamRequest) next = { 0 };

  g_return_val_if_fail (request != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (!validate_required_text (account_identity, "account_identity", error))
    return FALSE;

  if (mailbox_identity != NULL &&
      !validate_required_text (mailbox_identity, "mailbox_identity", error))
    return FALSE;

  if (message_id != NULL && !validate_required_text (message_id, "message_id",
          error))
    return FALSE;

  if (event_type != NULL &&
      !validate_required_text (event_type, "event_type", error))
    return FALSE;

  if (!validate_journal_cursor (after_journal_offset, after_journal_sequence,
          error))
    return FALSE;

  if (after_unix_us != 0 && before_unix_us != 0 &&
      after_unix_us > before_unix_us) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "mail event stream after_unix_us must not exceed before_unix_us");
    return FALSE;
  }

  next.account_identity = g_strdup (account_identity);
  next.mailbox_identity = g_strdup (mailbox_identity);
  next.message_id = g_strdup (message_id);
  next.event_type = g_strdup (event_type);
  next.after_journal_offset = after_journal_offset;
  next.after_journal_sequence = after_journal_sequence;
  next.after_unix_us = after_unix_us;
  next.before_unix_us = before_unix_us;

  wyrebox_daemon_mail_event_stream_request_clear (request);
  *request = next;
  next.account_identity = NULL;
  next.mailbox_identity = NULL;
  next.message_id = NULL;
  next.event_type = NULL;

  return TRUE;
}
