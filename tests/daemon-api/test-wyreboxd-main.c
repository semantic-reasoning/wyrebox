#include <gio/gio.h>
#include <glib.h>
#include <glib/gstdio.h>

#include "wyrebox-build-config.h"
#include "wyrebox-daemon-frame-io.h"
#include "wyrebox-daemon-mailbox-list-result.h"
#include "wyrebox-daemon-mailbox-select-result.h"
#if defined(WYREBOX_HAVE_CAPNP_SERIALIZATION) && \
    WYREBOX_HAVE_CAPNP_SERIALIZATION
#include "wyrebox-daemon-capnp-codec.h"
#endif
#include "wyrebox-daemon-runtime.h"
#include "wyrebox-dovecot-daemon-client.h"
#include "wyrebox-eml-ingestor.h"
#include "wyrebox-journal-writer.h"
#include "wyrebox-local-object-store.h"

#include <signal.h>
#include <string.h>
#include <sys/stat.h>
#include <duckdb.h>
#include <sysexits.h>

#define JOURNAL_SEGMENT_NAME "00000000000000000000.wbj"

static void
duckdb_connection_clear (duckdb_connection *connection)
{
    duckdb_disconnect (connection);
}

/* *INDENT-OFF* */
G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (duckdb_connection, duckdb_connection_clear)
/* *INDENT-ON* */

static void
duckdb_database_clear (duckdb_database *database)
{
    duckdb_close (database);
}

/* *INDENT-OFF* */
G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (duckdb_database, duckdb_database_clear)
/* *INDENT-ON* */

typedef struct
{
    char *root;
    char *journal_dir;
    char *object_dir;
    char *catalog_path;
    char *socket_path;
    char *config_path;
} DaemonRoot;

static void
remove_tree (const char *path)
{
    g_autoptr (GDir) dir = g_dir_open (path, 0, NULL);
    const char *name = NULL;

    if (dir == NULL) {
        (void)g_remove (path);
        return;
    }

    while ((name = g_dir_read_name (dir)) != NULL) {
        g_autofree char *child = g_build_filename (path, name, NULL);

        remove_tree (child);
    }

    (void)g_rmdir (path);
}

static void
daemon_root_clear (DaemonRoot *daemon_root)
{
    if (daemon_root->root != NULL)
        remove_tree (daemon_root->root);
    g_clear_pointer (&daemon_root->root, g_free);
    g_clear_pointer (&daemon_root->journal_dir, g_free);
    g_clear_pointer (&daemon_root->object_dir, g_free);
    g_clear_pointer (&daemon_root->catalog_path, g_free);
    g_clear_pointer (&daemon_root->socket_path, g_free);
    g_clear_pointer (&daemon_root->config_path, g_free);
}

/* *INDENT-OFF* */
G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (DaemonRoot, daemon_root_clear)
/* *INDENT-ON* */

static void
daemon_root_init (DaemonRoot *daemon_root)
{
    g_autofree char *run_dir = NULL;
    g_autofree char *config_contents = NULL;
    g_autoptr (GError) error = NULL;

    daemon_root->root = g_dir_make_tmp ("wyreboxd-main-test-XXXXXX", NULL);
    g_assert_nonnull (daemon_root->root);
    run_dir = g_build_filename (daemon_root->root, "run", "wyrebox", NULL);
    daemon_root->journal_dir = g_build_filename (daemon_root->root, "journal",
            NULL);
    daemon_root->object_dir = g_build_filename (daemon_root->root, "objects",
            NULL);
    daemon_root->catalog_path = g_build_filename (daemon_root->root,
            "catalog.duckdb", NULL);
    daemon_root->socket_path = g_build_filename (run_dir, "wyrebox.sock",
            NULL);
    daemon_root->config_path = g_build_filename (daemon_root->root,
            "wyrebox.conf", NULL);

    g_assert_cmpint (g_mkdir_with_parents (run_dir, 0750), ==, 0);
    g_assert_cmpint (g_mkdir_with_parents (daemon_root->journal_dir, 0750), ==,
        0);
    g_assert_cmpint (g_mkdir_with_parents (daemon_root->object_dir, 0750), ==,
        0);

    config_contents = g_strdup_printf ("[daemon]\n"
            "socket_path=%s\n"
            "journal_root_dir=%s\n"
            "object_root_dir=%s\n"
            "catalog_path=%s\n", daemon_root->socket_path,
            daemon_root->journal_dir, daemon_root->object_dir,
            daemon_root->catalog_path);
    g_assert_true (g_file_set_contents (daemon_root->config_path,
        config_contents, -1, &error));
    g_assert_no_error (error);
    g_assert_cmpint (chmod (daemon_root->config_path, 0600), ==, 0);
}

static const char *
wyreboxd_executable (void)
{
    const char *path = g_getenv ("WYREBOXD_EXECUTABLE");

    g_assert_nonnull (path);
    g_assert_cmpstr (path, !=, "");
    return path;
}

static GSubprocess *
spawn_daemon (const DaemonRoot *daemon_root, GSubprocessFlags stderr_flag)
{
    g_autoptr (GError) error = NULL;
    GSubprocess *subprocess = NULL;
    const char *argv[] = {
        wyreboxd_executable (),
        "--config",
        daemon_root->config_path,
        NULL
    };

    (void)g_remove (daemon_root->socket_path);
    subprocess = g_subprocess_newv (argv,
            (GSubprocessFlags)(G_SUBPROCESS_FLAGS_STDOUT_SILENCE | stderr_flag),
            &error);
    g_assert_no_error (error);
    g_assert_nonnull (subprocess);

    return subprocess;
}

/*
 * Startup validates storage, prepares the catalog and catches up before the
 * socket appears, which is slow under sanitizers, so allow a generous bound
 * but stop as soon as the daemon exits.
 */
static gboolean
wait_for_socket (GSubprocess *subprocess, const char *socket_path)
{
    gint64 deadline = g_get_monotonic_time () + 30 * G_TIME_SPAN_SECOND;

    while (g_get_monotonic_time () < deadline) {
        if (g_file_test (socket_path, G_FILE_TEST_EXISTS))
            return TRUE;
        if (g_subprocess_get_identifier (subprocess) == NULL)
            return FALSE;
        g_usleep (10 * 1000);
    }

    return FALSE;
}

static GSubprocess *
start_daemon (const DaemonRoot *daemon_root)
{
    GSubprocess *subprocess = spawn_daemon (daemon_root,
            G_SUBPROCESS_FLAGS_STDERR_SILENCE);

    g_assert_true (wait_for_socket (subprocess, daemon_root->socket_path));
    return subprocess;
}

static void
stop_daemon (GSubprocess *subprocess)
{
    g_autoptr (GError) error = NULL;

    g_subprocess_send_signal (subprocess, SIGTERM);
    g_assert_true (g_subprocess_wait (subprocess, NULL, &error));
    g_assert_no_error (error);
    g_assert_cmpint (g_subprocess_get_exit_status (subprocess), ==, 0);
}

static void
assert_daemon_startup_fails (const DaemonRoot *daemon_root, int exit_status,
    const char *stderr_substring)
{
    g_autoptr (GSubprocess) subprocess = spawn_daemon (daemon_root,
            G_SUBPROCESS_FLAGS_STDERR_PIPE);
    g_autoptr (GError) error = NULL;
    g_autofree char *stderr_text = NULL;

    g_assert_true (g_subprocess_communicate_utf8 (subprocess, NULL, NULL,
        NULL, &stderr_text, &error));
    g_assert_no_error (error);
    g_assert_cmpint (g_subprocess_get_exit_status (subprocess), ==,
        exit_status);
    g_assert_nonnull (strstr (stderr_text, stderr_substring));
    g_assert_false (g_file_test (daemon_root->socket_path, G_FILE_TEST_EXISTS));
}

static GBytes *
build_message (const char *subject)
{
    g_autofree char *message = g_strdup_printf (
        "From: sender@example.com\r\n"
        "To: recipient@example.com\r\n"
        "Subject: %s\r\n" "\r\n" "body\r\n", subject);
    gsize length = strlen (message);

    return g_bytes_new_take (g_steal_pointer (&message), length);
}

/*
 * Appends a delivery to the journal from the test process while wyreboxd is
 * not running. A NULL @account_id records a pre-delivery-identity payload.
 */
static void
journal_delivery_offline (const DaemonRoot *daemon_root,
    const char *delivery_id, const char *account_id)
{
    const gchar *const recipients[] = { "recipient@example.com", NULL };
    g_autoptr (WyreboxLocalObjectStore) object_store = NULL;
    g_autoptr (WyreboxJournalWriter) writer = NULL;
    g_autoptr (WyreboxEmlIngestor) ingestor = NULL;
    g_autoptr (GBytes) message = build_message (delivery_id);
    g_auto (WyreboxEmlIngestResult) result = { 0 };
    g_autoptr (GError) error = NULL;

    object_store = wyrebox_local_object_store_new (daemon_root->object_dir,
            &error);
    g_assert_no_error (error);
    writer = wyrebox_journal_writer_new (daemon_root->journal_dir, &error);
    g_assert_no_error (error);
    ingestor = wyrebox_eml_ingestor_new_with_journal (object_store, writer);

    if (account_id == NULL) {
        g_assert_true (wyrebox_eml_ingestor_ingest_bytes (ingestor, message,
            &result, &error));
    } else {
        g_assert_true (wyrebox_eml_ingestor_ingest_delivery_bytes (ingestor,
            message, delivery_id, NULL, account_id, "sender@example.com",
            recipients, &result, &error));
    }
    g_assert_no_error (error);
}

static void
exec_catalog_sql (const char *catalog_path, const char *sql)
{
    g_auto (duckdb_database) database = NULL;
    g_auto (duckdb_connection) connection = NULL;
    duckdb_result result = { 0 };

    g_assert_cmpint (duckdb_open (catalog_path, &database), ==, DuckDBSuccess);
    g_assert_cmpint (duckdb_connect (database, &connection), ==, DuckDBSuccess);
    if (duckdb_query (connection, sql, &result) != DuckDBSuccess)
        g_error ("duckdb query failed: %s", duckdb_result_error (&result));
    duckdb_destroy_result (&result);
}

static char *
query_catalog_string (const char *catalog_path, const char *sql)
{
    g_auto (duckdb_database) database = NULL;
    g_auto (duckdb_connection) connection = NULL;
    duckdb_result result = { 0 };
    char *value = NULL;
    char *copy = NULL;

    g_assert_cmpint (duckdb_open (catalog_path, &database), ==, DuckDBSuccess);
    g_assert_cmpint (duckdb_connect (database, &connection), ==, DuckDBSuccess);
    if (duckdb_query (connection, sql, &result) != DuckDBSuccess)
        g_error ("duckdb query failed: %s", duckdb_result_error (&result));
    g_assert_cmpuint (duckdb_row_count (&result), ==, 1);
    value = duckdb_value_varchar (&result, 0, 0);
    copy = g_strdup (value != NULL ? value : "");
    duckdb_free (value);
    duckdb_destroy_result (&result);

    return copy;
}

/*
 * Summarizes every mailbox membership, UID namespace state and the
 * materialization checkpoint, so catalogs can be compared after a rebuild.
 */
static char *
catalog_snapshot (const char *catalog_path)
{
    g_autofree char *memberships = query_catalog_string (catalog_path,
            "SELECT string_agg(b.account_id || '/' || b.imap_name || '/' || "
            "b.mailbox_id || '@' || m.journal_offset || ':' || "
            "m.journal_sequence || '=' || m.uid, ';' "
            "ORDER BY m.journal_offset) FROM mailbox_memberships m "
            "JOIN mailboxes b ON b.mailbox_id = m.mailbox_id;");
    g_autofree char *uid_state = query_catalog_string (catalog_path,
            "SELECT string_agg(namespace_id || ' next=' || uidnext || "
            "' validity=' || uidvalidity, ';' ORDER BY namespace_id) "
            "FROM mailbox_uid_state;");
    g_autofree char *checkpoint = query_catalog_string (catalog_path,
            "SELECT journal_offset || ':' || journal_sequence "
            "FROM materialization_checkpoint "
            "WHERE checkpoint_key = 'materialization';");

    return g_strdup_printf ("memberships[%s] uid_state[%s] checkpoint[%s]",
               memberships, uid_state, checkpoint);
}

#if defined(WYREBOX_HAVE_CAPNP_SERIALIZATION) && \
    WYREBOX_HAVE_CAPNP_SERIALIZATION
static GBytes *
build_delivery_request (const char *delivery_id)
{
    g_autoptr (GError) error = NULL;
    g_autoptr (GBytes) encoded = NULL;
    g_autoptr (GBytes) message = build_message (delivery_id);
    g_auto (WyreboxDaemonRequestIdentity) identity = { 0 };
    g_auto (WyreboxDaemonDeliveryIngestionRequest) request = { 0 };
    const gchar *const recipients[] = { "recipient@example.com", NULL };

    g_assert_true (wyrebox_daemon_request_identity_init (&identity,
        "request-wyreboxd-1",
        "postfix",
        "account-1", "wyrebox-postfix-pipe", "corr-wyreboxd-1", &error));
    g_assert_no_error (error);

    g_assert_true (wyrebox_daemon_delivery_ingestion_request_init (&request,
        delivery_id,
        "queue-wyreboxd-1",
        "sender@example.com", recipients, message, &error));
    g_assert_no_error (error);

    encoded =
        wyrebox_daemon_capnp_codec_encode_delivery_ingestion_request (&identity,
            &request, NULL, &error);
    g_assert_no_error (error);
    g_assert_nonnull (encoded);

    return g_steal_pointer (&encoded);
}

static void
assert_success_response_roundtrip (GBytes *response)
{
    g_autoptr (GError) error = NULL;
    g_auto (WyreboxDaemonResponseFrame) frame = { 0 };

    g_assert_true (wyrebox_daemon_capnp_codec_decode_response_frame (response,
        &frame, &error));
    g_assert_no_error (error);
    g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_SUCCESS);
    g_assert_nonnull (frame.request_id);
}

static GBytes *
roundtrip_request (const char *socket_path, GBytes *request)
{
    g_autoptr (GBytes) response = NULL;
    g_autoptr (GError) error = NULL;
    g_autoptr (GSocketConnection) connection = NULL;
    GInputStream *input = NULL;
    GOutputStream *output = NULL;
    GSocket *socket = NULL;
    g_autoptr (GSocketClient) client = NULL;
    g_autoptr (GSocketAddress) address = NULL;

    client = g_socket_client_new ();
    address = g_unix_socket_address_new (socket_path);
    connection = g_socket_client_connect (client,
            G_SOCKET_CONNECTABLE (address),
            NULL, &error);
    g_assert_no_error (error);
    g_assert_nonnull (connection);

    input = g_io_stream_get_input_stream (G_IO_STREAM (connection));
    output = g_io_stream_get_output_stream (G_IO_STREAM (connection));
    socket = g_socket_connection_get_socket (connection);
    g_assert_nonnull (socket);

    g_assert_true (wyrebox_daemon_frame_io_write_payload (output,
        (const guint8 *)g_bytes_get_data (request, NULL),
        g_bytes_get_size (request), &error));
    g_assert_no_error (error);

    g_assert_true (g_socket_shutdown (socket, FALSE, TRUE, &error));
    g_assert_no_error (error);

    response = wyrebox_daemon_frame_io_read_payload (input, &error);
    g_assert_no_error (error);
    g_assert_nonnull (response);

    return g_steal_pointer (&response);
}

static void
deliver (const DaemonRoot *daemon_root, const char *delivery_id)
{
    g_autoptr (GBytes) request = build_delivery_request (delivery_id);
    g_autoptr (GBytes) response = roundtrip_request (daemon_root->socket_path,
            request);

    assert_success_response_roundtrip (response);
}

static void
assert_mailbox_inbox_state (const DaemonRoot *daemon_root,
    const char *mailbox_id, guint64 uid_next, guint64 message_count)
{
    g_auto (WyreboxDaemonMailboxSelectResult) select_result = { 0 };
    g_autoptr (GError) error = NULL;

    g_assert_true (wyrebox_dovecot_daemon_client_select_mailbox
            (daemon_root->socket_path, "account-1", "INBOX", &select_result,
        &error));
    g_assert_no_error (error);
    g_assert_cmpint (select_result.kind, ==,
        WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_ORDINARY);
    g_assert_cmpstr (select_result.mailbox_id, ==, mailbox_id);
    g_assert_cmpstr (select_result.mailbox_name, ==, "INBOX");
    g_assert_cmpuint (select_result.uid_validity, ==, 1);
    g_assert_cmpuint (select_result.uid_next, ==, uid_next);
    g_assert_cmpuint (select_result.message_count, ==, message_count);
}

static void
assert_inbox_state (const DaemonRoot *daemon_root, guint64 uid_next,
    guint64 message_count)
{
    assert_mailbox_inbox_state (daemon_root, "inbox:account-1", uid_next,
        message_count);
}
#endif

static void
test_wyreboxd_accepts_config_and_starts_socket (void)
{
    g_auto (DaemonRoot) daemon_root = { 0 };
    g_autoptr (GSubprocess) subprocess = NULL;

    daemon_root_init (&daemon_root);
    subprocess = start_daemon (&daemon_root);
    stop_daemon (subprocess);
}

static void
test_wyreboxd_rejects_invalid_config_path (void)
{
    g_autoptr (GSubprocess) subprocess = NULL;
    g_autoptr (GError) error = NULL;
    const char *argv[] = {
        wyreboxd_executable (),
        "--config",
        "/tmp/wyreboxd-does-not-exist.conf",
        NULL
    };

    subprocess = g_subprocess_newv (argv,
            (GSubprocessFlags)(G_SUBPROCESS_FLAGS_STDOUT_SILENCE |
            G_SUBPROCESS_FLAGS_STDERR_PIPE), &error);
    g_assert_no_error (error);
    g_assert_nonnull (subprocess);

    g_assert_true (g_subprocess_wait (subprocess, NULL, &error));
    g_assert_no_error (error);
    g_assert_cmpint (g_subprocess_get_exit_status (subprocess), ==, 78);
}

static void
test_wyreboxd_fails_startup_on_delivery_without_account (void)
{
    g_auto (DaemonRoot) daemon_root = { 0 };

    daemon_root_init (&daemon_root);
    journal_delivery_offline (&daemon_root, "delivery-legacy", NULL);

    assert_daemon_startup_fails (&daemon_root, EX_DATAERR,
        "has no account identity");
}

static void
test_wyreboxd_exits_tempfail_when_catalog_is_locked (void)
{
    g_auto (DaemonRoot) daemon_root = { 0 };
    g_auto (duckdb_database) database = NULL;

    daemon_root_init (&daemon_root);
    g_assert_cmpint (duckdb_open (daemon_root.catalog_path, &database), ==,
        DuckDBSuccess);

    assert_daemon_startup_fails (&daemon_root, EX_TEMPFAIL,
        "Could not set lock");
}

static void
test_wyreboxd_exits_dataerr_when_catalog_is_corrupt (void)
{
    g_auto (DaemonRoot) daemon_root = { 0 };
    g_autofree char *garbage = g_strnfill (8192, 'x');
    g_autoptr (GError) error = NULL;

    daemon_root_init (&daemon_root);
    g_assert_true (g_file_set_contents (daemon_root.catalog_path, garbage,
        8192, &error));
    g_assert_no_error (error);

    assert_daemon_startup_fails (&daemon_root, EX_DATAERR,
        "not a valid DuckDB database file");
}

static void
test_wyreboxd_exits_dataerr_on_newer_catalog_schema (void)
{
    g_auto (DaemonRoot) daemon_root = { 0 };
    g_autoptr (GError) error = NULL;

    daemon_root_init (&daemon_root);
    g_assert_true (wyrebox_daemon_runtime_prepare_catalog
            (daemon_root.journal_dir, daemon_root.catalog_path, FALSE,
        &error));
    g_assert_no_error (error);
    exec_catalog_sql (daemon_root.catalog_path,
        "UPDATE schema_metadata SET schema_version = schema_version + 1;");

    assert_daemon_startup_fails (&daemon_root, EX_DATAERR,
        "catalog preparation failed");
}

static void
test_wyreboxd_exits_dataerr_when_catalog_migration_needs_checkpoint (void)
{
    g_auto (DaemonRoot) daemon_root = { 0 };
    g_autoptr (GError) error = NULL;

    daemon_root_init (&daemon_root);
    g_assert_true (wyrebox_daemon_runtime_prepare_catalog
            (daemon_root.journal_dir, daemon_root.catalog_path, FALSE,
        &error));
    g_assert_no_error (error);
    exec_catalog_sql (daemon_root.catalog_path,
        "UPDATE schema_metadata SET schema_version = 0;");

    assert_daemon_startup_fails (&daemon_root, EX_DATAERR,
        "checkpoint precondition not satisfied");
}

static void
test_wyreboxd_exits_tempfail_when_object_root_is_missing (void)
{
    g_auto (DaemonRoot) daemon_root = { 0 };
    g_autofree char *sha256_dir = NULL;
    g_autofree char *objects_dir = NULL;

    daemon_root_init (&daemon_root);
    journal_delivery_offline (&daemon_root, "delivery-1", "account-1");
    objects_dir = g_build_filename (daemon_root.object_dir, "objects", NULL);
    sha256_dir = g_build_filename (objects_dir, "sha256", NULL);
    remove_tree (objects_dir);

    assert_daemon_startup_fails (&daemon_root, EX_TEMPFAIL,
        "check that the object store is mounted");
    g_assert_false (g_file_test (sha256_dir, G_FILE_TEST_EXISTS));
}

static char *
first_raw_object_path (const DaemonRoot *daemon_root)
{
    g_autofree char *sha256_dir = g_build_filename (daemon_root->object_dir,
            "objects", "sha256", NULL);
    g_autoptr (GDir) shards = g_dir_open (sha256_dir, 0, NULL);
    const char *shard = NULL;

    g_assert_nonnull (shards);
    while ((shard = g_dir_read_name (shards)) != NULL) {
        g_autofree char *shard_dir = g_build_filename (sha256_dir, shard, NULL);
        g_autoptr (GDir) objects = g_dir_open (shard_dir, 0, NULL);
        const char *name = NULL;

        while (objects != NULL &&
            (name = g_dir_read_name (objects)) != NULL) {
            if (g_str_has_suffix (name, ".eml"))
                return g_build_filename (shard_dir, name, NULL);
        }
    }

    g_assert_not_reached ();
    return NULL;
}

static void
test_wyreboxd_starts_with_missing_raw_object (void)
{
    g_auto (DaemonRoot) daemon_root = { 0 };
    g_autoptr (GSubprocess) subprocess = NULL;
    g_autoptr (GError) error = NULL;
    g_autofree char *object_path = NULL;
    g_autofree char *stderr_text = NULL;

    daemon_root_init (&daemon_root);
    journal_delivery_offline (&daemon_root, "delivery-1", "account-1");
    object_path = first_raw_object_path (&daemon_root);
    g_assert_cmpint (g_remove (object_path), ==, 0);

    subprocess = spawn_daemon (&daemon_root, G_SUBPROCESS_FLAGS_STDERR_PIPE);
    g_assert_true (wait_for_socket (subprocess, daemon_root.socket_path));
    g_subprocess_send_signal (subprocess, SIGTERM);
    g_assert_true (g_subprocess_communicate_utf8 (subprocess, NULL, NULL, NULL,
        &stderr_text, &error));
    g_assert_no_error (error);
    g_assert_cmpint (g_subprocess_get_exit_status (subprocess), ==, 0);
    g_assert_nonnull (strstr (stderr_text,
        "journaled deliveries with missing or corrupt raw objects: 1, first "
        "at journal sequence 1"));
    g_assert_nonnull (strstr (stderr_text,
        "delivery materialization held account account-1"));
    g_assert_false (g_file_test (object_path, G_FILE_TEST_EXISTS));
}

static void
test_wyreboxd_exits_tempfail_on_unreadable_raw_object (void)
{
    g_auto (DaemonRoot) daemon_root = { 0 };
    g_autofree char *object_path = NULL;

    daemon_root_init (&daemon_root);
    journal_delivery_offline (&daemon_root, "delivery-1", "account-1");
    object_path = first_raw_object_path (&daemon_root);
    g_assert_cmpint (g_remove (object_path), ==, 0);
    g_assert_cmpint (g_mkdir (object_path, 0700), ==, 0);

    assert_daemon_startup_fails (&daemon_root, EX_TEMPFAIL,
        "failed to read raw object");
}

static void
test_wyreboxd_exits_dataerr_on_corrupt_journal_record (void)
{
    g_auto (DaemonRoot) daemon_root = { 0 };
    g_autoptr (GError) error = NULL;
    g_autofree char *segment_path = NULL;
    g_autofree char *contents = NULL;
    gsize length = 0;

    daemon_root_init (&daemon_root);
    journal_delivery_offline (&daemon_root, "delivery-1", "account-1");
    journal_delivery_offline (&daemon_root, "delivery-2", "account-1");
    segment_path = g_build_filename (daemon_root.journal_dir,
            JOURNAL_SEGMENT_NAME, NULL);
    g_assert_true (g_file_get_contents (segment_path, &contents, &length,
        &error));
    g_assert_no_error (error);
    g_assert_cmpuint (length, >, 0);
    contents[length - 1] ^= 0x01;
    g_assert_true (g_file_set_contents (segment_path, contents, (gssize)length,
        &error));
    g_assert_no_error (error);

    assert_daemon_startup_fails (&daemon_root, EX_DATAERR,
        "delivery storage is invalid");
}

/*
 * Prepares the catalog offline with an unselectable account-1 INBOX that
 * WyreBox refuses to materialize into.
 */
static void
seed_unselectable_inbox (const DaemonRoot *daemon_root)
{
    g_autoptr (GError) error = NULL;

    g_assert_true (wyrebox_daemon_runtime_prepare_catalog
            (daemon_root->journal_dir, daemon_root->catalog_path, FALSE,
        &error));
    g_assert_no_error (error);
    exec_catalog_sql (daemon_root->catalog_path,
        "INSERT INTO accounts (account_id) VALUES ('account-1');");
    exec_catalog_sql (daemon_root->catalog_path,
        "INSERT INTO mailboxes (mailbox_id, account_id, imap_name, "
        "is_selectable, is_visible) VALUES "
        "('mailbox-inbox', 'account-1', 'INBOX', FALSE, TRUE);");
}

static void
test_wyreboxd_starts_with_held_account (void)
{
    g_auto (DaemonRoot) daemon_root = { 0 };
    g_autoptr (GSubprocess) subprocess = NULL;
    g_autoptr (GError) error = NULL;
    g_autofree char *stderr_text = NULL;

    daemon_root_init (&daemon_root);
    seed_unselectable_inbox (&daemon_root);
    journal_delivery_offline (&daemon_root, "delivery-1", "account-1");

    subprocess = spawn_daemon (&daemon_root, G_SUBPROCESS_FLAGS_STDERR_PIPE);
    g_assert_true (wait_for_socket (subprocess, daemon_root.socket_path));
    g_subprocess_send_signal (subprocess, SIGTERM);
    g_assert_true (g_subprocess_communicate_utf8 (subprocess, NULL, NULL, NULL,
        &stderr_text, &error));
    g_assert_no_error (error);
    g_assert_cmpint (g_subprocess_get_exit_status (subprocess), ==, 0);
    g_assert_nonnull (strstr (stderr_text,
        "delivery materialization held account account-1"));
    g_assert_nonnull (strstr (stderr_text, "retry in 5000 ms"));
}

#if defined(WYREBOX_HAVE_CAPNP_SERIALIZATION) && \
    WYREBOX_HAVE_CAPNP_SERIALIZATION
static void
test_wyreboxd_materializes_delivery_before_receipt (void)
{
    g_auto (DaemonRoot) daemon_root = { 0 };
    g_autoptr (GSubprocess) subprocess = NULL;
    g_auto (WyreboxDaemonMailboxListResult) list_result = { 0 };
    g_autoptr (GError) error = NULL;

    daemon_root_init (&daemon_root);
    subprocess = start_daemon (&daemon_root);

    deliver (&daemon_root, "delivery-1");
    assert_inbox_state (&daemon_root, 2, 1);

    g_assert_true (wyrebox_dovecot_daemon_client_list_mailboxes
            (daemon_root.socket_path, "account-1", "", &list_result, &error));
    g_assert_no_error (error);
    g_assert_cmpuint (wyrebox_daemon_mailbox_list_result_get_n_entries
            (&list_result), ==, 1);
    g_assert_cmpstr (wyrebox_daemon_mailbox_list_result_get_entry (&list_result,
        0)->mailbox_id, ==, "inbox:account-1");

    deliver (&daemon_root, "delivery-2");
    assert_inbox_state (&daemon_root, 3, 2);

    stop_daemon (subprocess);
}

static void
test_wyreboxd_catches_up_journal_on_startup (void)
{
    g_auto (DaemonRoot) daemon_root = { 0 };
    g_autoptr (GSubprocess) subprocess = NULL;

    daemon_root_init (&daemon_root);
    journal_delivery_offline (&daemon_root, "delivery-1", "account-1");
    journal_delivery_offline (&daemon_root, "delivery-2", "account-1");

    subprocess = start_daemon (&daemon_root);
    assert_inbox_state (&daemon_root, 3, 2);
    deliver (&daemon_root, "delivery-3");
    assert_inbox_state (&daemon_root, 4, 3);
    stop_daemon (subprocess);
}

static void
test_wyreboxd_rebuilds_identical_catalog_after_restart (void)
{
    g_auto (DaemonRoot) daemon_root = { 0 };
    g_autoptr (GSubprocess) first = NULL;
    g_autoptr (GSubprocess) restarted = NULL;
    g_autoptr (GSubprocess) rebuilt = NULL;
    g_autofree char *before = NULL;
    g_autofree char *after_restart = NULL;
    g_autofree char *after_rebuild = NULL;
    g_autofree char *catalog_wal_path = NULL;

    daemon_root_init (&daemon_root);
    catalog_wal_path = g_strconcat (daemon_root.catalog_path, ".wal", NULL);
    first = start_daemon (&daemon_root);
    deliver (&daemon_root, "delivery-1");
    deliver (&daemon_root, "delivery-2");
    stop_daemon (first);
    before = catalog_snapshot (daemon_root.catalog_path);
    g_assert_nonnull (strstr (before, "account-1/INBOX/inbox:account-1@"));
    g_assert_nonnull (strstr (before, "=2]"));
    g_assert_null (strstr (before, "checkpoint[]"));

    restarted = start_daemon (&daemon_root);
    assert_inbox_state (&daemon_root, 3, 2);
    stop_daemon (restarted);
    after_restart = catalog_snapshot (daemon_root.catalog_path);
    g_assert_cmpstr (after_restart, ==, before);

    g_assert_cmpint (g_remove (daemon_root.catalog_path), ==, 0);
    (void)g_remove (catalog_wal_path);
    rebuilt = start_daemon (&daemon_root);
    assert_inbox_state (&daemon_root, 3, 2);
    stop_daemon (rebuilt);
    after_rebuild = catalog_snapshot (daemon_root.catalog_path);
    g_assert_cmpstr (after_rebuild, ==, before);
}

static void
test_wyreboxd_acknowledges_delivery_when_materialization_fails (void)
{
    g_auto (DaemonRoot) daemon_root = { 0 };
    g_autoptr (GSubprocess) failing = NULL;
    g_autoptr (GSubprocess) restarted = NULL;
    g_autoptr (GError) error = NULL;
    g_autofree char *stderr_text = NULL;

    daemon_root_init (&daemon_root);
    seed_unselectable_inbox (&daemon_root);

    failing = spawn_daemon (&daemon_root, G_SUBPROCESS_FLAGS_STDERR_PIPE);
    g_assert_true (wait_for_socket (failing, daemon_root.socket_path));
    deliver (&daemon_root, "delivery-1");
    g_subprocess_send_signal (failing, SIGTERM);
    g_assert_true (g_subprocess_communicate_utf8 (failing, NULL, NULL, NULL,
        &stderr_text, &error));
    g_assert_no_error (error);
    g_assert_cmpint (g_subprocess_get_exit_status (failing), ==, 0);
    g_assert_nonnull (strstr (stderr_text,
        "delivery materialization held account account-1"));
    g_assert_nonnull (strstr (stderr_text, "retry in 5000 ms"));

    exec_catalog_sql (daemon_root.catalog_path,
        "UPDATE mailboxes SET is_selectable = TRUE "
        "WHERE mailbox_id = 'mailbox-inbox';");
    restarted = start_daemon (&daemon_root);
    assert_mailbox_inbox_state (&daemon_root, "mailbox-inbox", 2, 1);
    stop_daemon (restarted);
}

static void
test_wyreboxd_recovers_torn_journal_suffix (void)
{
    g_auto (DaemonRoot) daemon_root = { 0 };
    g_autoptr (GSubprocess) first = NULL;
    g_autoptr (GSubprocess) recovered = NULL;
    g_autoptr (GError) error = NULL;
    g_autofree char *segment_path = NULL;
    g_autofree char *contents = NULL;
    gsize length = 0;
    gsize committed_length = 0;

    daemon_root_init (&daemon_root);
    first = start_daemon (&daemon_root);
    deliver (&daemon_root, "delivery-1");
    stop_daemon (first);

    segment_path = g_build_filename (daemon_root.journal_dir,
            JOURNAL_SEGMENT_NAME, NULL);
    g_assert_true (g_file_get_contents (segment_path, &contents,
        &committed_length, &error));
    g_assert_no_error (error);
    g_clear_pointer (&contents, g_free);

    journal_delivery_offline (&daemon_root, "delivery-torn", "account-1");
    g_assert_true (g_file_get_contents (segment_path, &contents, &length,
        &error));
    g_assert_no_error (error);
    g_assert_cmpuint (length, >, committed_length + 1);
    g_assert_true (g_file_set_contents (segment_path, contents,
        (gssize)(length - 1), &error));
    g_assert_no_error (error);

    recovered = start_daemon (&daemon_root);
    assert_inbox_state (&daemon_root, 2, 1);
    deliver (&daemon_root, "delivery-2");
    assert_inbox_state (&daemon_root, 3, 2);
    stop_daemon (recovered);
}
#endif

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);

    g_test_add_func ("/daemon-api/wyreboxd/starts-with-valid-config",
        test_wyreboxd_accepts_config_and_starts_socket);
    g_test_add_func ("/daemon-api/wyreboxd/rejects-invalid-config-path",
        test_wyreboxd_rejects_invalid_config_path);
    g_test_add_func
        ("/daemon-api/wyreboxd/fails-startup-on-delivery-without-account",
        test_wyreboxd_fails_startup_on_delivery_without_account);
    g_test_add_func
        ("/daemon-api/wyreboxd/exits-tempfail-when-catalog-is-locked",
        test_wyreboxd_exits_tempfail_when_catalog_is_locked);
    g_test_add_func (
        "/daemon-api/wyreboxd/exits-dataerr-when-catalog-is-corrupt",
        test_wyreboxd_exits_dataerr_when_catalog_is_corrupt);
    g_test_add_func
        ("/daemon-api/wyreboxd/exits-dataerr-on-newer-catalog-schema",
        test_wyreboxd_exits_dataerr_on_newer_catalog_schema);
    g_test_add_func
    (
        "/daemon-api/wyreboxd/exits-dataerr-when-catalog-migration-needs-checkpoint",
        test_wyreboxd_exits_dataerr_when_catalog_migration_needs_checkpoint);
    g_test_add_func
        ("/daemon-api/wyreboxd/exits-tempfail-when-object-root-is-missing",
        test_wyreboxd_exits_tempfail_when_object_root_is_missing);
    g_test_add_func ("/daemon-api/wyreboxd/starts-with-missing-raw-object",
        test_wyreboxd_starts_with_missing_raw_object);
    g_test_add_func
        ("/daemon-api/wyreboxd/exits-tempfail-on-unreadable-raw-object",
        test_wyreboxd_exits_tempfail_on_unreadable_raw_object);
    g_test_add_func
        ("/daemon-api/wyreboxd/exits-dataerr-on-corrupt-journal-record",
        test_wyreboxd_exits_dataerr_on_corrupt_journal_record);
    g_test_add_func ("/daemon-api/wyreboxd/starts-with-held-account",
        test_wyreboxd_starts_with_held_account);
#if defined(WYREBOX_HAVE_CAPNP_SERIALIZATION) && \
    WYREBOX_HAVE_CAPNP_SERIALIZATION
    g_test_add_func
        ("/daemon-api/wyreboxd/materializes-delivery-before-receipt",
        test_wyreboxd_materializes_delivery_before_receipt);
    g_test_add_func ("/daemon-api/wyreboxd/catches-up-journal-on-startup",
        test_wyreboxd_catches_up_journal_on_startup);
    g_test_add_func
        ("/daemon-api/wyreboxd/rebuilds-identical-catalog-after-restart",
        test_wyreboxd_rebuilds_identical_catalog_after_restart);
    g_test_add_func ("/daemon-api/wyreboxd/recovers-torn-journal-suffix",
        test_wyreboxd_recovers_torn_journal_suffix);
    g_test_add_func
        ("/daemon-api/wyreboxd/acknowledges-delivery-when-materialization-fails",
        test_wyreboxd_acknowledges_delivery_when_materialization_fails);
#endif

    return g_test_run ();
}
