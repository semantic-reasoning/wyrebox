#include "wyrebox-daemon-request-router.h"
#include "wyrebox-journal-reader.h"

#include <gio/gio.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>

static void
remove_tree (const char *path)
{
  g_autoptr (GDir) dir = NULL;
  const char *entry = NULL;

  dir = g_dir_open (path, 0, NULL);
  if (dir == NULL) {
    (void) g_remove (path);
    return;
  }

  while ((entry = g_dir_read_name (dir)) != NULL) {
    g_autofree char *child = g_build_filename (path, entry, NULL);

    remove_tree (child);
  }

  (void) g_rmdir (path);
}

static void
assert_journal_is_empty (const char *root)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxJournalReader) reader = NULL;
  g_auto (WyreboxJournalRecord) record = { 0 };
  gboolean eof = FALSE;

  reader = wyrebox_journal_reader_new (root, &error);
  g_assert_no_error (error);
  g_assert_false (wyrebox_journal_reader_read_next (reader,
          &record, &eof, &error));
  g_assert_no_error (error);
  g_assert_true (eof);
}

static gboolean
append_fixture_mailboxes (const WyreboxDaemonRequestIdentity *identity,
    const WyreboxDaemonMailboxListRequest *request,
    WyreboxDaemonMailboxListResult *out_result,
    gpointer user_data, GError **error)
{
  gboolean *was_called = user_data;

  g_assert_cmpstr (identity->request_id, ==, "request-list");
  g_assert_cmpstr (identity->account_identity, ==, "account-1");
  g_assert_cmpstr (request->account_identity, ==, "account-1");
  g_assert_cmpstr (request->namespace_prefix, ==, "");
  *was_called = TRUE;

  wyrebox_daemon_mailbox_list_result_init_empty (out_result);
  g_assert_true (wyrebox_daemon_mailbox_list_result_append_entry (out_result,
          WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_ORDINARY,
          "mailbox-inbox", "INBOX", "/", "\\Inbox", TRUE,
          WYREBOX_DAEMON_MAILBOX_LIST_CHILD_STATE_HAS_NO_CHILDREN, error));
  g_assert_true (wyrebox_daemon_mailbox_list_result_append_entry (out_result,
          WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_VIRTUAL,
          "view-project-a", "Projects/Project A", "/", NULL, TRUE,
          WYREBOX_DAEMON_MAILBOX_LIST_CHILD_STATE_UNKNOWN, error));

  return TRUE;
}

static gboolean
fail_mailbox_list_without_error (const WyreboxDaemonRequestIdentity *identity,
    const WyreboxDaemonMailboxListRequest *request,
    WyreboxDaemonMailboxListResult *out_result,
    gpointer user_data, GError **error)
{
  gboolean *was_called = user_data;

  *was_called = TRUE;
  return FALSE;
}

static gboolean
select_fixture_mailbox (const WyreboxDaemonRequestIdentity *identity,
    const WyreboxDaemonMailboxSelectRequest *request,
    WyreboxDaemonMailboxSelectResult *out_result,
    gpointer user_data, GError **error)
{
  gboolean *was_called = user_data;

  g_assert_cmpstr (identity->request_id, ==, "request-select");
  g_assert_cmpstr (identity->account_identity, ==, "account-1");
  g_assert_cmpstr (request->account_identity, ==, "account-1");
  g_assert_cmpstr (request->mailbox_id, ==, "mailbox-inbox");
  g_assert_null (request->mailbox_name);
  *was_called = TRUE;

  return wyrebox_daemon_mailbox_select_result_init (out_result,
      WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_ORDINARY,
      "mailbox-inbox", "INBOX", 77, 42, error);
}

static gboolean
fetch_message_fixture (const WyreboxDaemonRequestIdentity *identity,
    const WyreboxDaemonMessageFetchRequest *request,
    WyreboxDaemonStreamChunkFrame *out_chunk,
    gpointer user_data, GError **error)
{
  const char *payload = "message-bytes";
  g_autoptr (GBytes) bytes = NULL;
  gboolean *was_called = user_data;

  g_assert_nonnull (was_called);
  *was_called = TRUE;

  g_assert_cmpstr (identity->request_id, ==, "request-fetch");
  g_assert_cmpstr (identity->caller_identity, ==, "dovecot");
  g_assert_cmpstr (identity->account_identity, ==, "account-1");
  g_assert_cmpstr (request->account_identity, ==, "account-1");
  g_assert_cmpstr (request->mailbox_id, ==, "mailbox-inbox");
  g_assert_cmpuint (request->uid_validity, ==, 77);
  g_assert_cmpuint (request->mailbox_uid, ==, 42);

  bytes = g_bytes_new_static (payload, strlen (payload));

  return wyrebox_daemon_stream_chunk_frame_init (out_chunk,
      identity->request_id,
      "message-1", NULL, identity->correlation_id, 0, bytes, TRUE, error);
}

static void
test_request_router_routes_fact_mutation (void)
{
  const char *args[] = { "mail-1", NULL };
  g_autofree char *root =
      g_dir_make_tmp ("wyrebox-request-router-XXXXXX", NULL);
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxJournalWriter) writer = NULL;
  g_autoptr (WyreboxDaemonFactMutationService) service = NULL;
  g_auto (WyreboxDaemonFactMutationRequest) mutation = { 0 };
  g_auto (WyreboxDaemonResponseFrame) frame = { 0 };
  WyreboxDaemonDecodedRequestFrame request_frame = { 0 };

  g_assert_nonnull (root);
  g_assert_true (wyrebox_daemon_fact_mutation_request_init (&mutation,
          WYREBOX_DAEMON_FACT_MUTATION_INSERT,
          "project_mention", "account-1", args, &error));
  g_assert_no_error (error);

  request_frame.request_id = "request-1";
  request_frame.caller_identity = "skill";
  request_frame.account_identity = "account-1";
  request_frame.tool_identity = "fact-importer";
  request_frame.correlation_id = "correlation-1";
  request_frame.operation =
      WYREBOX_DAEMON_REQUEST_FRAME_OPERATION_FACT_MUTATION;
  request_frame.fact_mutation = &mutation;

  writer = wyrebox_journal_writer_new (root, &error);
  g_assert_no_error (error);
  service = wyrebox_daemon_fact_mutation_service_new (writer);
  g_assert_nonnull (service);

  g_assert_true (wyrebox_daemon_request_router_route (service, NULL, NULL, NULL,
          &request_frame, &frame, &error));
  g_assert_no_error (error);
  g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_SUCCESS);
  g_assert_cmpstr (frame.request_id, ==, "request-1");
  g_assert_cmpstr (frame.correlation_id, ==, "correlation-1");
  g_assert_cmpstr (frame.success.durable_marker, ==, "journal:0:1");

  remove_tree (root);
}

static void
test_request_router_rejects_unauthorized_fact_mutation_with_error_frame (void)
{
  const char *args[] = { "mail-1", NULL };
  g_autofree char *root =
      g_dir_make_tmp ("wyrebox-request-router-XXXXXX", NULL);
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxJournalWriter) writer = NULL;
  g_autoptr (WyreboxDaemonFactMutationService) service = NULL;
  g_auto (WyreboxDaemonFactMutationRequest) mutation = { 0 };
  g_auto (WyreboxDaemonResponseFrame) frame = { 0 };
  WyreboxDaemonDecodedRequestFrame request_frame = { 0 };

  g_assert_nonnull (root);
  g_assert_true (wyrebox_daemon_fact_mutation_request_init (&mutation,
          WYREBOX_DAEMON_FACT_MUTATION_INSERT,
          "project_mention", "account-1", args, &error));
  g_assert_no_error (error);

  request_frame.request_id = "request-1";
  request_frame.caller_identity = "dovecot";
  request_frame.account_identity = "account-1";
  request_frame.tool_identity = "fact-importer";
  request_frame.correlation_id = "correlation-1";
  request_frame.operation =
      WYREBOX_DAEMON_REQUEST_FRAME_OPERATION_FACT_MUTATION;
  request_frame.fact_mutation = &mutation;

  writer = wyrebox_journal_writer_new (root, &error);
  g_assert_no_error (error);
  service = wyrebox_daemon_fact_mutation_service_new (writer);
  g_assert_nonnull (service);

  g_assert_true (wyrebox_daemon_request_router_route (service, NULL, NULL, NULL,
          &request_frame, &frame, &error));
  g_assert_no_error (error);
  g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_ERROR);
  g_assert_cmpstr (frame.request_id, ==, "request-1");
  g_assert_cmpstr (frame.correlation_id, ==, "correlation-1");
  g_assert_cmpint (frame.error.error_class, ==,
      WYREBOX_DAEMON_ERROR_PERMISSION_DENIED);

  assert_journal_is_empty (root);
  remove_tree (root);
}

static void
test_request_router_rejects_missing_fact_mutation_payload (void)
{
  g_autofree char *root =
      g_dir_make_tmp ("wyrebox-request-router-XXXXXX", NULL);
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxJournalWriter) writer = NULL;
  g_autoptr (WyreboxDaemonFactMutationService) service = NULL;
  g_auto (WyreboxDaemonResponseFrame) frame = { 0 };
  WyreboxDaemonDecodedRequestFrame request_frame = { 0 };

  g_assert_nonnull (root);

  request_frame.request_id = "request-1";
  request_frame.caller_identity = "skill";
  request_frame.account_identity = "account-1";
  request_frame.tool_identity = "fact-importer";
  request_frame.correlation_id = "correlation-1";
  request_frame.operation =
      WYREBOX_DAEMON_REQUEST_FRAME_OPERATION_FACT_MUTATION;
  request_frame.fact_mutation = NULL;

  writer = wyrebox_journal_writer_new (root, &error);
  g_assert_no_error (error);
  service = wyrebox_daemon_fact_mutation_service_new (writer);
  g_assert_nonnull (service);

  g_assert_true (wyrebox_daemon_request_router_route (service, NULL, NULL, NULL,
          &request_frame, &frame, &error));
  g_assert_no_error (error);
  g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_ERROR);
  g_assert_cmpstr (frame.request_id, ==, "request-1");
  g_assert_cmpstr (frame.correlation_id, ==, "correlation-1");
  g_assert_cmpstr (frame.error.request_id, ==, "request-1");
  g_assert_cmpint (frame.error.error_class, ==,
      WYREBOX_DAEMON_ERROR_PERMANENT_FAILURE);

  assert_journal_is_empty (root);
  remove_tree (root);
}

static void
test_request_router_rejects_unsupported_operation_with_error_frame (void)
{
  g_autofree char *root =
      g_dir_make_tmp ("wyrebox-request-router-XXXXXX", NULL);
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxJournalWriter) writer = NULL;
  g_autoptr (WyreboxDaemonFactMutationService) service = NULL;
  g_auto (WyreboxDaemonResponseFrame) frame = { 0 };
  WyreboxDaemonDecodedRequestFrame request_frame = { 0 };

  g_assert_nonnull (root);

  request_frame.request_id = "request-1";
  request_frame.caller_identity = "skill";
  request_frame.account_identity = "account-1";
  request_frame.tool_identity = "fact-importer";
  request_frame.correlation_id = "correlation-1";
  request_frame.operation = WYREBOX_DAEMON_REQUEST_FRAME_OPERATION_NONE;

  writer = wyrebox_journal_writer_new (root, &error);
  g_assert_no_error (error);
  service = wyrebox_daemon_fact_mutation_service_new (writer);
  g_assert_nonnull (service);

  g_assert_true (wyrebox_daemon_request_router_route (service, NULL, NULL, NULL,
          &request_frame, &frame, &error));
  g_assert_no_error (error);
  g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_ERROR);
  g_assert_cmpstr (frame.request_id, ==, "request-1");
  g_assert_cmpstr (frame.correlation_id, ==, "correlation-1");
  g_assert_cmpstr (frame.error.request_id, ==, "request-1");
  g_assert_cmpint (frame.error.error_class, ==,
      WYREBOX_DAEMON_ERROR_PERMANENT_FAILURE);

  assert_journal_is_empty (root);
  remove_tree (root);
}

static void
test_request_router_rejects_missing_request_id_without_error_frame (void)
{
  const char *args[] = { "mail-1", NULL };
  g_autofree char *root =
      g_dir_make_tmp ("wyrebox-request-router-XXXXXX", NULL);
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxJournalWriter) writer = NULL;
  g_autoptr (WyreboxDaemonFactMutationService) service = NULL;
  g_auto (WyreboxDaemonFactMutationRequest) mutation = { 0 };
  g_auto (WyreboxDaemonResponseFrame) frame = { 0 };
  WyreboxDaemonDecodedRequestFrame request_frame = { 0 };

  g_assert_nonnull (root);
  g_assert_true (wyrebox_daemon_fact_mutation_request_init (&mutation,
          WYREBOX_DAEMON_FACT_MUTATION_INSERT,
          "project_mention", "account-1", args, &error));
  g_assert_no_error (error);

  request_frame.request_id = "";
  request_frame.caller_identity = "skill";
  request_frame.account_identity = "account-1";
  request_frame.tool_identity = "fact-importer";
  request_frame.correlation_id = "correlation-1";
  request_frame.operation =
      WYREBOX_DAEMON_REQUEST_FRAME_OPERATION_FACT_MUTATION;
  request_frame.fact_mutation = &mutation;

  writer = wyrebox_journal_writer_new (root, &error);
  g_assert_no_error (error);
  service = wyrebox_daemon_fact_mutation_service_new (writer);
  g_assert_nonnull (service);

  g_assert_false (wyrebox_daemon_request_router_route (service, NULL, NULL,
          NULL, &request_frame, &frame, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_NONE);

  assert_journal_is_empty (root);
  remove_tree (root);
}

static void
test_request_router_routes_mailbox_list (void)
{
  gboolean was_called = FALSE;
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxDaemonMailboxListService) service = NULL;
  g_auto (WyreboxDaemonMailboxListRequest) request = { 0 };
  g_auto (WyreboxDaemonResponseFrame) frame = { 0 };
  WyreboxDaemonDecodedRequestFrame request_frame = { 0 };
  const WyreboxDaemonMailboxListEntry *entry = NULL;

  g_assert_true (wyrebox_daemon_mailbox_list_request_init (&request,
          "account-1", NULL, &error));
  g_assert_no_error (error);

  request_frame.request_id = "request-list";
  request_frame.caller_identity = "dovecot";
  request_frame.account_identity = "account-1";
  request_frame.tool_identity = "dovecot-storage";
  request_frame.correlation_id = "imap-list-1";
  request_frame.operation = WYREBOX_DAEMON_REQUEST_FRAME_OPERATION_MAILBOX_LIST;
  request_frame.mailbox_list = &request;

  service = wyrebox_daemon_mailbox_list_service_new (append_fixture_mailboxes,
      &was_called, NULL);
  g_assert_nonnull (service);

  g_assert_true (wyrebox_daemon_request_router_route (NULL, service, NULL, NULL,
          &request_frame, &frame, &error));
  g_assert_no_error (error);
  g_assert_true (was_called);
  g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_MAILBOX_LIST);
  g_assert_cmpstr (frame.request_id, ==, "request-list");
  g_assert_cmpstr (frame.correlation_id, ==, "imap-list-1");
  g_assert_cmpuint (wyrebox_daemon_mailbox_list_result_get_n_entries
      (&frame.mailbox_list), ==, 2);

  entry = wyrebox_daemon_mailbox_list_result_get_entry (&frame.mailbox_list, 0);
  g_assert_cmpint (entry->kind, ==, WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_ORDINARY);
  g_assert_cmpstr (entry->mailbox_id, ==, "mailbox-inbox");
  entry = wyrebox_daemon_mailbox_list_result_get_entry (&frame.mailbox_list, 1);
  g_assert_cmpint (entry->kind, ==, WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_VIRTUAL);
  g_assert_cmpstr (entry->mailbox_id, ==, "view-project-a");
}

static void
test_request_router_rejects_missing_mailbox_list_payload (void)
{
  gboolean was_called = FALSE;
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxDaemonMailboxListService) service = NULL;
  g_auto (WyreboxDaemonResponseFrame) frame = { 0 };
  WyreboxDaemonDecodedRequestFrame request_frame = { 0 };

  request_frame.request_id = "request-list";
  request_frame.caller_identity = "dovecot";
  request_frame.account_identity = "account-1";
  request_frame.tool_identity = "dovecot-storage";
  request_frame.correlation_id = "imap-list-1";
  request_frame.operation = WYREBOX_DAEMON_REQUEST_FRAME_OPERATION_MAILBOX_LIST;
  request_frame.mailbox_list = NULL;

  service = wyrebox_daemon_mailbox_list_service_new (append_fixture_mailboxes,
      &was_called, NULL);
  g_assert_nonnull (service);

  g_assert_true (wyrebox_daemon_request_router_route (NULL, service, NULL, NULL,
          &request_frame, &frame, &error));
  g_assert_no_error (error);
  g_assert_false (was_called);
  g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_ERROR);
  g_assert_cmpstr (frame.request_id, ==, "request-list");
  g_assert_cmpstr (frame.correlation_id, ==, "imap-list-1");
  g_assert_cmpint (frame.error.error_class, ==,
      WYREBOX_DAEMON_ERROR_PERMANENT_FAILURE);
}

static void
test_request_router_rejects_unauthorized_mailbox_list_with_error_frame (void)
{
  gboolean was_called = FALSE;
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxDaemonMailboxListService) service = NULL;
  g_auto (WyreboxDaemonMailboxListRequest) request = { 0 };
  g_auto (WyreboxDaemonResponseFrame) frame = { 0 };
  WyreboxDaemonDecodedRequestFrame request_frame = { 0 };

  g_assert_true (wyrebox_daemon_mailbox_list_request_init (&request,
          "account-1", NULL, &error));
  g_assert_no_error (error);

  request_frame.request_id = "request-list";
  request_frame.caller_identity = "skill";
  request_frame.account_identity = "account-1";
  request_frame.tool_identity = "fact-importer";
  request_frame.correlation_id = "imap-list-1";
  request_frame.operation = WYREBOX_DAEMON_REQUEST_FRAME_OPERATION_MAILBOX_LIST;
  request_frame.mailbox_list = &request;

  service = wyrebox_daemon_mailbox_list_service_new (append_fixture_mailboxes,
      &was_called, NULL);
  g_assert_nonnull (service);

  g_assert_true (wyrebox_daemon_request_router_route (NULL, service, NULL, NULL,
          &request_frame, &frame, &error));
  g_assert_no_error (error);
  g_assert_false (was_called);
  g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_ERROR);
  g_assert_cmpint (frame.error.error_class, ==,
      WYREBOX_DAEMON_ERROR_PERMISSION_DENIED);
}

static void
test_request_router_rejects_missing_mailbox_list_service (void)
{
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonMailboxListRequest) request = { 0 };
  g_auto (WyreboxDaemonResponseFrame) frame = { 0 };
  WyreboxDaemonDecodedRequestFrame request_frame = { 0 };

  g_assert_true (wyrebox_daemon_mailbox_list_request_init (&request,
          "account-1", NULL, &error));
  g_assert_no_error (error);

  request_frame.request_id = "request-list";
  request_frame.caller_identity = "dovecot";
  request_frame.account_identity = "account-1";
  request_frame.tool_identity = "dovecot-storage";
  request_frame.correlation_id = "imap-list-1";
  request_frame.operation = WYREBOX_DAEMON_REQUEST_FRAME_OPERATION_MAILBOX_LIST;
  request_frame.mailbox_list = &request;

  g_assert_true (wyrebox_daemon_request_router_route (NULL, NULL, NULL, NULL,
          &request_frame, &frame, &error));
  g_assert_no_error (error);
  g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_ERROR);
  g_assert_cmpstr (frame.request_id, ==, "request-list");
  g_assert_cmpint (frame.error.error_class, ==,
      WYREBOX_DAEMON_ERROR_PERMANENT_FAILURE);
}

static void
test_request_router_rejects_mailbox_list_account_mismatch (void)
{
  gboolean was_called = FALSE;
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxDaemonMailboxListService) service = NULL;
  g_auto (WyreboxDaemonMailboxListRequest) request = { 0 };
  g_auto (WyreboxDaemonResponseFrame) frame = { 0 };
  WyreboxDaemonDecodedRequestFrame request_frame = { 0 };

  g_assert_true (wyrebox_daemon_mailbox_list_request_init (&request,
          "account-2", NULL, &error));
  g_assert_no_error (error);

  request_frame.request_id = "request-list";
  request_frame.caller_identity = "dovecot";
  request_frame.account_identity = "account-1";
  request_frame.tool_identity = "dovecot-storage";
  request_frame.correlation_id = "imap-list-1";
  request_frame.operation = WYREBOX_DAEMON_REQUEST_FRAME_OPERATION_MAILBOX_LIST;
  request_frame.mailbox_list = &request;

  service = wyrebox_daemon_mailbox_list_service_new (append_fixture_mailboxes,
      &was_called, NULL);
  g_assert_nonnull (service);

  g_assert_true (wyrebox_daemon_request_router_route (NULL, service, NULL, NULL,
          &request_frame, &frame, &error));
  g_assert_no_error (error);
  g_assert_false (was_called);
  g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_ERROR);
  g_assert_cmpint (frame.error.error_class, ==,
      WYREBOX_DAEMON_ERROR_PERMISSION_DENIED);
}

static void
test_request_router_rejects_mailbox_list_missing_request_id_without_frame (void)
{
  gboolean was_called = FALSE;
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxDaemonMailboxListService) service = NULL;
  g_auto (WyreboxDaemonMailboxListRequest) request = { 0 };
  g_auto (WyreboxDaemonResponseFrame) frame = { 0 };
  WyreboxDaemonDecodedRequestFrame request_frame = { 0 };

  g_assert_true (wyrebox_daemon_mailbox_list_request_init (&request,
          "account-1", NULL, &error));
  g_assert_no_error (error);

  request_frame.request_id = "";
  request_frame.caller_identity = "dovecot";
  request_frame.account_identity = "account-1";
  request_frame.tool_identity = "dovecot-storage";
  request_frame.correlation_id = "imap-list-1";
  request_frame.operation = WYREBOX_DAEMON_REQUEST_FRAME_OPERATION_MAILBOX_LIST;
  request_frame.mailbox_list = &request;

  service = wyrebox_daemon_mailbox_list_service_new (append_fixture_mailboxes,
      &was_called, NULL);
  g_assert_nonnull (service);

  g_assert_false (wyrebox_daemon_request_router_route (NULL, service, NULL,
          NULL, &request_frame, &frame, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_false (was_called);
  g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_NONE);
}

static void
test_request_router_converts_silent_mailbox_list_failure_to_error_frame (void)
{
  gboolean was_called = FALSE;
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxDaemonMailboxListService) service = NULL;
  g_auto (WyreboxDaemonMailboxListRequest) request = { 0 };
  g_auto (WyreboxDaemonResponseFrame) frame = { 0 };
  WyreboxDaemonDecodedRequestFrame request_frame = { 0 };

  g_assert_true (wyrebox_daemon_mailbox_list_request_init (&request,
          "account-1", NULL, &error));
  g_assert_no_error (error);

  request_frame.request_id = "request-list";
  request_frame.caller_identity = "dovecot";
  request_frame.account_identity = "account-1";
  request_frame.tool_identity = "dovecot-storage";
  request_frame.correlation_id = "imap-list-1";
  request_frame.operation = WYREBOX_DAEMON_REQUEST_FRAME_OPERATION_MAILBOX_LIST;
  request_frame.mailbox_list = &request;

  service =
      wyrebox_daemon_mailbox_list_service_new (fail_mailbox_list_without_error,
      &was_called, NULL);
  g_assert_nonnull (service);

  g_assert_true (wyrebox_daemon_request_router_route (NULL, service, NULL, NULL,
          &request_frame, &frame, &error));
  g_assert_no_error (error);
  g_assert_true (was_called);
  g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_ERROR);
  g_assert_cmpint (frame.error.error_class, ==,
      WYREBOX_DAEMON_ERROR_INTERNAL_ERROR);
}

static void
test_request_router_routes_mailbox_select (void)
{
  gboolean was_called = FALSE;
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxDaemonMailboxSelectService) service = NULL;
  g_auto (WyreboxDaemonMailboxSelectRequest) request = { 0 };
  g_auto (WyreboxDaemonResponseFrame) frame = { 0 };
  WyreboxDaemonDecodedRequestFrame request_frame = { 0 };

  g_assert_true (wyrebox_daemon_mailbox_select_request_init (&request,
          "account-1", "mailbox-inbox", NULL, &error));
  g_assert_no_error (error);

  request_frame.request_id = "request-select";
  request_frame.caller_identity = "dovecot";
  request_frame.account_identity = "account-1";
  request_frame.tool_identity = "dovecot-storage";
  request_frame.correlation_id = "imap-select-1";
  request_frame.operation =
      WYREBOX_DAEMON_REQUEST_FRAME_OPERATION_MAILBOX_SELECT;
  request_frame.mailbox_select = &request;

  service = wyrebox_daemon_mailbox_select_service_new (select_fixture_mailbox,
      &was_called, NULL);
  g_assert_nonnull (service);

  g_assert_true (wyrebox_daemon_request_router_route (NULL, NULL, service, NULL,
          &request_frame, &frame, &error));
  g_assert_no_error (error);
  g_assert_true (was_called);
  g_assert_cmpint (frame.kind, ==,
      WYREBOX_DAEMON_RESPONSE_FRAME_MAILBOX_SELECT);
  g_assert_cmpstr (frame.request_id, ==, "request-select");
  g_assert_cmpstr (frame.correlation_id, ==, "imap-select-1");
  g_assert_cmpstr (frame.mailbox_select.mailbox_id, ==, "mailbox-inbox");
  g_assert_cmpstr (frame.mailbox_select.mailbox_name, ==, "INBOX");
  g_assert_cmpuint (frame.mailbox_select.uid_validity, ==, 77);
  g_assert_cmpuint (frame.mailbox_select.uid_next, ==, 42);
}

static void
test_request_router_rejects_missing_mailbox_select_payload (void)
{
  gboolean was_called = FALSE;
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxDaemonMailboxSelectService) service = NULL;
  g_auto (WyreboxDaemonResponseFrame) frame = { 0 };
  WyreboxDaemonDecodedRequestFrame request_frame = { 0 };

  request_frame.request_id = "request-select";
  request_frame.caller_identity = "dovecot";
  request_frame.account_identity = "account-1";
  request_frame.tool_identity = "dovecot-storage";
  request_frame.correlation_id = "imap-select-1";
  request_frame.operation =
      WYREBOX_DAEMON_REQUEST_FRAME_OPERATION_MAILBOX_SELECT;
  request_frame.mailbox_select = NULL;

  service = wyrebox_daemon_mailbox_select_service_new (select_fixture_mailbox,
      &was_called, NULL);
  g_assert_nonnull (service);

  g_assert_true (wyrebox_daemon_request_router_route (NULL, NULL, service, NULL,
          &request_frame, &frame, &error));
  g_assert_no_error (error);
  g_assert_false (was_called);
  g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_ERROR);
  g_assert_cmpstr (frame.request_id, ==, "request-select");
  g_assert_cmpstr (frame.correlation_id, ==, "imap-select-1");
  g_assert_cmpint (frame.error.error_class, ==,
      WYREBOX_DAEMON_ERROR_PERMANENT_FAILURE);
}

static void
test_request_router_rejects_missing_mailbox_select_service (void)
{
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonMailboxSelectRequest) request = { 0 };
  g_auto (WyreboxDaemonResponseFrame) frame = { 0 };
  WyreboxDaemonDecodedRequestFrame request_frame = { 0 };

  g_assert_true (wyrebox_daemon_mailbox_select_request_init (&request,
          "account-1", "mailbox-inbox", NULL, &error));
  g_assert_no_error (error);

  request_frame.request_id = "request-select";
  request_frame.caller_identity = "dovecot";
  request_frame.account_identity = "account-1";
  request_frame.tool_identity = "dovecot-storage";
  request_frame.correlation_id = "imap-select-1";
  request_frame.operation =
      WYREBOX_DAEMON_REQUEST_FRAME_OPERATION_MAILBOX_SELECT;
  request_frame.mailbox_select = &request;

  g_assert_true (wyrebox_daemon_request_router_route (NULL, NULL, NULL, NULL,
          &request_frame, &frame, &error));
  g_assert_no_error (error);
  g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_ERROR);
  g_assert_cmpstr (frame.request_id, ==, "request-select");
  g_assert_cmpint (frame.error.error_class, ==,
      WYREBOX_DAEMON_ERROR_PERMANENT_FAILURE);
}

static void
test_request_router_routes_message_fetch (void)
{
  gboolean was_called = FALSE;
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxDaemonMessageFetchService) service = NULL;
  g_auto (WyreboxDaemonMessageFetchRequest) request = { 0 };
  g_auto (WyreboxDaemonResponseFrame) frame = { 0 };
  WyreboxDaemonDecodedRequestFrame request_frame = { 0 };

  g_assert_true (wyrebox_daemon_message_fetch_request_init (&request,
          "account-1", "mailbox-inbox", 77, 42, &error));
  g_assert_no_error (error);

  request_frame.request_id = "request-fetch";
  request_frame.caller_identity = "dovecot";
  request_frame.account_identity = "account-1";
  request_frame.tool_identity = "dovecot-storage";
  request_frame.correlation_id = "imap-fetch-1";
  request_frame.operation =
      WYREBOX_DAEMON_REQUEST_FRAME_OPERATION_MESSAGE_FETCH;
  request_frame.message_fetch = &request;

  service = wyrebox_daemon_message_fetch_service_new (fetch_message_fixture,
      &was_called, NULL);
  g_assert_nonnull (service);

  g_assert_true (wyrebox_daemon_request_router_route (NULL, NULL, NULL, service,
          &request_frame, &frame, &error));
  g_assert_no_error (error);
  g_assert_true (was_called);
  g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_STREAM_CHUNK);
  g_assert_cmpstr (frame.request_id, ==, "request-fetch");
  g_assert_cmpstr (frame.correlation_id, ==, "imap-fetch-1");
  g_assert_cmpstr (frame.stream_chunk.message_id, ==, "message-1");
  g_assert_cmpuint (frame.stream_chunk.chunk_index, ==, 0);
  g_assert_true (frame.stream_chunk.end_of_stream);
}

static void
test_request_router_rejects_missing_message_fetch_payload (void)
{
  gboolean was_called = FALSE;
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxDaemonMessageFetchService) service = NULL;
  g_auto (WyreboxDaemonResponseFrame) frame = { 0 };
  WyreboxDaemonDecodedRequestFrame request_frame = { 0 };

  request_frame.request_id = "request-fetch";
  request_frame.caller_identity = "dovecot";
  request_frame.account_identity = "account-1";
  request_frame.tool_identity = "dovecot-storage";
  request_frame.correlation_id = "imap-fetch-1";
  request_frame.operation =
      WYREBOX_DAEMON_REQUEST_FRAME_OPERATION_MESSAGE_FETCH;
  request_frame.message_fetch = NULL;

  service = wyrebox_daemon_message_fetch_service_new (fetch_message_fixture,
      &was_called, NULL);
  g_assert_nonnull (service);

  g_assert_true (wyrebox_daemon_request_router_route (NULL, NULL, NULL, service,
          &request_frame, &frame, &error));
  g_assert_no_error (error);
  g_assert_false (was_called);
  g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_ERROR);
  g_assert_cmpstr (frame.request_id, ==, "request-fetch");
  g_assert_cmpstr (frame.correlation_id, ==, "imap-fetch-1");
  g_assert_cmpint (frame.error.error_class, ==,
      WYREBOX_DAEMON_ERROR_PERMANENT_FAILURE);
}

static void
test_request_router_rejects_missing_message_fetch_service (void)
{
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonMessageFetchRequest) request = { 0 };
  g_auto (WyreboxDaemonResponseFrame) frame = { 0 };
  WyreboxDaemonDecodedRequestFrame request_frame = { 0 };

  g_assert_true (wyrebox_daemon_message_fetch_request_init (&request,
          "account-1", "mailbox-inbox", 77, 42, &error));
  g_assert_no_error (error);

  request_frame.request_id = "request-fetch";
  request_frame.caller_identity = "dovecot";
  request_frame.account_identity = "account-1";
  request_frame.tool_identity = "dovecot-storage";
  request_frame.correlation_id = "imap-fetch-1";
  request_frame.operation =
      WYREBOX_DAEMON_REQUEST_FRAME_OPERATION_MESSAGE_FETCH;
  request_frame.message_fetch = &request;

  g_assert_true (wyrebox_daemon_request_router_route (NULL, NULL, NULL, NULL,
          &request_frame, &frame, &error));
  g_assert_no_error (error);
  g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_ERROR);
  g_assert_cmpstr (frame.request_id, ==, "request-fetch");
  g_assert_cmpint (frame.error.error_class, ==,
      WYREBOX_DAEMON_ERROR_PERMANENT_FAILURE);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/daemon-api/request-router/routes-fact-mutation",
      test_request_router_routes_fact_mutation);
  g_test_add_func ("/daemon-api/request-router/"
      "rejects-unauthorized-fact-mutation-with-error-frame",
      test_request_router_rejects_unauthorized_fact_mutation_with_error_frame);
  g_test_add_func ("/daemon-api/request-router/"
      "rejects-missing-fact-mutation-payload",
      test_request_router_rejects_missing_fact_mutation_payload);
  g_test_add_func ("/daemon-api/request-router/"
      "rejects-unsupported-operation-with-error-frame",
      test_request_router_rejects_unsupported_operation_with_error_frame);
  g_test_add_func ("/daemon-api/request-router/"
      "rejects-missing-request-id-without-error-frame",
      test_request_router_rejects_missing_request_id_without_error_frame);
  g_test_add_func ("/daemon-api/request-router/routes-mailbox-list",
      test_request_router_routes_mailbox_list);
  g_test_add_func ("/daemon-api/request-router/"
      "rejects-missing-mailbox-list-payload",
      test_request_router_rejects_missing_mailbox_list_payload);
  g_test_add_func ("/daemon-api/request-router/"
      "rejects-unauthorized-mailbox-list-with-error-frame",
      test_request_router_rejects_unauthorized_mailbox_list_with_error_frame);
  g_test_add_func ("/daemon-api/request-router/"
      "rejects-missing-mailbox-list-service",
      test_request_router_rejects_missing_mailbox_list_service);
  g_test_add_func ("/daemon-api/request-router/"
      "rejects-mailbox-list-account-mismatch",
      test_request_router_rejects_mailbox_list_account_mismatch);
  g_test_add_func ("/daemon-api/request-router/"
      "rejects-mailbox-list-missing-request-id-without-frame",
      test_request_router_rejects_mailbox_list_missing_request_id_without_frame);
  g_test_add_func ("/daemon-api/request-router/"
      "converts-silent-mailbox-list-failure-to-error-frame",
      test_request_router_converts_silent_mailbox_list_failure_to_error_frame);
  g_test_add_func ("/daemon-api/request-router/routes-mailbox-select",
      test_request_router_routes_mailbox_select);
  g_test_add_func ("/daemon-api/request-router/"
      "rejects-missing-mailbox-select-payload",
      test_request_router_rejects_missing_mailbox_select_payload);
  g_test_add_func ("/daemon-api/request-router/"
      "rejects-missing-mailbox-select-service",
      test_request_router_rejects_missing_mailbox_select_service);
  g_test_add_func ("/daemon-api/request-router/routes-message-fetch",
      test_request_router_routes_message_fetch);
  g_test_add_func ("/daemon-api/request-router/"
      "rejects-missing-message-fetch-payload",
      test_request_router_rejects_missing_message_fetch_payload);
  g_test_add_func ("/daemon-api/request-router/"
      "rejects-missing-message-fetch-service",
      test_request_router_rejects_missing_message_fetch_service);

  return g_test_run ();
}
