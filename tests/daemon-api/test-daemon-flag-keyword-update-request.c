#include "wyrebox-daemon-flag-keyword-update-request.h"

#include <gio/gio.h>

static gboolean
init_request (WyreboxDaemonFlagKeywordUpdateRequest *request,
    WyreboxDaemonFlagKeywordUpdateMode mode,
    const char *account_identity,
    const char *mailbox_id,
    guint64 uid_validity,
    guint64 mailbox_uid,
    const char *const *system_flags,
    const char *const *user_keywords, GError **error)
{
  return wyrebox_daemon_flag_keyword_update_request_init (request,
      account_identity,
      mailbox_id,
      uid_validity, mailbox_uid, mode, system_flags, user_keywords, error);
}

static void
test_request_init_copies_fields (void)
{
  const char *system_flags[] = { "\\Seen", "\\Flagged", NULL };
  const char *user_keywords[] = { "project", "todo", NULL };
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonFlagKeywordUpdateRequest) request = { 0 };

  g_assert_true (init_request (&request,
          WYREBOX_DAEMON_FLAG_KEYWORD_UPDATE_MODE_SET,
          "account-1",
          "mailbox-inbox", 77, 42, system_flags, user_keywords, &error));
  g_assert_no_error (error);

  g_assert_cmpstr (request.account_identity, ==, "account-1");
  g_assert_cmpstr (request.mailbox_id, ==, "mailbox-inbox");
  g_assert_cmpuint (request.uid_validity, ==, 77);
  g_assert_cmpuint (request.mailbox_uid, ==, 42);
  g_assert_cmpint (request.mode, ==,
      WYREBOX_DAEMON_FLAG_KEYWORD_UPDATE_MODE_SET);
  g_assert_cmpstr (request.system_flags[0], ==, "\\Seen");
  g_assert_cmpstr (request.system_flags[1], ==, "\\Flagged");
  g_assert_null (request.system_flags[2]);
  g_assert_cmpstr (request.user_keywords[0], ==, "project");
  g_assert_cmpstr (request.user_keywords[1], ==, "todo");
  g_assert_null (request.user_keywords[2]);
}

static void
test_request_init_rejects_missing_account_identity (void)
{
  const char *system_flags[] = { "\\Seen", NULL };
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonFlagKeywordUpdateRequest) request = { 0 };

  g_assert_false (init_request (&request,
          WYREBOX_DAEMON_FLAG_KEYWORD_UPDATE_MODE_SET,
          NULL, "mailbox-inbox", 77, 42, system_flags, NULL, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_null (request.account_identity);
}

static void
test_request_init_rejects_missing_mailbox_id (void)
{
  const char *system_flags[] = { "\\Seen", NULL };
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonFlagKeywordUpdateRequest) request = { 0 };

  g_assert_false (init_request (&request,
          WYREBOX_DAEMON_FLAG_KEYWORD_UPDATE_MODE_SET,
          "account-1", NULL, 77, 42, system_flags, NULL, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_null (request.mailbox_id);
}

static void
test_request_init_rejects_control_character_text (void)
{
  const char *system_flags[] = { "\\Seen", NULL };
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonFlagKeywordUpdateRequest) request = { 0 };

  g_assert_false (init_request (&request,
          WYREBOX_DAEMON_FLAG_KEYWORD_UPDATE_MODE_SET,
          "account-1\nother",
          "mailbox-inbox", 77, 42, system_flags, NULL, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_null (request.account_identity);
}

static void
test_request_init_rejects_zero_uid_validity (void)
{
  const char *system_flags[] = { "\\Seen", NULL };
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonFlagKeywordUpdateRequest) request = { 0 };

  g_assert_false (init_request (&request,
          WYREBOX_DAEMON_FLAG_KEYWORD_UPDATE_MODE_SET,
          "account-1", "mailbox-inbox", 0, 42, system_flags, NULL, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_cmpuint (request.uid_validity, ==, 0);
}

static void
test_request_init_rejects_zero_mailbox_uid (void)
{
  const char *system_flags[] = { "\\Seen", NULL };
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonFlagKeywordUpdateRequest) request = { 0 };

  g_assert_false (init_request (&request,
          WYREBOX_DAEMON_FLAG_KEYWORD_UPDATE_MODE_SET,
          "account-1", "mailbox-inbox", 77, 0, system_flags, NULL, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_cmpuint (request.mailbox_uid, ==, 0);
}

static void
test_request_init_rejects_invalid_mode (void)
{
  const char *system_flags[] = { "\\Seen", NULL };
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonFlagKeywordUpdateRequest) request = { 0 };

  g_assert_false (init_request (&request,
          (WyreboxDaemonFlagKeywordUpdateMode) 99,
          "account-1", "mailbox-inbox", 77, 42, system_flags, NULL, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
}

static void
test_request_init_rejects_missing_payload (void)
{
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonFlagKeywordUpdateRequest) request = { 0 };

  g_assert_false (init_request (&request,
          WYREBOX_DAEMON_FLAG_KEYWORD_UPDATE_MODE_SET,
          "account-1", "mailbox-inbox", 77, 42, NULL, NULL, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_null (request.system_flags);
  g_assert_null (request.user_keywords);
}

static void
test_request_init_rejects_clear_missing_payload (void)
{
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonFlagKeywordUpdateRequest) request = { 0 };

  g_assert_false (init_request (&request,
          WYREBOX_DAEMON_FLAG_KEYWORD_UPDATE_MODE_CLEAR,
          "account-1", "mailbox-inbox", 77, 42, NULL, NULL, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_null (request.system_flags);
  g_assert_null (request.user_keywords);
}

static void
test_request_init_allows_replace_empty_payload (void)
{
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonFlagKeywordUpdateRequest) request = { 0 };

  g_assert_true (init_request (&request,
          WYREBOX_DAEMON_FLAG_KEYWORD_UPDATE_MODE_REPLACE,
          "account-1", "mailbox-inbox", 77, 42, NULL, NULL, &error));
  g_assert_no_error (error);
  g_assert_cmpint (request.mode, ==,
      WYREBOX_DAEMON_FLAG_KEYWORD_UPDATE_MODE_REPLACE);
  g_assert_null (request.system_flags);
  g_assert_null (request.user_keywords);
}

static void
test_request_init_rejects_empty_system_flag (void)
{
  const char *system_flags[] = { "", NULL };
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonFlagKeywordUpdateRequest) request = { 0 };

  g_assert_false (init_request (&request,
          WYREBOX_DAEMON_FLAG_KEYWORD_UPDATE_MODE_SET,
          "account-1", "mailbox-inbox", 77, 42, system_flags, NULL, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_null (request.system_flags);
}

static void
test_request_init_rejects_unknown_system_flag (void)
{
  const char *system_flags[] = { "not-a-system-flag", NULL };
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonFlagKeywordUpdateRequest) request = { 0 };

  g_assert_false (init_request (&request,
          WYREBOX_DAEMON_FLAG_KEYWORD_UPDATE_MODE_SET,
          "account-1", "mailbox-inbox", 77, 42, system_flags, NULL, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_null (request.system_flags);
}

static void
test_request_init_rejects_recent_system_flag (void)
{
  const char *system_flags[] = { "\\Recent", NULL };
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonFlagKeywordUpdateRequest) request = { 0 };

  g_assert_false (init_request (&request,
          WYREBOX_DAEMON_FLAG_KEYWORD_UPDATE_MODE_SET,
          "account-1", "mailbox-inbox", 77, 42, system_flags, NULL, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_null (request.system_flags);
}

static void
test_request_init_rejects_system_flag_user_keyword (void)
{
  const char *user_keywords[] = { "\\Seen", NULL };
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonFlagKeywordUpdateRequest) request = { 0 };

  g_assert_false (init_request (&request,
          WYREBOX_DAEMON_FLAG_KEYWORD_UPDATE_MODE_SET,
          "account-1", "mailbox-inbox", 77, 42, NULL, user_keywords, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_null (request.user_keywords);
}

static void
test_request_init_rejects_space_user_keyword (void)
{
  const char *user_keywords[] = { "needs review", NULL };
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonFlagKeywordUpdateRequest) request = { 0 };

  g_assert_false (init_request (&request,
          WYREBOX_DAEMON_FLAG_KEYWORD_UPDATE_MODE_SET,
          "account-1", "mailbox-inbox", 77, 42, NULL, user_keywords, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_null (request.user_keywords);
}

static void
test_request_init_rejects_slash_user_keyword (void)
{
  const char *user_keywords[] = { "needs/review", NULL };
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonFlagKeywordUpdateRequest) request = { 0 };

  g_assert_false (init_request (&request,
          WYREBOX_DAEMON_FLAG_KEYWORD_UPDATE_MODE_SET,
          "account-1", "mailbox-inbox", 77, 42, NULL, user_keywords, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_null (request.user_keywords);
}

static void
test_request_init_rejects_empty_user_keyword (void)
{
  const char *system_flags[] = { "\\Seen", NULL };
  const char *user_keywords[] = { "", NULL };
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonFlagKeywordUpdateRequest) request = { 0 };

  g_assert_false (init_request (&request,
          WYREBOX_DAEMON_FLAG_KEYWORD_UPDATE_MODE_SET,
          "account-1",
          "mailbox-inbox", 77, 42, system_flags, user_keywords, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_null (request.user_keywords);
}

static void
test_request_init_rejects_duplicate_system_flags (void)
{
  const char *system_flags[] = { "\\Seen", "\\Seen", NULL };
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonFlagKeywordUpdateRequest) request = { 0 };

  g_assert_false (init_request (&request,
          WYREBOX_DAEMON_FLAG_KEYWORD_UPDATE_MODE_SET,
          "account-1", "mailbox-inbox", 77, 42, system_flags, NULL, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_null (request.system_flags);
}

static void
test_request_init_rejects_duplicate_user_keywords (void)
{
  const char *system_flags[] = { "\\Seen", NULL };
  const char *user_keywords[] = { "project", "project", NULL };
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonFlagKeywordUpdateRequest) request = { 0 };

  g_assert_false (init_request (&request,
          WYREBOX_DAEMON_FLAG_KEYWORD_UPDATE_MODE_SET,
          "account-1",
          "mailbox-inbox", 77, 42, system_flags, user_keywords, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_null (request.user_keywords);
}

static void
test_request_init_rejects_control_in_system_flag (void)
{
  const char *system_flags[] = { "\\Seen\n", NULL };
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonFlagKeywordUpdateRequest) request = { 0 };

  g_assert_false (init_request (&request,
          WYREBOX_DAEMON_FLAG_KEYWORD_UPDATE_MODE_SET,
          "account-1", "mailbox-inbox", 77, 42, system_flags, NULL, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_null (request.system_flags);
}

static void
test_request_init_rejects_control_in_user_keyword (void)
{
  const char *system_flags[] = { "\\Seen", NULL };
  const char *user_keywords[] = { "pro\nject", NULL };
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonFlagKeywordUpdateRequest) request = { 0 };

  g_assert_false (init_request (&request,
          WYREBOX_DAEMON_FLAG_KEYWORD_UPDATE_MODE_SET,
          "account-1",
          "mailbox-inbox", 77, 42, system_flags, user_keywords, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_null (request.user_keywords);
}

static void
test_request_init_replaces_existing_value (void)
{
  const char *system_flags_first[] = { "\\Seen", NULL };
  const char *system_flags_second[] = { "\\Flagged", NULL };
  const char *user_keywords_first[] = { "project", NULL };
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonFlagKeywordUpdateRequest) request = { 0 };

  g_assert_true (init_request (&request,
          WYREBOX_DAEMON_FLAG_KEYWORD_UPDATE_MODE_CLEAR,
          "account-1",
          "mailbox-old",
          10, 11, system_flags_first, user_keywords_first, &error));
  g_assert_no_error (error);

  g_assert_true (init_request (&request,
          WYREBOX_DAEMON_FLAG_KEYWORD_UPDATE_MODE_REPLACE,
          "account-2",
          "mailbox-new", 20, 21, system_flags_second, NULL, &error));
  g_assert_no_error (error);

  g_assert_cmpint (request.mode, ==,
      WYREBOX_DAEMON_FLAG_KEYWORD_UPDATE_MODE_REPLACE);
  g_assert_cmpstr (request.account_identity, ==, "account-2");
  g_assert_cmpstr (request.mailbox_id, ==, "mailbox-new");
  g_assert_cmpuint (request.uid_validity, ==, 20);
  g_assert_cmpuint (request.mailbox_uid, ==, 21);
  g_assert_cmpstr (request.system_flags[0], ==, "\\Flagged");
  g_assert_null (request.user_keywords);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/daemon-api/flag-keyword-update-request/copies-fields",
      test_request_init_copies_fields);
  g_test_add_func ("/daemon-api/flag-keyword-update-request/"
      "rejects-missing-account-identity",
      test_request_init_rejects_missing_account_identity);
  g_test_add_func ("/daemon-api/flag-keyword-update-request/"
      "rejects-missing-mailbox-id",
      test_request_init_rejects_missing_mailbox_id);
  g_test_add_func ("/daemon-api/flag-keyword-update-request/"
      "rejects-control-character-text",
      test_request_init_rejects_control_character_text);
  g_test_add_func ("/daemon-api/flag-keyword-update-request/"
      "rejects-zero-uid-validity", test_request_init_rejects_zero_uid_validity);
  g_test_add_func ("/daemon-api/flag-keyword-update-request/"
      "rejects-zero-mailbox-uid", test_request_init_rejects_zero_mailbox_uid);
  g_test_add_func ("/daemon-api/flag-keyword-update-request/"
      "rejects-invalid-mode", test_request_init_rejects_invalid_mode);
  g_test_add_func ("/daemon-api/flag-keyword-update-request/"
      "rejects-missing-payload", test_request_init_rejects_missing_payload);
  g_test_add_func ("/daemon-api/flag-keyword-update-request/"
      "rejects-clear-missing-payload",
      test_request_init_rejects_clear_missing_payload);
  g_test_add_func ("/daemon-api/flag-keyword-update-request/"
      "allows-replace-empty-payload",
      test_request_init_allows_replace_empty_payload);
  g_test_add_func ("/daemon-api/flag-keyword-update-request/"
      "rejects-empty-system-flag", test_request_init_rejects_empty_system_flag);
  g_test_add_func ("/daemon-api/flag-keyword-update-request/"
      "rejects-unknown-system-flag",
      test_request_init_rejects_unknown_system_flag);
  g_test_add_func ("/daemon-api/flag-keyword-update-request/"
      "rejects-recent-system-flag",
      test_request_init_rejects_recent_system_flag);
  g_test_add_func ("/daemon-api/flag-keyword-update-request/"
      "rejects-system-flag-user-keyword",
      test_request_init_rejects_system_flag_user_keyword);
  g_test_add_func ("/daemon-api/flag-keyword-update-request/"
      "rejects-space-user-keyword",
      test_request_init_rejects_space_user_keyword);
  g_test_add_func ("/daemon-api/flag-keyword-update-request/"
      "rejects-slash-user-keyword",
      test_request_init_rejects_slash_user_keyword);
  g_test_add_func ("/daemon-api/flag-keyword-update-request/"
      "rejects-empty-user-keyword",
      test_request_init_rejects_empty_user_keyword);
  g_test_add_func ("/daemon-api/flag-keyword-update-request/"
      "rejects-duplicate-system-flags",
      test_request_init_rejects_duplicate_system_flags);
  g_test_add_func ("/daemon-api/flag-keyword-update-request/"
      "rejects-duplicate-user-keywords",
      test_request_init_rejects_duplicate_user_keywords);
  g_test_add_func ("/daemon-api/flag-keyword-update-request/"
      "rejects-control-in-system-flag",
      test_request_init_rejects_control_in_system_flag);
  g_test_add_func ("/daemon-api/flag-keyword-update-request/"
      "rejects-control-in-user-keyword",
      test_request_init_rejects_control_in_user_keyword);
  g_test_add_func ("/daemon-api/flag-keyword-update-request/"
      "replaces-existing-value", test_request_init_replaces_existing_value);

  return g_test_run ();
}
