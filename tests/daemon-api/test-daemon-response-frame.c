#include "wyrebox-daemon-response-frame.h"

#include <gio/gio.h>
#include <glib.h>

static WyreboxDaemonSuccessReceipt
make_success_receipt (void)
{
  WyreboxDaemonSuccessReceipt receipt = {
    .request_id = g_strdup ("request-success"),
    .durable_marker = g_strdup ("journal:0:1"),
    .journal_offset = 0,
    .journal_sequence = 1,
    .summary = g_strdup ("delivery_ingestion object_key=sha256:test "
        "size_bytes=42"),
  };

  return receipt;
}

static WyreboxDaemonMailboxListResult
make_mailbox_list_result (void)
{
  g_auto (WyreboxDaemonMailboxListResult) result = { 0 };
  g_autoptr (GError) error = NULL;

  wyrebox_daemon_mailbox_list_result_init_empty (&result);
  g_assert_true (wyrebox_daemon_mailbox_list_result_append_entry (&result,
          WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_ORDINARY,
          "mailbox-inbox", "INBOX", "/", "\\Inbox", TRUE,
          WYREBOX_DAEMON_MAILBOX_LIST_CHILD_STATE_HAS_NO_CHILDREN, &error));
  g_assert_no_error (error);
  g_assert_true (wyrebox_daemon_mailbox_list_result_append_entry (&result,
          WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_VIRTUAL,
          "view-project-a", "Projects/Project A", "/", NULL, TRUE,
          WYREBOX_DAEMON_MAILBOX_LIST_CHILD_STATE_UNKNOWN, &error));
  g_assert_no_error (error);

  return (WyreboxDaemonMailboxListResult) {
  .entries = g_steal_pointer (&result.entries),};
}

static void
assert_mailbox_list_entry (const WyreboxDaemonMailboxListResult *result,
    guint index,
    WyreboxDaemonMailboxListEntryKind kind,
    const char *mailbox_id,
    const char *mailbox_name,
    const char *hierarchy_delimiter,
    const char *special_use,
    gboolean is_selectable, WyreboxDaemonMailboxListChildState child_state)
{
  const WyreboxDaemonMailboxListEntry *entry =
      wyrebox_daemon_mailbox_list_result_get_entry (result, index);

  g_assert_nonnull (entry);
  g_assert_cmpint (entry->kind, ==, kind);
  g_assert_cmpstr (entry->mailbox_id, ==, mailbox_id);
  g_assert_cmpstr (entry->mailbox_name, ==, mailbox_name);
  g_assert_cmpstr (entry->hierarchy_delimiter, ==, hierarchy_delimiter);
  g_assert_cmpstr (entry->special_use, ==, special_use);
  g_assert_cmpint (entry->is_selectable, ==, is_selectable);
  g_assert_cmpint (entry->child_state, ==, child_state);
}

static void
test_response_frame_init_success_copies_payload (void)
{
  g_auto (WyreboxDaemonSuccessReceipt) receipt = make_success_receipt ();
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonResponseFrame) frame = { 0 };

  g_assert_true (wyrebox_daemon_response_frame_init_success (&frame,
          &receipt, "correlation-1", &error));
  g_assert_no_error (error);

  g_assert_cmpstr (frame.request_id, ==, "request-success");
  g_assert_cmpstr (frame.correlation_id, ==, "correlation-1");
  g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_SUCCESS);
  g_assert_cmpstr (frame.success.request_id, ==, "request-success");
  g_assert_cmpstr (frame.success.durable_marker, ==, "journal:0:1");
  g_assert_cmpuint (frame.success.journal_offset, ==, 0);
  g_assert_cmpuint (frame.success.journal_sequence, ==, 1);
  g_assert_cmpstr (frame.success.summary,
      ==, "delivery_ingestion object_key=sha256:test size_bytes=42");
  g_assert_null (frame.error.request_id);
}

static void
test_response_frame_init_error_copies_payload (void)
{
  g_auto (WyreboxDaemonErrorFrame) error_frame = { 0 };
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonResponseFrame) frame = { 0 };

  g_assert_true (wyrebox_daemon_error_frame_init (&error_frame,
          "request-error",
          WYREBOX_DAEMON_ERROR_TEMPORARY_FAILURE,
          "try later", "retry", &error));
  g_assert_no_error (error);

  g_assert_true (wyrebox_daemon_response_frame_init_error (&frame,
          &error_frame, "correlation-2", &error));
  g_assert_no_error (error);

  g_assert_cmpstr (frame.request_id, ==, "request-error");
  g_assert_cmpstr (frame.correlation_id, ==, "correlation-2");
  g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_ERROR);
  g_assert_cmpstr (frame.error.request_id, ==, "request-error");
  g_assert_cmpint (frame.error.error_class, ==,
      WYREBOX_DAEMON_ERROR_TEMPORARY_FAILURE);
  g_assert_cmpstr (frame.error.message, ==, "try later");
  g_assert_cmpstr (frame.error.retry_hint, ==, "retry");
  g_assert_null (frame.success.request_id);
}

static void
test_response_frame_init_mailbox_list_copies_payload (void)
{
  g_auto (WyreboxDaemonMailboxListResult) result = make_mailbox_list_result ();
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonResponseFrame) frame = { 0 };
  const WyreboxDaemonMailboxListEntry *entry = NULL;

  g_assert_true (wyrebox_daemon_response_frame_init_mailbox_list (&frame,
          "request-list", "correlation-list", &result, &error));
  g_assert_no_error (error);

  g_assert_cmpstr (frame.request_id, ==, "request-list");
  g_assert_cmpstr (frame.correlation_id, ==, "correlation-list");
  g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_MAILBOX_LIST);
  g_assert_null (frame.success.request_id);
  g_assert_null (frame.error.request_id);
  g_assert_cmpuint (wyrebox_daemon_mailbox_list_result_get_n_entries
      (&frame.mailbox_list), ==, 2);

  entry = wyrebox_daemon_mailbox_list_result_get_entry (&frame.mailbox_list, 0);
  g_assert_nonnull (entry);
  g_assert_cmpint (entry->kind, ==, WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_ORDINARY);
  g_assert_cmpstr (entry->mailbox_id, ==, "mailbox-inbox");
  g_assert_cmpstr (entry->mailbox_name, ==, "INBOX");
  g_assert_cmpstr (entry->hierarchy_delimiter, ==, "/");
  g_assert_cmpstr (entry->special_use, ==, "\\Inbox");
  g_assert_true (entry->is_selectable);
  g_assert_cmpint (entry->child_state, ==,
      WYREBOX_DAEMON_MAILBOX_LIST_CHILD_STATE_HAS_NO_CHILDREN);

  entry = wyrebox_daemon_mailbox_list_result_get_entry (&frame.mailbox_list, 1);
  g_assert_nonnull (entry);
  g_assert_cmpint (entry->kind, ==, WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_VIRTUAL);
  g_assert_cmpstr (entry->mailbox_id, ==, "view-project-a");
  g_assert_cmpstr (entry->mailbox_name, ==, "Projects/Project A");
  g_assert_cmpstr (entry->special_use, ==, NULL);
}

static void
test_response_frame_mailbox_list_deep_copies_payload (void)
{
  g_auto (WyreboxDaemonMailboxListResult) result = { 0 };
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonResponseFrame) frame = { 0 };
  const WyreboxDaemonMailboxListEntry *entry = NULL;

  wyrebox_daemon_mailbox_list_result_init_empty (&result);
  g_assert_true (wyrebox_daemon_mailbox_list_result_append_entry (&result,
          WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_ORDINARY,
          "mailbox-inbox", "INBOX", "/", "\\Inbox", TRUE,
          WYREBOX_DAEMON_MAILBOX_LIST_CHILD_STATE_HAS_NO_CHILDREN, &error));
  g_assert_no_error (error);

  g_assert_true (wyrebox_daemon_response_frame_init_mailbox_list (&frame,
          "request-list", NULL, &result, &error));
  g_assert_no_error (error);

  wyrebox_daemon_mailbox_list_result_clear (&result);
  wyrebox_daemon_mailbox_list_result_init_empty (&result);

  g_assert_cmpuint (wyrebox_daemon_mailbox_list_result_get_n_entries
      (&frame.mailbox_list), ==, 1);
  entry = wyrebox_daemon_mailbox_list_result_get_entry (&frame.mailbox_list, 0);
  g_assert_cmpstr (entry->mailbox_id, ==, "mailbox-inbox");
  g_assert_cmpstr (entry->mailbox_name, ==, "INBOX");
}

static void
test_response_frame_allows_empty_mailbox_list (void)
{
  g_auto (WyreboxDaemonMailboxListResult) result = { 0 };
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonResponseFrame) frame = { 0 };

  wyrebox_daemon_mailbox_list_result_init_empty (&result);

  g_assert_true (wyrebox_daemon_response_frame_init_mailbox_list (&frame,
          "request-list-empty", NULL, &result, &error));
  g_assert_no_error (error);

  g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_MAILBOX_LIST);
  g_assert_cmpuint (wyrebox_daemon_mailbox_list_result_get_n_entries
      (&frame.mailbox_list), ==, 0);
}

static void
test_response_frame_rejects_invalid_mailbox_list_payload (void)
{
  WyreboxDaemonMailboxListResult result = { 0 };
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonResponseFrame) frame = { 0 };

  g_assert_false (wyrebox_daemon_response_frame_init_mailbox_list (&frame,
          "request-list", NULL, &result, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_NONE);
  g_assert_null (frame.request_id);

  g_clear_error (&error);
  wyrebox_daemon_mailbox_list_result_init_empty (&result);
  g_assert_false (wyrebox_daemon_response_frame_init_mailbox_list (&frame,
          "", NULL, &result, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_NONE);
  wyrebox_daemon_mailbox_list_result_clear (&result);
}

static void
test_response_frame_rejects_malformed_mailbox_list_entry (void)
{
  g_auto (WyreboxDaemonMailboxListResult) result = make_mailbox_list_result ();
  g_auto (WyreboxDaemonMailboxListResult) malformed = { 0 };
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonResponseFrame) frame = { 0 };

  g_assert_true (wyrebox_daemon_response_frame_init_mailbox_list (&frame,
          "request-list", "stable-correlation", &result, &error));
  g_assert_no_error (error);

  malformed.entries = g_ptr_array_new ();
  g_ptr_array_add (malformed.entries, NULL);

  g_assert_false (wyrebox_daemon_response_frame_init_mailbox_list (&frame,
          "request-malformed", NULL, &malformed, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);

  g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_MAILBOX_LIST);
  g_assert_cmpstr (frame.request_id, ==, "request-list");
  g_assert_cmpstr (frame.correlation_id, ==, "stable-correlation");
  g_assert_cmpuint (wyrebox_daemon_mailbox_list_result_get_n_entries
      (&frame.mailbox_list), ==, 2);
  assert_mailbox_list_entry (&frame.mailbox_list, 0,
      WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_ORDINARY,
      "mailbox-inbox", "INBOX", "/", "\\Inbox", TRUE,
      WYREBOX_DAEMON_MAILBOX_LIST_CHILD_STATE_HAS_NO_CHILDREN);
  assert_mailbox_list_entry (&frame.mailbox_list, 1,
      WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_VIRTUAL,
      "view-project-a", "Projects/Project A", "/", NULL, TRUE,
      WYREBOX_DAEMON_MAILBOX_LIST_CHILD_STATE_UNKNOWN);
}

static void
test_response_frame_rejects_invalid_success_payload (void)
{
  WyreboxDaemonSuccessReceipt receipt = {
    .durable_marker = "journal:0:1",
    .journal_sequence = 1,
    .summary = "missing request id",
  };
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonResponseFrame) frame = { 0 };

  g_assert_false (wyrebox_daemon_response_frame_init_success (&frame,
          &receipt, NULL, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_NONE);
  g_assert_null (frame.request_id);
}

static void
test_response_frame_rejects_non_journaled_success_payload (void)
{
  WyreboxDaemonSuccessReceipt receipt = {
    .request_id = "request-success",
    .durable_marker = "journal:0:0",
    .journal_sequence = 0,
    .summary = "missing durable journal sequence",
  };
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonResponseFrame) frame = { 0 };

  g_assert_false (wyrebox_daemon_response_frame_init_success (&frame,
          &receipt, NULL, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_NONE);
  g_assert_null (frame.request_id);
}

static void
test_response_frame_rejects_invalid_error_payload (void)
{
  WyreboxDaemonErrorFrame error_frame = {
    .error_class = WYREBOX_DAEMON_ERROR_INTERNAL_ERROR,
    .message = "missing request id",
  };
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonResponseFrame) frame = { 0 };

  g_assert_false (wyrebox_daemon_response_frame_init_error (&frame,
          &error_frame, NULL, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_NONE);
  g_assert_null (frame.request_id);
}

static void
test_response_frame_success_then_error_is_mutually_exclusive (void)
{
  g_auto (WyreboxDaemonSuccessReceipt) receipt = make_success_receipt ();
  g_auto (WyreboxDaemonErrorFrame) error_frame = { 0 };
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonResponseFrame) frame = { 0 };

  g_assert_true (wyrebox_daemon_response_frame_init_success (&frame,
          &receipt, NULL, &error));
  g_assert_no_error (error);

  g_assert_true (wyrebox_daemon_error_frame_init (&error_frame,
          "request-error",
          WYREBOX_DAEMON_ERROR_NOT_FOUND, "not found", NULL, &error));
  g_assert_no_error (error);

  g_assert_true (wyrebox_daemon_response_frame_init_error (&frame,
          &error_frame, NULL, &error));
  g_assert_no_error (error);

  g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_ERROR);
  g_assert_cmpstr (frame.request_id, ==, "request-error");
  g_assert_null (frame.success.request_id);
  g_assert_cmpstr (frame.error.request_id, ==, "request-error");
}

static void
test_response_frame_mailbox_list_then_error_is_mutually_exclusive (void)
{
  g_auto (WyreboxDaemonMailboxListResult) result = make_mailbox_list_result ();
  g_auto (WyreboxDaemonErrorFrame) error_frame = { 0 };
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonResponseFrame) frame = { 0 };

  g_assert_true (wyrebox_daemon_response_frame_init_mailbox_list (&frame,
          "request-list", NULL, &result, &error));
  g_assert_no_error (error);

  g_assert_true (wyrebox_daemon_error_frame_init (&error_frame,
          "request-error",
          WYREBOX_DAEMON_ERROR_NOT_FOUND, "not found", NULL, &error));
  g_assert_no_error (error);

  g_assert_true (wyrebox_daemon_response_frame_init_error (&frame,
          &error_frame, NULL, &error));
  g_assert_no_error (error);

  g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_ERROR);
  g_assert_cmpstr (frame.request_id, ==, "request-error");
  g_assert_cmpuint (wyrebox_daemon_mailbox_list_result_get_n_entries
      (&frame.mailbox_list), ==, 0);
  g_assert_cmpstr (frame.error.request_id, ==, "request-error");
}

static void
test_response_frame_failure_leaves_existing_contents (void)
{
  g_auto (WyreboxDaemonSuccessReceipt) receipt = make_success_receipt ();
  WyreboxDaemonSuccessReceipt invalid = {
    .durable_marker = "journal:0:1",
    .journal_sequence = 1,
    .summary = "missing request id",
  };
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonResponseFrame) frame = { 0 };

  g_assert_true (wyrebox_daemon_response_frame_init_success (&frame,
          &receipt, "stable-correlation", &error));
  g_assert_no_error (error);

  g_assert_false (wyrebox_daemon_response_frame_init_success (&frame,
          &invalid, NULL, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);

  g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_SUCCESS);
  g_assert_cmpstr (frame.request_id, ==, "request-success");
  g_assert_cmpstr (frame.correlation_id, ==, "stable-correlation");
  g_assert_cmpstr (frame.success.request_id, ==, "request-success");
}

static void
test_response_frame_mailbox_list_failure_leaves_existing_contents (void)
{
  g_auto (WyreboxDaemonMailboxListResult) result = make_mailbox_list_result ();
  WyreboxDaemonMailboxListResult invalid = { 0 };
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonResponseFrame) frame = { 0 };

  g_assert_true (wyrebox_daemon_response_frame_init_mailbox_list (&frame,
          "request-list", "stable-correlation", &result, &error));
  g_assert_no_error (error);

  g_assert_false (wyrebox_daemon_response_frame_init_mailbox_list (&frame,
          "request-invalid", NULL, &invalid, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);

  g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_MAILBOX_LIST);
  g_assert_cmpstr (frame.request_id, ==, "request-list");
  g_assert_cmpstr (frame.correlation_id, ==, "stable-correlation");
  g_assert_cmpuint (wyrebox_daemon_mailbox_list_result_get_n_entries
      (&frame.mailbox_list), ==, 2);
  assert_mailbox_list_entry (&frame.mailbox_list, 0,
      WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_ORDINARY,
      "mailbox-inbox", "INBOX", "/", "\\Inbox", TRUE,
      WYREBOX_DAEMON_MAILBOX_LIST_CHILD_STATE_HAS_NO_CHILDREN);
  assert_mailbox_list_entry (&frame.mailbox_list, 1,
      WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_VIRTUAL,
      "view-project-a", "Projects/Project A", "/", NULL, TRUE,
      WYREBOX_DAEMON_MAILBOX_LIST_CHILD_STATE_UNKNOWN);
}

static void
test_response_frame_init_fact_mutation_success (void)
{
  const char *args[] = { "mail-1", NULL };
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonFactMutationRequest) request = { 0 };
  g_auto (WyreboxDaemonResponseFrame) frame = { 0 };

  g_assert_true (wyrebox_daemon_fact_mutation_request_init (&request,
          WYREBOX_DAEMON_FACT_MUTATION_INSERT,
          "project_mention", "account-1", args, &error));
  g_assert_no_error (error);

  g_assert_true (wyrebox_daemon_response_frame_init_fact_mutation_success
      (&frame, "request-1", "correlation-1", &request, 4096, 7, &error));
  g_assert_no_error (error);

  g_assert_cmpstr (frame.request_id, ==, "request-1");
  g_assert_cmpstr (frame.correlation_id, ==, "correlation-1");
  g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_SUCCESS);
  g_assert_cmpstr (frame.success.request_id, ==, "request-1");
  g_assert_cmpstr (frame.success.durable_marker, ==, "journal:4096:7");
  g_assert_cmpuint (frame.success.journal_offset, ==, 4096);
  g_assert_cmpuint (frame.success.journal_sequence, ==, 7);
  g_assert_cmpstr (frame.success.summary,
      ==,
      "fact_mutation mutation=insert predicate_id=project_mention "
      "scope_id=account-1 argument_count=1");
}

static void
test_response_frame_rejects_invalid_fact_mutation_success (void)
{
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonFactMutationRequest) request = { 0 };
  g_auto (WyreboxDaemonResponseFrame) frame = { 0 };

  g_assert_false (wyrebox_daemon_response_frame_init_fact_mutation_success
      (&frame, "request-1", NULL, &request, 4096, 7, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_NONE);
  g_assert_null (frame.request_id);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/daemon-api/response-frame/success-copies-payload",
      test_response_frame_init_success_copies_payload);
  g_test_add_func ("/daemon-api/response-frame/error-copies-payload",
      test_response_frame_init_error_copies_payload);
  g_test_add_func ("/daemon-api/response-frame/mailbox-list-copies-payload",
      test_response_frame_init_mailbox_list_copies_payload);
  g_test_add_func ("/daemon-api/response-frame/"
      "mailbox-list-deep-copies-payload",
      test_response_frame_mailbox_list_deep_copies_payload);
  g_test_add_func ("/daemon-api/response-frame/allows-empty-mailbox-list",
      test_response_frame_allows_empty_mailbox_list);
  g_test_add_func ("/daemon-api/response-frame/rejects-invalid-success",
      test_response_frame_rejects_invalid_success_payload);
  g_test_add_func ("/daemon-api/response-frame/rejects-non-journaled-success",
      test_response_frame_rejects_non_journaled_success_payload);
  g_test_add_func ("/daemon-api/response-frame/rejects-invalid-error",
      test_response_frame_rejects_invalid_error_payload);
  g_test_add_func ("/daemon-api/response-frame/"
      "rejects-invalid-mailbox-list",
      test_response_frame_rejects_invalid_mailbox_list_payload);
  g_test_add_func ("/daemon-api/response-frame/"
      "rejects-malformed-mailbox-list-entry",
      test_response_frame_rejects_malformed_mailbox_list_entry);
  g_test_add_func ("/daemon-api/response-frame/success-then-error-exclusive",
      test_response_frame_success_then_error_is_mutually_exclusive);
  g_test_add_func ("/daemon-api/response-frame/"
      "mailbox-list-then-error-exclusive",
      test_response_frame_mailbox_list_then_error_is_mutually_exclusive);
  g_test_add_func ("/daemon-api/response-frame/failure-leaves-existing",
      test_response_frame_failure_leaves_existing_contents);
  g_test_add_func ("/daemon-api/response-frame/"
      "mailbox-list-failure-leaves-existing",
      test_response_frame_mailbox_list_failure_leaves_existing_contents);
  g_test_add_func ("/daemon-api/response-frame/fact-mutation-success",
      test_response_frame_init_fact_mutation_success);
  g_test_add_func ("/daemon-api/response-frame/"
      "rejects-invalid-fact-mutation-success",
      test_response_frame_rejects_invalid_fact_mutation_success);

  return g_test_run ();
}
