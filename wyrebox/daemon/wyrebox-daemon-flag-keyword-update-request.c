#include "wyrebox-daemon-flag-keyword-update-request.h"

#include <gio/gio.h>

static gboolean
validate_non_empty_text (const char *value, const char *field_name,
    GError **error)
{
  if (value == NULL || *value == '\0') {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "flag keyword update %s is required", field_name);
    return FALSE;
  }

  for (const char *cursor = value; *cursor != '\0'; cursor++) {
    if (g_ascii_iscntrl (*cursor)) {
      g_set_error (error,
          G_IO_ERROR,
          G_IO_ERROR_INVALID_ARGUMENT,
          "flag keyword update %s must not contain control characters",
          field_name);
      return FALSE;
    }
  }

  return TRUE;
}

static gboolean
validate_system_flag (const char *value, GError **error)
{
  static const char *valid_system_flags[] = {
    "\\Seen",
    "\\Answered",
    "\\Flagged",
    "\\Deleted",
    "\\Draft",
    NULL
  };

  if (!validate_non_empty_text (value, "system_flags", error))
    return FALSE;

  for (gsize i = 0; valid_system_flags[i] != NULL; i++) {
    if (g_strcmp0 (value, valid_system_flags[i]) == 0)
      return TRUE;
  }

  g_set_error (error,
      G_IO_ERROR,
      G_IO_ERROR_INVALID_ARGUMENT,
      "flag keyword update system_flags contains unsupported system flag");
  return FALSE;
}

static gboolean
validate_user_keyword (const char *value, GError **error)
{
  if (!validate_non_empty_text (value, "user_keywords", error))
    return FALSE;

  if (value[0] == '\\') {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "flag keyword update user_keywords must not contain system flags");
    return FALSE;
  }

  for (const char *cursor = value; *cursor != '\0'; cursor++) {
    if (!(g_ascii_isalnum (*cursor) || *cursor == '_' || *cursor == '-'
            || *cursor == '.')) {
      g_set_error (error,
          G_IO_ERROR,
          G_IO_ERROR_INVALID_ARGUMENT,
          "flag keyword update user_keywords contains invalid characters");
      return FALSE;
    }
  }

  return TRUE;
}

static gboolean
validate_mode (WyreboxDaemonFlagKeywordUpdateMode mode, GError **error)
{
  switch (mode) {
    case WYREBOX_DAEMON_FLAG_KEYWORD_UPDATE_MODE_SET:
    case WYREBOX_DAEMON_FLAG_KEYWORD_UPDATE_MODE_CLEAR:
    case WYREBOX_DAEMON_FLAG_KEYWORD_UPDATE_MODE_REPLACE:
      return TRUE;
    default:
      g_set_error (error,
          G_IO_ERROR,
          G_IO_ERROR_INVALID_ARGUMENT, "flag keyword update mode is invalid");
      return FALSE;
  }
}

static gboolean
validate_payload (WyreboxDaemonFlagKeywordUpdateMode mode,
    const char *const *system_flags,
    const char *const *user_keywords, GError **error)
{
  if (mode == WYREBOX_DAEMON_FLAG_KEYWORD_UPDATE_MODE_REPLACE)
    return TRUE;

  if ((system_flags == NULL || *system_flags == NULL)
      && (user_keywords == NULL || *user_keywords == NULL)) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "flag keyword update requires at least one system flag or user keyword");
    return FALSE;
  }

  return TRUE;
}

static gboolean
copy_text_vector (const char *const *values,
    const char *field_name,
    gboolean (*validate_value) (const char *value, GError **error),
    gchar ***out_values, GError **error)
{
  g_auto (GStrv) copy = NULL;
  gsize count = 0;

  g_return_val_if_fail (out_values != NULL, FALSE);

  *out_values = NULL;

  if (values == NULL || *values == NULL)
    return TRUE;

  for (gsize i = 0; values[i] != NULL; i++) {
    const char *value = values[i];

    if (!validate_value (value, error))
      return FALSE;

    for (gsize j = 0; j < i; j++) {
      if (g_strcmp0 (values[j], value) == 0) {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_INVALID_ARGUMENT,
            "flag keyword update %s contains duplicate values", field_name);
        return FALSE;
      }
    }
  }

  for (gsize i = 0; values[i] != NULL; i++)
    count++;

  copy = g_new0 (gchar *, count + 1);
  for (gsize i = 0; values[i] != NULL; i++)
    copy[i] = g_strdup (values[i]);

  *out_values = g_steal_pointer (&copy);
  return TRUE;
}

void wyrebox_daemon_flag_keyword_update_request_clear
    (WyreboxDaemonFlagKeywordUpdateRequest * request)
{
  if (request == NULL)
    return;

  g_clear_pointer (&request->account_identity, g_free);
  g_clear_pointer (&request->mailbox_id, g_free);
  request->uid_validity = 0;
  request->mailbox_uid = 0;
  request->mode = WYREBOX_DAEMON_FLAG_KEYWORD_UPDATE_MODE_SET;
  g_clear_pointer (&request->system_flags, g_strfreev);
  g_clear_pointer (&request->user_keywords, g_strfreev);
}

gboolean
    wyrebox_daemon_flag_keyword_update_request_init
    (WyreboxDaemonFlagKeywordUpdateRequest * request,
    const char *account_identity, const char *mailbox_id, guint64 uid_validity,
    guint64 mailbox_uid, WyreboxDaemonFlagKeywordUpdateMode mode,
    const char *const *system_flags, const char *const *user_keywords,
    GError ** error)
{
  g_auto (WyreboxDaemonFlagKeywordUpdateRequest) next = { 0 };
  g_auto (GStrv) copied_system_flags = NULL;
  g_auto (GStrv) copied_user_keywords = NULL;

  g_return_val_if_fail (request != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (!validate_non_empty_text (account_identity, "account_identity", error))
    return FALSE;

  if (!validate_non_empty_text (mailbox_id, "mailbox_id", error))
    return FALSE;

  if (uid_validity == 0) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "flag keyword update uid_validity is required");
    return FALSE;
  }

  if (mailbox_uid == 0) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "flag keyword update mailbox_uid is required");
    return FALSE;
  }

  if (!validate_mode (mode, error))
    return FALSE;

  if (!copy_text_vector (system_flags,
          "system_flags", validate_system_flag, &copied_system_flags, error))
    return FALSE;

  if (!copy_text_vector (user_keywords,
          "user_keywords", validate_user_keyword, &copied_user_keywords, error))
    return FALSE;

  if (!validate_payload (mode,
          (const char *const *) copied_system_flags,
          (const char *const *) copied_user_keywords, error))
    return FALSE;

  next.account_identity = g_strdup (account_identity);
  next.mailbox_id = g_strdup (mailbox_id);
  next.uid_validity = uid_validity;
  next.mailbox_uid = mailbox_uid;
  next.mode = mode;
  next.system_flags = g_steal_pointer (&copied_system_flags);
  next.user_keywords = g_steal_pointer (&copied_user_keywords);

  wyrebox_daemon_flag_keyword_update_request_clear (request);
  *request = next;
  next.account_identity = NULL;
  next.mailbox_id = NULL;
  next.system_flags = NULL;
  next.user_keywords = NULL;

  return TRUE;
}
