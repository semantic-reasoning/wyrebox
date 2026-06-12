#include "wyrebox-daemon-mailbox-select-result.h"

#include <gio/gio.h>

static gboolean
validate_required_text (const char *value, const char *field_name,
    GError **error)
{
  if (value == NULL || *value == '\0') {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT, "mailbox SELECT %s is required",
        field_name);
    return FALSE;
  }

  for (const char *cursor = value; *cursor != '\0'; cursor++) {
    if (g_ascii_iscntrl (*cursor)) {
      g_set_error (error,
          G_IO_ERROR,
          G_IO_ERROR_INVALID_ARGUMENT,
          "mailbox SELECT %s must not contain control characters", field_name);
      return FALSE;
    }
  }

  return TRUE;
}

static gboolean
validate_kind (WyreboxDaemonMailboxListEntryKind kind, GError **error)
{
  switch (kind) {
    case WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_ORDINARY:
    case WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_VIRTUAL:
      return TRUE;
    default:
      g_set_error (error,
          G_IO_ERROR,
          G_IO_ERROR_INVALID_ARGUMENT, "mailbox SELECT kind is invalid");
      return FALSE;
  }
}

void
wyrebox_daemon_mailbox_select_result_clear (WyreboxDaemonMailboxSelectResult
    *result)
{
  if (result == NULL)
    return;

  g_clear_pointer (&result->mailbox_id, g_free);
  g_clear_pointer (&result->mailbox_name, g_free);
  result->kind = WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_ORDINARY;
  result->uid_validity = 0;
  result->uid_next = 0;
}

gboolean
wyrebox_daemon_mailbox_select_result_init (WyreboxDaemonMailboxSelectResult
    *result, WyreboxDaemonMailboxListEntryKind kind, const char *mailbox_id,
    const char *mailbox_name, guint32 uid_validity, guint32 uid_next,
    GError **error)
{
  g_auto (WyreboxDaemonMailboxSelectResult) next = { 0 };

  g_return_val_if_fail (result != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (!validate_kind (kind, error))
    return FALSE;

  if (!validate_required_text (mailbox_id, "mailbox_id", error))
    return FALSE;

  if (!validate_required_text (mailbox_name, "mailbox_name", error))
    return FALSE;

  if (uid_validity == 0) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "mailbox SELECT uidvalidity must be non-zero");
    return FALSE;
  }

  if (uid_next == 0) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT, "mailbox SELECT uidnext must be non-zero");
    return FALSE;
  }

  next.kind = kind;
  next.mailbox_id = g_strdup (mailbox_id);
  next.mailbox_name = g_strdup (mailbox_name);
  next.uid_validity = uid_validity;
  next.uid_next = uid_next;

  wyrebox_daemon_mailbox_select_result_clear (result);
  *result = next;
  next.mailbox_id = NULL;
  next.mailbox_name = NULL;

  return TRUE;
}
