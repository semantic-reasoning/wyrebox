#include "wyrebox-daemon-fact-mutation-request.h"
#include "wyrebox-daemon-wirelog-predicate-query-dispatcher.h"
#include "wyrebox-journal-writer.h"

#include <gio/gio.h>
#include <glib/gstdio.h>
#include <string.h>

static const char *membership_rules =
    ".decl project_keyword(message_id: symbol, view_id: symbol)\n"
    ".decl show_in_virtual_folder(view_id: symbol, message_id: symbol)\n"
    "show_in_virtual_folder(view_id, message_id) :- "
    "project_keyword(message_id, view_id).\n";

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

static char *
make_journal_root (void)
{
  g_autoptr (GError) error = NULL;
  char *root = NULL;

  root = g_dir_make_tmp ("wyrebox-wirelog-predicate-service-XXXXXX", &error);
  g_assert_no_error (error);
  g_assert_nonnull (root);
  return root;
}

static void
append_fact_mutation (WyreboxJournalWriter *writer,
    WyreboxDaemonFactMutationKind mutation,
    const char *scope_id, const char *message_id, const char *view_id)
{
  const char *args[] = {
    message_id,
    view_id,
    NULL,
  };
  g_auto (WyreboxDaemonFactMutationRequest) request = { 0 };
  g_autoptr (GError) error = NULL;
  guint64 offset = 0;
  guint64 sequence = 0;

  g_assert_true (wyrebox_daemon_fact_mutation_request_init (&request,
          mutation, "project_keyword", scope_id, args, &error));
  g_assert_no_error (error);
  g_assert_true (wyrebox_daemon_fact_mutation_request_append_journal (&request,
          writer, &offset, &sequence, &error));
  g_assert_no_error (error);
}

static void
append_malformed_fact_mutation (WyreboxJournalWriter *writer)
{
  const guint8 payload[] = { 0xff };
  g_autoptr (GBytes) bytes = g_bytes_new_static (payload, sizeof (payload));
  g_autoptr (GError) error = NULL;
  guint64 offset = 0;
  guint64 sequence = 0;

  g_assert_true (wyrebox_journal_writer_append (writer,
          WYREBOX_JOURNAL_EVENT_FACT_INSERTED, bytes, &offset, &sequence,
          &error));
  g_assert_no_error (error);
}

static WyreboxJournalWriter *
new_writer (const char *root)
{
  g_autoptr (GError) error = NULL;
  WyreboxJournalWriter *writer = NULL;

  writer = wyrebox_journal_writer_new (root, &error);
  g_assert_no_error (error);
  g_assert_nonnull (writer);
  return writer;
}

static void
init_query_request (WyreboxDaemonWirelogPredicateQueryRequest *request)
{
  const char *bindings[] = { NULL };
  g_autoptr (GError) error = NULL;

  g_assert_true (wyrebox_daemon_wirelog_predicate_query_request_init (request,
          "query-1", "show_in_virtual_folder.v1", "account-1", bindings,
          &error));
  g_assert_no_error (error);
}

static char *
dispatch_csv (const char *root,
    const char *caller_identity,
    const char *account_identity, WyreboxDaemonResponseFrame *out_frame)
{
  g_auto (WyreboxDaemonWirelogPredicateQueryRequest) request = { 0 };
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxDaemonWirelogPredicateQueryService) service = NULL;
  gconstpointer data = NULL;
  gsize size = 0;

  service = wyrebox_daemon_wirelog_predicate_query_service_new_wirelog
      (membership_rules, root, &error);
  g_assert_no_error (error);
  g_assert_nonnull (service);

  init_query_request (&request);
  g_assert_true (wyrebox_daemon_wirelog_predicate_query_dispatch (service,
          "request-1", caller_identity, account_identity, "wirelog-tool",
          "correlation-1", &request, out_frame, &error));
  g_assert_no_error (error);

  if (out_frame->kind != WYREBOX_DAEMON_RESPONSE_FRAME_STREAM_CHUNK)
    return NULL;

  g_assert_cmpuint (out_frame->stream_chunk.chunk_index, ==, 0);
  g_assert_true (out_frame->stream_chunk.end_of_stream);

  data = g_bytes_get_data (out_frame->stream_chunk.bytes, &size);
  return g_strndup (data, size);
}

static void
test_wirelog_service_returns_membership_csv (void)
{
  g_autofree char *root = make_journal_root ();
  g_autoptr (WyreboxJournalWriter) writer = NULL;
  g_auto (WyreboxDaemonResponseFrame) frame = { 0 };
  g_autofree char *csv = NULL;

  writer = new_writer (root);
  append_fact_mutation (writer, WYREBOX_DAEMON_FACT_MUTATION_INSERT,
      "account-1", "message-2", "view-beta");
  append_fact_mutation (writer, WYREBOX_DAEMON_FACT_MUTATION_INSERT,
      "account-1", "message-1", "view-alpha");
  g_clear_object (&writer);

  csv = dispatch_csv (root, "trusted-tool", "account-1", &frame);
  g_assert_cmpstr (csv, ==,
      "account_id,view_id,message_id\n"
      "account-1,view-alpha,message-1\n" "account-1,view-beta,message-2\n");
  remove_tree (root);
}

static void
test_wirelog_service_empty_result_is_header_only (void)
{
  g_autofree char *root = make_journal_root ();
  g_auto (WyreboxDaemonResponseFrame) frame = { 0 };
  g_autofree char *csv = NULL;

  csv = dispatch_csv (root, "admin-cli", "account-1", &frame);
  g_assert_cmpstr (csv, ==, "account_id,view_id,message_id\n");
  remove_tree (root);
}

static void
test_wirelog_service_excludes_retracted_facts (void)
{
  g_autofree char *root = make_journal_root ();
  g_autoptr (WyreboxJournalWriter) writer = NULL;
  g_auto (WyreboxDaemonResponseFrame) frame = { 0 };
  g_autofree char *csv = NULL;

  writer = new_writer (root);
  append_fact_mutation (writer, WYREBOX_DAEMON_FACT_MUTATION_INSERT,
      "account-1", "message-1", "view-alpha");
  append_fact_mutation (writer, WYREBOX_DAEMON_FACT_MUTATION_RETRACT,
      "account-1", "message-1", "view-alpha");
  append_fact_mutation (writer, WYREBOX_DAEMON_FACT_MUTATION_INSERT,
      "account-1", "message-2", "view-beta");
  g_clear_object (&writer);

  csv = dispatch_csv (root, "trusted-tool", "account-1", &frame);
  g_assert_cmpstr (csv, ==,
      "account_id,view_id,message_id\n" "account-1,view-beta,message-2\n");
  remove_tree (root);
}

static void
test_wirelog_service_isolates_account_scope (void)
{
  g_autofree char *root = make_journal_root ();
  g_autoptr (WyreboxJournalWriter) writer = NULL;
  g_auto (WyreboxDaemonResponseFrame) frame = { 0 };
  g_autofree char *csv = NULL;

  writer = new_writer (root);
  append_fact_mutation (writer, WYREBOX_DAEMON_FACT_MUTATION_INSERT,
      "account-2", "message-other-account", "view-alpha");
  append_fact_mutation (writer, WYREBOX_DAEMON_FACT_MUTATION_INSERT,
      "account-1", "message-1", "view-alpha");
  g_clear_object (&writer);

  csv = dispatch_csv (root, "trusted-tool", "account-1", &frame);
  g_assert_cmpstr (csv, ==,
      "account_id,view_id,message_id\n" "account-1,view-alpha,message-1\n");
  remove_tree (root);
}

static void
test_wirelog_service_rejects_invalid_rules (void)
{
  g_autofree char *root = make_journal_root ();
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxDaemonWirelogPredicateQueryService) service = NULL;

  service = wyrebox_daemon_wirelog_predicate_query_service_new_wirelog
      ("not valid wirelog", root, &error);
  g_assert_null (service);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
  remove_tree (root);
}

static void
test_wirelog_service_reports_malformed_journal (void)
{
  g_autofree char *root = make_journal_root ();
  g_autoptr (WyreboxJournalWriter) writer = NULL;
  g_auto (WyreboxDaemonResponseFrame) frame = { 0 };
  g_autofree char *csv = NULL;

  writer = new_writer (root);
  append_malformed_fact_mutation (writer);
  g_clear_object (&writer);

  csv = dispatch_csv (root, "trusted-tool", "account-1", &frame);
  g_assert_null (csv);
  g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_ERROR);
  g_assert_cmpint (frame.error.error_class, ==,
      WYREBOX_DAEMON_ERROR_PERMANENT_FAILURE);
  remove_tree (root);
}

static void
test_wirelog_service_uses_existing_auth_and_scope_rejection (void)
{
  g_autofree char *root = make_journal_root ();
  g_auto (WyreboxDaemonResponseFrame) frame = { 0 };
  g_autofree char *csv = NULL;

  csv = dispatch_csv (root, "postfix-helper", "account-1", &frame);
  g_assert_null (csv);
  g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_ERROR);
  g_assert_cmpint (frame.error.error_class, ==,
      WYREBOX_DAEMON_ERROR_PERMISSION_DENIED);

  wyrebox_daemon_response_frame_clear (&frame);
  csv = dispatch_csv (root, "trusted-tool", "account-2", &frame);
  g_assert_null (csv);
  g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_ERROR);
  g_assert_cmpint (frame.error.error_class, ==,
      WYREBOX_DAEMON_ERROR_PERMISSION_DENIED);
  remove_tree (root);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func
      ("/daemon-api/wirelog-predicate-query/service-wirelog/returns-membership-csv",
      test_wirelog_service_returns_membership_csv);
  g_test_add_func
      ("/daemon-api/wirelog-predicate-query/service-wirelog/empty-result-header-only",
      test_wirelog_service_empty_result_is_header_only);
  g_test_add_func
      ("/daemon-api/wirelog-predicate-query/service-wirelog/excludes-retracted-facts",
      test_wirelog_service_excludes_retracted_facts);
  g_test_add_func
      ("/daemon-api/wirelog-predicate-query/service-wirelog/isolates-account-scope",
      test_wirelog_service_isolates_account_scope);
  g_test_add_func
      ("/daemon-api/wirelog-predicate-query/service-wirelog/rejects-invalid-rules",
      test_wirelog_service_rejects_invalid_rules);
  g_test_add_func
      ("/daemon-api/wirelog-predicate-query/service-wirelog/reports-malformed-journal",
      test_wirelog_service_reports_malformed_journal);
  g_test_add_func
      ("/daemon-api/wirelog-predicate-query/service-wirelog/reuses-auth-and-scope-rejection",
      test_wirelog_service_uses_existing_auth_and_scope_rejection);

  return g_test_run ();
}
