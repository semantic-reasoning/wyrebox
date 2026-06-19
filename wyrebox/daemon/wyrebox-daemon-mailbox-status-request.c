#include "wyrebox-daemon-mailbox-status-request.h"

#include <gio/gio.h>
#include <string.h>

static gboolean
text_has_value (const char *value)
{
  return value != NULL && *value != '\0';
}

static gboolean
validate_text (const char *value, const char *field_name,
    gboolean is_required, GError **error)
{
  if (!text_has_value (value)) {
    if (!is_required)
      return TRUE;

    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT, "mailbox STATUS %s is required",
        field_name);
    return FALSE;
  }

  for (const char *cursor = value; *cursor != '\0'; cursor++) {
    if (g_ascii_iscntrl (*cursor)) {
      g_set_error (error,
          G_IO_ERROR,
          G_IO_ERROR_INVALID_ARGUMENT,
          "mailbox STATUS %s must not contain control characters", field_name);
      return FALSE;
    }
  }

  return TRUE;
}

void
wyrebox_daemon_mailbox_status_request_clear (WyreboxDaemonMailboxStatusRequest
    *request)
{
  if (request == NULL)
    return;

  g_clear_pointer (&request->account_identity, g_free);
  g_clear_pointer (&request->mailbox_id, g_free);
  g_clear_pointer (&request->mailbox_name, g_free);
}

gboolean
wyrebox_daemon_mailbox_status_request_init (WyreboxDaemonMailboxStatusRequest
    *request, const char *account_identity, const char *mailbox_id,
    const char *mailbox_name, GError **error)
{
  g_auto (WyreboxDaemonMailboxStatusRequest) next = { 0 };
  gboolean has_mailbox_id = text_has_value (mailbox_id);
  gboolean has_mailbox_name = text_has_value (mailbox_name);

  g_return_val_if_fail (request != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (!validate_text (account_identity, "account_identity", TRUE, error))
    return FALSE;

  if (has_mailbox_id == has_mailbox_name) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "mailbox STATUS requires exactly one of mailbox_id or mailbox_name");
    return FALSE;
  }

  if (!validate_text (mailbox_id, "mailbox_id", FALSE, error))
    return FALSE;

  if (!validate_text (mailbox_name, "mailbox_name", FALSE, error))
    return FALSE;

  next.account_identity = g_strdup (account_identity);
  if (has_mailbox_id)
    next.mailbox_id = g_strdup (mailbox_id);
  if (has_mailbox_name)
    next.mailbox_name = g_strdup (mailbox_name);

  wyrebox_daemon_mailbox_status_request_clear (request);
  *request = next;
  next.account_identity = NULL;
  next.mailbox_id = NULL;
  next.mailbox_name = NULL;

  return TRUE;
}
