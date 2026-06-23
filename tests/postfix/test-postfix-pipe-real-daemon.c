#include "wyrebox-daemon-config.h"
#include "wyrebox-daemon-runtime.h"
#include "wyrebox-journal-reader.h"
#include "wyrebox-local-object-store.h"
#include "wyrebox-message-delivered-payload.h"
#include "wyrebox-schema-metadata-store.h"

#include <gio/gio.h>
#include <glib.h>
#include <glib/gstdio.h>

#include <signal.h>
#include <string.h>
#include <sysexits.h>

typedef struct
{
  int exit_status;
  GBytes *stdout_bytes;
  GBytes *stderr_bytes;
} ProcessResult;

static void
process_result_clear (ProcessResult *result)
{
  if (result == NULL)
    return;

  result->exit_status = -1;
  g_clear_pointer (&result->stdout_bytes, g_bytes_unref);
  g_clear_pointer (&result->stderr_bytes, g_bytes_unref);
}

/* *INDENT-OFF* */
G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (ProcessResult, process_result_clear)
/* *INDENT-ON* */

static char *
make_temp_root (const char *prefix)
{
  g_autoptr (GError) error = NULL;
  g_autofree char *root = g_dir_make_tmp (prefix, &error);

  g_assert_no_error (error);
  g_assert_nonnull (root);
  return g_steal_pointer (&root);
}

static gboolean
wait_for_socket (const char *socket_path)
{
  for (guint i = 0; i < 200; i++) {
    if (g_file_test (socket_path, G_FILE_TEST_EXISTS))
      return TRUE;
    g_usleep (10 * 1000);
  }

  return FALSE;
}

static const char *
wyreboxd_executable (void)
{
  const char *path = g_getenv ("WYREBOXD_EXECUTABLE");

  g_assert_nonnull (path);
  g_assert_cmpstr (path, !=, "");
  return path;
}

static const char *
postfix_pipe_executable (void)
{
  const char *path = g_getenv ("WYREBOX_POSTFIX_PIPE_EXECUTABLE");

  g_assert_nonnull (path);
  g_assert_cmpstr (path, !=, "");
  return path;
}

static void
write_config (const char *path, const char *socket_path,
    const char *journal_root_dir, const char *object_root_dir,
    const char *catalog_path)
{
  g_autofree char *contents = g_strdup_printf ("[daemon]\n"
      "socket_path=%s\n" "journal_root_dir=%s\n" "object_root_dir=%s\n"
      "catalog_path=%s\n", socket_path, journal_root_dir, object_root_dir,
      catalog_path);
  g_autoptr (GError) error = NULL;

  g_assert_true (g_file_set_contents (path, contents, -1, &error));
  g_assert_no_error (error);
  g_assert_cmpint (chmod (path, 0600), ==, 0);
}

static void
bootstrap_catalog (const char *catalog_path)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxSchemaMetadataStore) store = NULL;

  store = wyrebox_schema_metadata_store_new_duckdb (catalog_path, &error);
  g_assert_no_error (error);
  g_assert_nonnull (store);

  g_assert_true (wyrebox_schema_metadata_store_apply_migration_operation
      (store,
          WYREBOX_SCHEMA_METADATA_STORE_MIGRATION_OPERATION_LEGACY_BOOTSTRAP,
          0, 1, &error));
  g_assert_no_error (error);
}

static void
run_pipe_helper (const char *config_socket, GBytes *message,
    ProcessResult *result)
{
  g_autoptr (GSubprocess) subprocess = NULL;
  g_autoptr (GBytes) stdin_bytes = NULL;
  g_autoptr (GBytes) stdout_bytes = NULL;
  g_autoptr (GBytes) stderr_bytes = NULL;
  g_autoptr (GError) error = NULL;
  const char *argv[] = {
    postfix_pipe_executable (),
    "--account-id", "account-1",
    "--delivery-id", "stable-delivery-1",
    "--queue-id", "queue-1",
    "--sender", "sender@example.com",
    "--recipient", "alice@example.com",
    "--socket", config_socket,
    NULL
  };

  process_result_clear (result);
  stdin_bytes = g_bytes_ref (message);
  subprocess = g_subprocess_newv (argv,
      (GSubprocessFlags) (G_SUBPROCESS_FLAGS_STDIN_PIPE |
          G_SUBPROCESS_FLAGS_STDOUT_PIPE |
          G_SUBPROCESS_FLAGS_STDERR_PIPE), &error);
  g_assert_no_error (error);
  g_assert_nonnull (subprocess);

  g_assert_true (g_subprocess_communicate (subprocess,
          stdin_bytes, NULL, &stdout_bytes, &stderr_bytes, &error));
  g_assert_no_error (error);

  result->exit_status = g_subprocess_get_exit_status (subprocess);
  result->stdout_bytes = g_steal_pointer (&stdout_bytes);
  result->stderr_bytes = g_steal_pointer (&stderr_bytes);
}

static void
assert_stream_contains (GBytes *bytes, const char *needle)
{
  gsize size = 0;
  g_autofree char *text = NULL;
  const guint8 *data = NULL;

  g_assert_nonnull (bytes);
  data = g_bytes_get_data (bytes, &size);
  text = g_strndup ((const char *) data, size);

  g_assert_nonnull (strstr (text, needle));
}

static void
assert_stream_omits (GBytes *bytes, const char *needle)
{
  gsize size = 0;
  g_autofree char *text = NULL;
  const guint8 *data = NULL;

  g_assert_nonnull (bytes);
  data = g_bytes_get_data (bytes, &size);
  text = g_strndup ((const char *) data, size);

  g_assert_null (strstr (text, needle));
}

static guint
count_message_delivered_records (const char *journal_root_dir,
    WyreboxMessageDeliveredPayload *out_payload, GBytes **out_raw_payload)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxJournalReader) reader = NULL;
  guint count = 0;

  reader = wyrebox_journal_reader_new (journal_root_dir, &error);
  g_assert_no_error (error);
  g_assert_nonnull (reader);

  while (TRUE) {
    g_auto (WyreboxJournalRecord) record = { 0 };
    gboolean eof = FALSE;

    if (!wyrebox_journal_reader_read_next (reader, &record, &eof, &error)) {
      g_assert_no_error (error);
      g_assert_true (eof);
      break;
    }

    if (record.event_type == WYREBOX_JOURNAL_EVENT_MESSAGE_DELIVERED) {
      g_autoptr (GError) decode_error = NULL;

      count++;
      if (out_raw_payload != NULL)
        *out_raw_payload = g_bytes_ref (record.payload);
      if (out_payload != NULL) {
        g_assert_true (wyrebox_message_delivered_payload_decode (record.payload,
                out_payload, &decode_error));
        g_assert_no_error (decode_error);
      }
    }
  }

  return count;
}

static void
test_pipe_helper_delivers_through_wyreboxd_and_persists_journal (void)
{
  const guint8 message[] =
      "From: sender@example.com\r\n"
      "To: alice@example.com\r\n"
      "Subject: wyrebox real daemon delivery\r\n" "\r\n" "body\r\n";
  g_autofree char *root = make_temp_root ("wyrebox-pipe-real-daemon-XXXXXX");
  g_autofree char *run_dir = g_build_filename (root, "run", "wyrebox", NULL);
  g_autofree char *journal_dir = g_build_filename (root, "journal", NULL);
  g_autofree char *object_dir = g_build_filename (root, "objects", NULL);
  g_autofree char *catalog_path = g_build_filename (root, "catalog.duckdb",
      NULL);
  g_autofree char *config_path = g_build_filename (root, "wyrebox.conf", NULL);
  g_autofree char *socket_path = g_build_filename (run_dir, "wyrebox.sock",
      NULL);
  g_autoptr (GSubprocess) daemon = NULL;
  g_autoptr (GError) error = NULL;
  g_auto (ProcessResult) helper_result = { 0 };
  g_auto (WyreboxMessageDeliveredPayload) payload = { 0 };
  g_autoptr (WyreboxLocalObjectStore) object_store = NULL;
  g_autoptr (GBytes) message_bytes = NULL;
  g_autoptr (GBytes) object_bytes = NULL;
  gsize object_size = 0;
  const guint8 *object_data = NULL;
  const char *expected_object_key = NULL;

  g_assert_cmpint (g_mkdir_with_parents (run_dir, 0750), ==, 0);
  g_assert_cmpint (g_mkdir_with_parents (journal_dir, 0750), ==, 0);
  g_assert_cmpint (g_mkdir_with_parents (object_dir, 0750), ==, 0);
  bootstrap_catalog (catalog_path);
  write_config (config_path, socket_path, journal_dir, object_dir,
      catalog_path);
  message_bytes = g_bytes_new_static (message, sizeof (message) - 1);

  const char *daemon_argv[] = {
    wyreboxd_executable (),
    "--config",
    config_path,
    NULL
  };

  daemon = g_subprocess_newv (daemon_argv,
      (GSubprocessFlags) (G_SUBPROCESS_FLAGS_STDOUT_PIPE |
          G_SUBPROCESS_FLAGS_STDERR_PIPE), &error);
  g_assert_no_error (error);
  g_assert_nonnull (daemon);

  g_assert_true (wait_for_socket (socket_path));
  run_pipe_helper (socket_path, message_bytes, &helper_result);

  g_assert_cmpint (helper_result.exit_status, ==, EX_OK);
  assert_stream_contains (helper_result.stderr_bytes, "outcome=delivered");
  assert_stream_contains (helper_result.stderr_bytes, "exit_status=0");
  assert_stream_contains (helper_result.stderr_bytes,
      "durable_marker=journal:");
  assert_stream_omits (helper_result.stderr_bytes,
      "wyrebox real daemon delivery");

  g_assert_cmpuint (count_message_delivered_records (journal_dir, &payload,
          NULL), ==, 1);
  g_assert_cmpstr (payload.delivery_id, ==, "stable-delivery-1");
  g_assert_cmpstr (payload.queue_id, ==, "queue-1");
  g_assert_cmpstr (payload.account_identity, ==, "account-1");
  g_assert_cmpstr (payload.envelope_sender, ==, "sender@example.com");
  g_assert_cmpstr (payload.recipients[0], ==, "alice@example.com");
  g_assert_null (payload.recipients[1]);
  g_assert_cmpuint (payload.size_bytes, ==, sizeof (message) - 1);

  object_store = wyrebox_local_object_store_open_existing (object_dir, &error);
  g_assert_no_error (error);
  g_assert_nonnull (object_store);

  expected_object_key = payload.object_key;
  object_bytes = wyrebox_local_object_store_get_bytes (object_store,
      expected_object_key, &error);
  g_assert_no_error (error);
  g_assert_nonnull (object_bytes);

  object_data = (const guint8 *) g_bytes_get_data (object_bytes, &object_size);
  g_assert_cmpuint (object_size, ==, sizeof (message) - 1);
  g_assert_cmpmem (object_data, object_size, message, sizeof (message) - 1);

  g_subprocess_send_signal (daemon, SIGTERM);
  g_assert_true (g_subprocess_wait (daemon, NULL, &error));
  g_assert_no_error (error);
  g_assert_cmpint (g_subprocess_get_exit_status (daemon), ==, 0);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func
      ("/postfix/pipe/real-daemon-delivery-persists-journal",
      test_pipe_helper_delivers_through_wyreboxd_and_persists_journal);

  return g_test_run ();
}
