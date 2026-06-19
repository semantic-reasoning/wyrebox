#include "wyrebox-admin-socket-status.h"
#include "wyrebox-admin-health.h"
#include "wyrebox-daemon-runtime.h"
#include "wyrebox-journal-writer.h"
#include "wyrebox-schema-metadata-store.h"

#include <gio/gio.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

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
create_tmp_dir (void)
{
  g_autoptr (GError) error = NULL;
  char *dir = NULL;

  dir = g_dir_make_tmp ("wyrebox-admin-socket-status-XXXXXX", &error);
  g_assert_no_error (error);
  g_assert_nonnull (dir);

  return dir;
}

static int
create_listening_socket (const char *path)
{
  struct sockaddr_un addr = { 0 };
  int fd = -1;

  fd = socket (AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  g_assert_cmpint (fd, >=, 0);

  addr.sun_family = AF_UNIX;
  g_assert_cmpuint (strlen (path), <, sizeof (addr.sun_path));
  g_strlcpy (addr.sun_path, path, sizeof (addr.sun_path));

  g_assert_cmpint (bind (fd, (struct sockaddr *) &addr, sizeof (addr)), ==, 0);
  g_assert_cmpint (listen (fd, 1), ==, 0);

  return fd;
}

typedef struct
{
  int fd;
} AcceptThreadData;

static gpointer
accept_one_connection (gpointer user_data)
{
  AcceptThreadData *data = user_data;
  struct sockaddr_un addr = { 0 };
  socklen_t addrlen = sizeof (addr);
  int client_fd = -1;

  client_fd = accept (data->fd, (struct sockaddr *) &addr, &addrlen);
  if (client_fd >= 0)
    close (client_fd);

  return NULL;
}

static void
assert_cli (char **argv,
    int expected_status, const char *stdout_substring,
    const char *stderr_substring)
{
  g_autoptr (GError) error = NULL;
  g_autofree char *stdout_text = NULL;
  g_autofree char *stderr_text = NULL;
  int wait_status = 0;

  g_assert_true (g_spawn_sync (NULL, argv, NULL, G_SPAWN_DEFAULT, NULL, NULL,
          &stdout_text, &stderr_text, &wait_status, &error));
  g_assert_no_error (error);
  g_assert_true (WIFEXITED (wait_status));
  g_assert_cmpint (WEXITSTATUS (wait_status), ==, expected_status);

  if (stdout_substring != NULL)
    g_assert_nonnull (g_strstr_len (stdout_text, -1, stdout_substring));
  if (stderr_substring != NULL)
    g_assert_nonnull (g_strstr_len (stderr_text, -1, stderr_substring));
}

static void
test_default_socket_path_uses_daemon_runtime_constant (void)
{
  g_assert_cmpstr (wyrebox_admin_socket_status_default_socket_path (), ==,
      wyrebox_daemon_runtime_get_default_socket_path ());
}

static void
test_missing_path (void)
{
  g_autofree char *dir = create_tmp_dir ();
  g_autofree char *path = g_build_filename (dir, "missing.sock", NULL);
  g_auto (WyreboxAdminSocketStatusResult) result = { 0 };

  g_assert_cmpint (wyrebox_admin_socket_status_probe (path, &result), ==,
      WYREBOX_ADMIN_SOCKET_STATUS_EXIT_MISSING_PATH);
  g_assert_cmpstr (result.socket_path, ==, path);
  g_assert_cmpstr (result.status_name, ==, "missing-path");

  remove_tree (dir);
}

static void
test_regular_file_is_not_socket (void)
{
  g_autofree char *dir = create_tmp_dir ();
  g_autofree char *path = g_build_filename (dir, "regular", NULL);
  g_auto (WyreboxAdminSocketStatusResult) result = { 0 };
  g_autoptr (GError) error = NULL;

  g_assert_true (g_file_set_contents (path, "not a socket\n", -1, &error));
  g_assert_no_error (error);

  g_assert_cmpint (wyrebox_admin_socket_status_probe (path, &result), ==,
      WYREBOX_ADMIN_SOCKET_STATUS_EXIT_NOT_SOCKET);
  g_assert_cmpstr (result.status_name, ==, "not-socket");

  remove_tree (dir);
}

static void
test_live_socket_is_connectable (void)
{
  g_autofree char *dir = create_tmp_dir ();
  g_autofree char *path = g_build_filename (dir, "wyrebox.sock", NULL);
  g_auto (WyreboxAdminSocketStatusResult) result = { 0 };
  int fd = -1;

  fd = create_listening_socket (path);

  g_assert_cmpint (wyrebox_admin_socket_status_probe (path, &result), ==,
      WYREBOX_ADMIN_SOCKET_STATUS_EXIT_SUCCESS);
  g_assert_cmpstr (result.status_name, ==, "ok");
  g_assert_true (result.connectable);

  close (fd);
  remove_tree (dir);
}

static void
test_stale_socket_is_not_connectable (void)
{
  g_autofree char *dir = create_tmp_dir ();
  g_autofree char *path = g_build_filename (dir, "stale.sock", NULL);
  g_auto (WyreboxAdminSocketStatusResult) result = { 0 };
  int fd = -1;

  fd = create_listening_socket (path);
  close (fd);

  g_assert_cmpint (wyrebox_admin_socket_status_probe (path, &result), ==,
      WYREBOX_ADMIN_SOCKET_STATUS_EXIT_CONNECT_FAILED);
  g_assert_cmpstr (result.status_name, ==, "connect-failed");
  g_assert_false (result.connectable);

  remove_tree (dir);
}

static void
test_cli_human_and_json_output (void)
{
  const char *admin = g_getenv ("WYREBOX_ADMIN_EXECUTABLE");
  g_autofree char *dir = create_tmp_dir ();
  g_autofree char *path = g_build_filename (dir, "wyrebox.sock", NULL);
  int fd = -1;
  char *human_argv[] = { (char *) admin, (char *) "socket-status",
    (char *) "--socket-path", path, NULL
  };
  char *json_argv[] = { (char *) admin, (char *) "socket-status",
    (char *) "--socket-path", path, (char *) "--json", NULL
  };

  g_assert_nonnull (admin);
  fd = create_listening_socket (path);

  assert_cli (human_argv, 0, "status: ok", NULL);
  assert_cli (json_argv, 0, "\"status\":\"ok\"", NULL);

  close (fd);
  remove_tree (dir);
}

static void
test_health_probe_and_cli_output (void)
{
  const char *admin = g_getenv ("WYREBOX_ADMIN_EXECUTABLE");
  g_autofree char *dir = create_tmp_dir ();
  g_autofree char *path = g_build_filename (dir, "wyrebox.sock", NULL);
  g_auto (WyreboxAdminHealthResult) result = { 0 };
  AcceptThreadData accept_data = { 0 };
  g_autoptr (GThread) accept_thread = NULL;
  int fd = -1;
  char *human_argv[] = { (char *) admin, (char *) "health",
    (char *) "--socket-path", path, NULL
  };
  char *json_argv[] = { (char *) admin, (char *) "health",
    (char *) "--socket-path", path, (char *) "--json", NULL
  };

  g_assert_nonnull (admin);
  fd = create_listening_socket (path);
  accept_data.fd = fd;
  accept_thread = g_thread_new ("wyrebox-admin-health-accept",
      accept_one_connection, &accept_data);

  g_assert_cmpint (wyrebox_admin_health_probe (path, &result), ==, 0);
  g_assert_true (result.healthy);
  g_assert_cmpstr (result.status_name, ==, "ok");

  assert_cli (human_argv, 0, "healthy=true", NULL);
  assert_cli (json_argv, 0, "\"healthy\":true", NULL);

  g_thread_join (g_steal_pointer (&accept_thread));
  close (fd);
  remove_tree (dir);
}

static void
test_cli_usage_error_output (void)
{
  const char *admin = g_getenv ("WYREBOX_ADMIN_EXECUTABLE");
  char *argv[] = { (char *) admin, (char *) "unknown-command", NULL };

  g_assert_nonnull (admin);
  assert_cli (argv, WYREBOX_ADMIN_SOCKET_STATUS_EXIT_USAGE_ERROR, NULL,
      "Usage:");
}

static void
test_journal_position_cli_output (void)
{
  const char *admin = g_getenv ("WYREBOX_ADMIN_EXECUTABLE");
  g_autofree char *dir = create_tmp_dir ();
  g_autofree char *journal_root = g_build_filename (dir, "journal", NULL);
  g_autoptr (WyreboxJournalWriter) writer = NULL;
  g_autoptr (GBytes) payload = NULL;
  g_autoptr (GError) error = NULL;
  char payload_bytes[] = "event-payload";
  guint64 offset = 0;
  guint64 sequence = 0;
  char *human_argv[] = { (char *) admin, (char *) "journal-position",
    (char *) "--journal-root", journal_root, NULL
  };
  char *json_argv[] = { (char *) admin, (char *) "journal-position",
    (char *) "--journal-root", journal_root, (char *) "--json", NULL
  };

  g_assert_nonnull (admin);
  g_assert_cmpint (g_mkdir_with_parents (journal_root, 0755), ==, 0);

  writer = wyrebox_journal_writer_new (journal_root, &error);
  g_assert_no_error (error);
  g_assert_nonnull (writer);

  payload = g_bytes_new_static (payload_bytes, sizeof payload_bytes - 1);
  g_assert_true (wyrebox_journal_writer_append (writer,
          WYREBOX_JOURNAL_EVENT_MESSAGE_DELIVERED, payload,
          &offset, &sequence, &error));
  g_assert_no_error (error);
  g_assert_cmpuint (offset, ==, 0);
  g_assert_cmpuint (sequence, ==, 1);

  assert_cli (human_argv, 0, "last_offset=0", NULL);
  assert_cli (json_argv, 0, "\"last_offset\":0", NULL);

  remove_tree (dir);
}

static void
seed_materialization_manifest (const char *catalog_path,
    const char *run_id, guint64 start_offset, guint64 start_sequence,
    guint64 end_offset, guint64 end_sequence)
{
  g_autoptr (WyreboxSchemaMetadataStore) store = NULL;
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxMaterializationManifest) manifest = { 0 };

  store = wyrebox_schema_metadata_store_new_duckdb (catalog_path, &error);
  g_assert_no_error (error);
  g_assert_nonnull (store);

  manifest.run_id = g_strdup (run_id);
  manifest.start_journal_offset = start_offset;
  manifest.start_journal_sequence = start_sequence;
  manifest.end_journal_offset = end_offset;
  manifest.end_journal_sequence = end_sequence;
  manifest.materialized_schema_version =
      wyrebox_schema_migration_get_current_schema_version ();
  manifest.object_store_identity = g_strdup ("object-store");
  manifest.rule_package_version = g_strdup ("rules");
  manifest.view_package_version = g_strdup ("views");
  manifest.engine_version = g_strdup ("duckdb");
  manifest.created_at_unix_us = 1;
  manifest.completion_status = g_strdup ("completed");

  if (!wyrebox_schema_metadata_store_save_materialization_manifest (store,
          &manifest, &error))
    g_error ("save manifest failed: %s", error != NULL ? error->message :
        "unknown error");
}

static void
test_materialization_checkpoint_cli_output (void)
{
  const char *admin = g_getenv ("WYREBOX_ADMIN_EXECUTABLE");
  g_autofree char *dir = create_tmp_dir ();
  g_autofree char *journal_root = g_build_filename (dir, "journal", NULL);
  g_autofree char *catalog_path = g_build_filename (dir, "catalog.duckdb",
      NULL);
  g_autoptr (WyreboxJournalWriter) writer = NULL;
  g_autoptr (GBytes) payload = NULL;
  g_autoptr (GError) error = NULL;
  char payload_bytes[] = "event-payload";
  guint64 offset = 0;
  guint64 sequence = 0;
  char *human_argv[] = {
    (char *) admin,
    (char *) "materialization-checkpoint",
    (char *) "--journal-root",
    (char *) journal_root,
    (char *) "--catalog-path",
    (char *) catalog_path,
    NULL
  };
  char *json_argv[] = {
    (char *) admin,
    (char *) "materialization-checkpoint",
    (char *) "--journal-root",
    (char *) journal_root,
    (char *) "--catalog-path",
    (char *) catalog_path,
    (char *) "--json",
    NULL
  };

  g_assert_nonnull (admin);
  g_assert_cmpint (g_mkdir_with_parents (journal_root, 0755), ==, 0);

  writer = wyrebox_journal_writer_new (journal_root, &error);
  g_assert_no_error (error);
  g_assert_nonnull (writer);

  payload = g_bytes_new_static (payload_bytes, sizeof payload_bytes - 1);
  g_assert_true (wyrebox_journal_writer_append (writer,
          WYREBOX_JOURNAL_EVENT_MESSAGE_DELIVERED, payload,
          &offset, &sequence, &error));
  g_assert_no_error (error);
  g_assert_cmpuint (offset, ==, 0);
  g_assert_cmpuint (sequence, ==, 1);

  seed_materialization_manifest (catalog_path, "run-1", 10, 1, 20, 1);

  assert_cli (human_argv, 0, "stale=false", NULL);
  assert_cli (human_argv, 0, "journal_offset_lag=0", NULL);
  assert_cli (json_argv, 0, "\"stale\":false", NULL);
  assert_cli (json_argv, 0, "\"journal_offset_lag\":0", NULL);

  remove_tree (dir);
}

static void
test_materialization_checkpoint_cli_output_reports_stale_lag (void)
{
  const char *admin = g_getenv ("WYREBOX_ADMIN_EXECUTABLE");
  g_autofree char *dir = create_tmp_dir ();
  g_autofree char *journal_root = g_build_filename (dir, "journal", NULL);
  g_autofree char *catalog_path = g_build_filename (dir, "catalog.duckdb",
      NULL);
  g_autoptr (WyreboxJournalWriter) writer = NULL;
  g_autoptr (GBytes) payload = NULL;
  g_autoptr (GError) error = NULL;
  char payload_bytes[] = "event-payload";
  guint64 offset = 0;
  guint64 sequence = 0;
  char *human_argv[] = {
    (char *) admin,
    (char *) "materialization-checkpoint",
    (char *) "--journal-root",
    (char *) journal_root,
    (char *) "--catalog-path",
    (char *) catalog_path,
    NULL
  };
  char *json_argv[] = {
    (char *) admin,
    (char *) "materialization-checkpoint",
    (char *) "--journal-root",
    (char *) journal_root,
    (char *) "--catalog-path",
    (char *) catalog_path,
    (char *) "--json",
    NULL
  };

  g_assert_nonnull (admin);
  g_assert_cmpint (g_mkdir_with_parents (journal_root, 0755), ==, 0);

  writer = wyrebox_journal_writer_new (journal_root, &error);
  g_assert_no_error (error);
  g_assert_nonnull (writer);

  payload = g_bytes_new_static (payload_bytes, sizeof payload_bytes - 1);
  g_assert_true (wyrebox_journal_writer_append (writer,
          WYREBOX_JOURNAL_EVENT_MESSAGE_DELIVERED, payload,
          &offset, &sequence, &error));
  g_assert_no_error (error);
  g_assert_cmpuint (offset, >=, 0);
  g_assert_cmpuint (sequence, ==, 1);
  g_assert_true (wyrebox_journal_writer_append (writer,
          WYREBOX_JOURNAL_EVENT_MESSAGE_DELIVERED, payload,
          &offset, &sequence, &error));
  g_assert_no_error (error);
  g_assert_cmpuint (offset, >, 0);
  g_assert_cmpuint (sequence, ==, 2);

  seed_materialization_manifest (catalog_path, "run-1", 0, 1, 0, 1);

  assert_cli (human_argv, 0, "stale=true", NULL);
  assert_cli (human_argv, 0, "journal_sequence_lag=1", NULL);
  assert_cli (json_argv, 0, "\"stale\":true", NULL);
  assert_cli (json_argv, 0, "\"journal_sequence_lag\":1", NULL);

  remove_tree (dir);
}

static void
test_materialization_manifest_cli_output (void)
{
  const char *admin = g_getenv ("WYREBOX_ADMIN_EXECUTABLE");
  g_autofree char *dir = create_tmp_dir ();
  g_autofree char *catalog_path = g_build_filename (dir, "catalog.duckdb",
      NULL);
  g_autoptr (GError) error = NULL;
  char *human_argv[] = {
    (char *) admin,
    (char *) "materialization-manifest",
    (char *) "--catalog-path",
    (char *) catalog_path,
    NULL
  };
  char *json_argv[] = {
    (char *) admin,
    (char *) "materialization-manifest",
    (char *) "--catalog-path",
    (char *) catalog_path,
    (char *) "--json",
    NULL
  };

  g_assert_nonnull (admin);
  seed_materialization_manifest (catalog_path, "run-1", 10, 1, 20, 1);

  assert_cli (human_argv, 0, "run_id=run-1", NULL);
  assert_cli (json_argv, 0, "\"run_id\":\"run-1\"", NULL);

  remove_tree (dir);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/admin/socket-status/default-socket-path",
      test_default_socket_path_uses_daemon_runtime_constant);
  g_test_add_func ("/admin/socket-status/missing-path", test_missing_path);
  g_test_add_func ("/admin/socket-status/not-socket",
      test_regular_file_is_not_socket);
  g_test_add_func ("/admin/socket-status/connectable",
      test_live_socket_is_connectable);
  g_test_add_func ("/admin/socket-status/stale-socket",
      test_stale_socket_is_not_connectable);
  g_test_add_func ("/admin/socket-status/cli-output",
      test_cli_human_and_json_output);
  g_test_add_func ("/admin/health/probe-and-cli-output",
      test_health_probe_and_cli_output);
  g_test_add_func ("/admin/socket-status/cli-usage",
      test_cli_usage_error_output);
  g_test_add_func ("/admin/journal-position/cli-output",
      test_journal_position_cli_output);
  g_test_add_func ("/admin/materialization-checkpoint/cli-output",
      test_materialization_checkpoint_cli_output);
  g_test_add_func ("/admin/materialization-checkpoint/cli-output-stale",
      test_materialization_checkpoint_cli_output_reports_stale_lag);
  g_test_add_func ("/admin/materialization-manifest/cli-output",
      test_materialization_manifest_cli_output);

  return g_test_run ();
}
