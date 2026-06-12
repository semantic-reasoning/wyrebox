#include "wyrebox-daemon-message-fetch-request.h"

#include <gio/gio.h>

static gboolean
validate_text (const char *value, const char *field_name, GError **error)
{
  if (value == NULL || *value == '\0') {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "message FETCH %s is required", field_name);
    return FALSE;
  }

  for (const char *cursor = value; *cursor != '\0'; cursor++) {
    if (g_ascii_iscntrl (*cursor)) {
      g_set_error (error,
          G_IO_ERROR,
          G_IO_ERROR_INVALID_ARGUMENT,
          "message FETCH %s must not contain control characters", field_name);
      return FALSE;
    }
  }

  return TRUE;
}

void
wyrebox_daemon_message_fetch_request_clear (WyreboxDaemonMessageFetchRequest
    *request)
{
  if (request == NULL)
    return;

  g_clear_pointer (&request->account_identity, g_free);
  g_clear_pointer (&request->mailbox_id, g_free);
  request->uid_validity = 0;
  request->mailbox_uid = 0;
}

gboolean
wyrebox_daemon_message_fetch_request_init (WyreboxDaemonMessageFetchRequest
    *request, const char *account_identity, const char *mailbox_id,
    guint64 uid_validity, guint64 mailbox_uid, GError **error)
{
  g_auto (WyreboxDaemonMessageFetchRequest) next = { 0 };

  g_return_val_if_fail (request != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (!validate_text (account_identity, "account_identity", error))
    return FALSE;

  if (!validate_text (mailbox_id, "mailbox_id", error))
    return FALSE;

  if (uid_validity == 0) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT, "message FETCH uid_validity is required");
    return FALSE;
  }

  if (mailbox_uid == 0) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT, "message FETCH mailbox_uid is required");
    return FALSE;
  }

  next.account_identity = g_strdup (account_identity);
  next.mailbox_id = g_strdup (mailbox_id);
  next.uid_validity = uid_validity;
  next.mailbox_uid = mailbox_uid;

  wyrebox_daemon_message_fetch_request_clear (request);
  *request = next;
  next.account_identity = NULL;
  next.mailbox_id = NULL;

  return TRUE;
}
