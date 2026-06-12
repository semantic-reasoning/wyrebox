#include "wyrebox-daemon-message-search-request.h"

#include <gio/gio.h>

static gboolean
validate_non_empty_text (const char *value, const char *field_name,
    GError **error)
{
  if (value == NULL || *value == '\0') {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "message SEARCH %s is required", field_name);
    return FALSE;
  }

  for (const char *cursor = value; *cursor != '\0'; cursor++) {
    if (g_ascii_iscntrl (*cursor)) {
      g_set_error (error,
          G_IO_ERROR,
          G_IO_ERROR_INVALID_ARGUMENT,
          "message SEARCH %s must not contain control characters", field_name);
      return FALSE;
    }
  }

  return TRUE;
}

static gboolean
validate_criteria_token (const char *value, GError **error)
{
  if (!validate_non_empty_text (value, "criteria_token", error))
    return FALSE;

  for (const char *cursor = value; *cursor != '\0'; cursor++) {
    if (!(g_ascii_isalnum (*cursor) || *cursor == '_' || *cursor == '-'
            || *cursor == '.' || *cursor == ':')) {
      g_set_error (error,
          G_IO_ERROR,
          G_IO_ERROR_INVALID_ARGUMENT,
          "message SEARCH criteria_token contains invalid characters");
      return FALSE;
    }
  }

  return TRUE;
}

void
wyrebox_daemon_message_search_request_clear (WyreboxDaemonMessageSearchRequest
    *request)
{
  if (request == NULL)
    return;

  g_clear_pointer (&request->account_identity, g_free);
  g_clear_pointer (&request->mailbox_id, g_free);
  g_clear_pointer (&request->criteria_token, g_free);
  request->uid_validity = 0;
}

gboolean
wyrebox_daemon_message_search_request_init (WyreboxDaemonMessageSearchRequest
    *request, const char *account_identity, const char *mailbox_id,
    guint64 uid_validity, const char *criteria_token, GError **error)
{
  g_auto (WyreboxDaemonMessageSearchRequest) next = { 0 };

  g_return_val_if_fail (request != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (!validate_non_empty_text (account_identity, "account_identity", error))
    return FALSE;

  if (!validate_non_empty_text (mailbox_id, "mailbox_id", error))
    return FALSE;

  if (uid_validity == 0) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT, "message SEARCH uid_validity is required");
    return FALSE;
  }

  if (!validate_criteria_token (criteria_token, error))
    return FALSE;

  next.account_identity = g_strdup (account_identity);
  next.mailbox_id = g_strdup (mailbox_id);
  next.uid_validity = uid_validity;
  next.criteria_token = g_strdup (criteria_token);

  wyrebox_daemon_message_search_request_clear (request);
  *request = next;
  next.account_identity = NULL;
  next.mailbox_id = NULL;
  next.criteria_token = NULL;

  return TRUE;
}
