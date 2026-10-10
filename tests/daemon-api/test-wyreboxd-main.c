#include <gio/gio.h>
#include <glib.h>
#include <glib/gstdio.h>

#include "wyrebox-build-config.h"
#include "wyrebox-daemon-audit-payload.h"
#include "wyrebox-daemon-fact-mutation-request.h"
#include "wyrebox-daemon-frame-io.h"
#include "wyrebox-daemon-mailbox-list-result.h"
#include "wyrebox-daemon-mailbox-select-result.h"
#if defined(WYREBOX_HAVE_CAPNP_SERIALIZATION) && \
    WYREBOX_HAVE_CAPNP_SERIALIZATION
#include "wyrebox-daemon-capnp-codec.h"
#endif
#include "wyrebox-daemon-runtime.h"
#include "wyrebox-daemon-storage.h"
#include "wyrebox-dovecot-daemon-client.h"
#include "wyrebox-eml-ingestor.h"
#include "wyrebox-journal-reader.h"
#include "wyrebox-journal-writer.h"
#include "wyrebox-local-object-store.h"

#include <signal.h>
#include <string.h>
#include <sys/stat.h>
#include <duckdb.h>
#include <sysexits.h>

#define JOURNAL_SEGMENT_NAME "00000000000000000000.wbj"
#define OTHER_STORAGE_ID "01920000-0000-7000-8000-0000000000ff"

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
daemon_root_init_uninitialized (DaemonRoot *daemon_root)
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

/*
 * Runs wyreboxd to completion, optionally with --initialize-storage, and
 * returns its exit status.
 */
static int
run_wyreboxd (const DaemonRoot *daemon_root, gboolean initialize_storage,
    char **out_stdout, char **out_stderr)
{
    g_autoptr (GSubprocess) subprocess = NULL;
    g_autoptr (GError) error = NULL;
    g_autofree char *stdout_text = NULL;
    g_autofree char *stderr_text = NULL;
    const char *argv[] = {
        wyreboxd_executable (),
        "--config",
        daemon_root->config_path,
        initialize_storage ? "--initialize-storage" : NULL,
        NULL
    };

    subprocess = g_subprocess_newv (argv,
            (GSubprocessFlags)(G_SUBPROCESS_FLAGS_STDOUT_PIPE |
            G_SUBPROCESS_FLAGS_STDERR_PIPE), &error);
    g_assert_no_error (error);
    g_assert_true (g_subprocess_communicate_utf8 (subprocess, NULL, NULL,
        &stdout_text, &stderr_text, &error));
    g_assert_no_error (error);

    if (out_stdout != NULL)
        *out_stdout = g_steal_pointer (&stdout_text);
    if (out_stderr != NULL)
        *out_stderr = g_steal_pointer (&stderr_text);
    return g_subprocess_get_exit_status (subprocess);
}

static void
initialize_storage (const DaemonRoot *daemon_root)
{
    g_autofree char *stdout_text = NULL;

    g_assert_cmpint (run_wyreboxd (daemon_root, TRUE, &stdout_text, NULL), ==,
        EX_OK);
    g_assert_nonnull (strstr (stdout_text, "storage initialized"));
}

static void
daemon_root_init (DaemonRoot *daemon_root)
{
    daemon_root_init_uninitialized (daemon_root);
    initialize_storage (daemon_root);
}

#define PROJECTS_RULES \
        ".decl has_keyword(message_id: symbol, keyword: symbol)\n" \
        ".decl show_in_virtual_folder(view_id: symbol, message_id: symbol)\n" \
        "show_in_virtual_folder(\"projects\", message_id) :- " \
        "has_keyword(message_id, \"project\").\n"

/*
 * Configures the Projects virtual mailbox with @rules written to the rules
 * file, or with a rules_path that does not exist when @rules is NULL.
 */
static void
configure_projects_view (const DaemonRoot *daemon_root, const char *rules)
{
    g_autofree char *rules_path = g_build_filename (daemon_root->root,
            "views.dl", NULL);
    g_autofree char *config = NULL;
    g_autofree char *extended = NULL;
    g_autoptr (GError) error = NULL;

    if (rules != NULL) {
        g_assert_true (g_file_set_contents (rules_path, rules, -1, &error));
        g_assert_no_error (error);
    }

    g_assert_true (g_file_get_contents (daemon_root->config_path, &config,
        NULL, &error));
    g_assert_no_error (error);
    extended = g_strdup_printf ("%s\n[wirelog]\nrules_path=%s\n\n"
            "[view:projects]\nimap_name=Projects\n", config, rules_path);
    g_assert_true (g_file_set_contents (daemon_root->config_path, extended, -1,
        &error));
    g_assert_no_error (error);
}

/*
 * Adds an extraction rules file with @rules to the [wirelog] section, or a
 * path that does not exist when @rules is NULL.
 */
static void
configure_extraction_rules (const DaemonRoot *daemon_root, const char *rules)
{
    g_autofree char *rules_path = g_build_filename (daemon_root->root,
            "extraction.rules", NULL);
    g_autofree char *config = NULL;
    g_autofree char *extended = NULL;
    g_autoptr (GError) error = NULL;

    if (rules != NULL) {
        g_assert_true (g_file_set_contents (rules_path, rules, -1, &error));
        g_assert_no_error (error);
    }

    g_assert_true (g_file_get_contents (daemon_root->config_path, &config,
        NULL, &error));
    g_assert_no_error (error);
    extended = g_strdup_printf ("%s\n[wirelog]\nextraction_rules_path=%s\n",
            config, rules_path);
    g_assert_true (g_file_set_contents (daemon_root->config_path, extended, -1,
        &error));
    g_assert_no_error (error);
}

static char *
storage_marker_path (const DaemonRoot *daemon_root, gboolean journal)
{
    return journal ? g_build_filename (daemon_root->journal_dir,
               WYREBOX_DAEMON_STORAGE_JOURNAL_MARKER, NULL) :
           g_build_filename (daemon_root->object_dir,
               WYREBOX_DAEMON_STORAGE_OBJECT_STORE_MARKER, NULL);
}

static void
write_storage_marker (const DaemonRoot *daemon_root, gboolean journal,
    const char *contents, gssize length)
{
    g_autofree char *path = storage_marker_path (daemon_root, journal);
    g_autoptr (GError) error = NULL;

    g_assert_true (g_file_set_contents (path, contents, length, &error));
    g_assert_no_error (error);
}

static gboolean
storage_marker_exists (const DaemonRoot *daemon_root, gboolean journal)
{
    g_autofree char *path = storage_marker_path (daemon_root, journal);

    return g_file_test (path, G_FILE_TEST_EXISTS);
}

static gboolean
objects_dir_exists (const DaemonRoot *daemon_root)
{
    g_autofree char *path = g_build_filename (daemon_root->object_dir,
            "objects", "sha256", NULL);

    return g_file_test (path, G_FILE_TEST_IS_DIR);
}

static char *
read_segment (const DaemonRoot *daemon_root, gsize *out_length)
{
    g_autofree char *path = g_build_filename (daemon_root->journal_dir,
            JOURNAL_SEGMENT_NAME, NULL);
    g_autoptr (GError) error = NULL;
    char *contents = NULL;

    g_assert_true (g_file_get_contents (path, &contents, out_length, &error));
    g_assert_no_error (error);
    return contents;
}

static void
write_segment (const DaemonRoot *daemon_root, const char *contents,
    gsize length)
{
    g_autofree char *path = g_build_filename (daemon_root->journal_dir,
            JOURNAL_SEGMENT_NAME, NULL);
    g_autoptr (GError) error = NULL;

    g_assert_true (g_file_set_contents (path, contents, (gssize)length,
        &error));
    g_assert_no_error (error);
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
/*
 * Appends a fact mutation to the journal from the test process while wyreboxd
 * is not running.
 */
static void
journal_fact_offline (const DaemonRoot *daemon_root,
    WyreboxDaemonFactMutationKind kind, const char *account_id,
    const char *const *arguments)
{
    g_autoptr (WyreboxJournalWriter) writer = NULL;
    g_auto (WyreboxDaemonFactMutationRequest) request = { 0 };
    g_autoptr (GError) error = NULL;
    guint64 offset = 0;
    guint64 sequence = 0;

    writer = wyrebox_journal_writer_new (daemon_root->journal_dir, &error);
    g_assert_no_error (error);
    g_assert_true (wyrebox_daemon_fact_mutation_request_init (&request, kind,
        "project_keyword", account_id, arguments, &error));
    g_assert_no_error (error);
    g_assert_true (wyrebox_daemon_fact_mutation_request_append_journal
            (&request, writer, &offset, &sequence, &error));
    g_assert_no_error (error);
}

static GBytes *
build_delivery_request_for_message (const char *delivery_id, GBytes *message)
{
    g_autoptr (GError) error = NULL;
    g_autoptr (GBytes) encoded = NULL;
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

static GBytes *
build_delivery_request (const char *delivery_id)
{
    g_autoptr (GBytes) message = build_message (delivery_id);

    return build_delivery_request_for_message (delivery_id, message);
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

/*
 * Fetches a UID from account-1's INBOX as Dovecot acting for
 * @envelope_account, naming @request_account in the fetch request.
 */
static void
fetch_inbox_uid (const DaemonRoot *daemon_root, const char *envelope_account,
    const char *request_account, guint64 uid_validity, guint64 uid,
    WyreboxDaemonResponseFrame *out_frame)
{
    g_autoptr (GError) error = NULL;
    g_autoptr (GBytes) encoded = NULL;
    g_autoptr (GBytes) response = NULL;
    g_auto (WyreboxDaemonRequestIdentity) identity = { 0 };
    g_auto (WyreboxDaemonMessageFetchRequest) request = { 0 };

    g_assert_true (wyrebox_daemon_request_identity_init (&identity,
        "request-fetch-1", "dovecot", envelope_account, "dovecot-storage",
        "corr-fetch-1", &error));
    g_assert_no_error (error);
    g_assert_true (wyrebox_daemon_message_fetch_request_init (&request,
        request_account, "inbox:account-1",
        WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_ORDINARY, uid_validity, uid,
        &error));
    g_assert_no_error (error);

    encoded = wyrebox_daemon_capnp_codec_encode_message_fetch_request
            (&identity, &request, NULL, &error);
    g_assert_no_error (error);
    response = roundtrip_request (daemon_root->socket_path, encoded);
    g_assert_true (wyrebox_daemon_capnp_codec_decode_response_frame (response,
        out_frame, &error));
    g_assert_no_error (error);
    g_assert_cmpstr (out_frame->request_id, ==, "request-fetch-1");
}

static void
assert_fetches_message (const DaemonRoot *daemon_root, guint64 uid,
    const char *delivery_id)
{
    g_auto (WyreboxDaemonResponseFrame) frame = { 0 };
    g_autoptr (GBytes) expected = build_message (delivery_id);

    fetch_inbox_uid (daemon_root, "account-1", "account-1", 1, uid, &frame);
    g_assert_cmpint (frame.kind, ==,
        WYREBOX_DAEMON_RESPONSE_FRAME_STREAM_CHUNK);
    g_assert_true (frame.stream_chunk.end_of_stream);
    g_assert_true (g_bytes_equal (frame.stream_chunk.bytes, expected));
}

/*
 * Searches account-1's INBOX for messages whose subject contains @subject,
 * or for every message when @subject is NULL.
 */
static void
search_inbox (const DaemonRoot *daemon_root, guint64 uid_validity,
    const char *subject, WyreboxDaemonResponseFrame *out_frame)
{
    g_autoptr (GError) error = NULL;
    g_autoptr (GBytes) encoded = NULL;
    g_autoptr (GBytes) response = NULL;
    g_auto (WyreboxDaemonRequestIdentity) identity = { 0 };
    g_auto (WyreboxDaemonMessageSearchRequest) request = { 0 };
    WyreboxDaemonMessageSearchCriterion criterion = {
        WYREBOX_DAEMON_MESSAGE_SEARCH_CRITERION_SUBJECT_CONTAINS,
        (char *)subject, 0
    };

    g_assert_true (wyrebox_daemon_request_identity_init (&identity,
        "request-search-1", "dovecot", "account-1", "dovecot-storage",
        "corr-search-1", &error));
    g_assert_no_error (error);
    g_assert_true (wyrebox_daemon_message_search_request_init (&request,
        "account-1", "inbox:account-1",
        WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_ORDINARY, uid_validity,
        subject != NULL ? &criterion : NULL, subject != NULL ? 1 : 0,
        &error));
    g_assert_no_error (error);

    encoded = wyrebox_daemon_capnp_codec_encode_message_search_request
            (&identity, &request, NULL, &error);
    g_assert_no_error (error);
    response = roundtrip_request (daemon_root->socket_path, encoded);
    g_assert_true (wyrebox_daemon_capnp_codec_decode_response_frame (response,
        out_frame, &error));
    g_assert_no_error (error);
    g_assert_cmpstr (out_frame->request_id, ==, "request-search-1");
}

static void
assert_search_finds (const DaemonRoot *daemon_root, const char *subject,
    const char *expected_uids)
{
    g_auto (WyreboxDaemonResponseFrame) frame = { 0 };
    gsize size = 0;
    const char *data = NULL;

    search_inbox (daemon_root, 1, subject, &frame);
    g_assert_cmpint (frame.kind, ==,
        WYREBOX_DAEMON_RESPONSE_FRAME_STREAM_CHUNK);
    g_assert_true (frame.stream_chunk.end_of_stream);
    data = g_bytes_get_data (frame.stream_chunk.bytes, &size);
    g_assert_cmpmem (data, size, expected_uids, strlen (expected_uids));
}

static void
update_inbox_flags (const DaemonRoot *daemon_root, guint64 uid_validity,
    guint64 uid, WyreboxDaemonFlagKeywordUpdateMode mode,
    const char *const *system_flags, const char *const *user_keywords,
    WyreboxDaemonResponseFrame *out_frame)
{
    g_autoptr (GError) error = NULL;
    g_autoptr (GBytes) encoded = NULL;
    g_autoptr (GBytes) response = NULL;
    g_auto (WyreboxDaemonRequestIdentity) identity = { 0 };
    g_auto (WyreboxDaemonFlagKeywordUpdateRequest) request = { 0 };

    g_assert_true (wyrebox_daemon_request_identity_init (&identity,
        "request-flags-1", "dovecot", "account-1", "dovecot-storage",
        "corr-flags-1", &error));
    g_assert_no_error (error);
    g_assert_true (wyrebox_daemon_flag_keyword_update_request_init (&request,
        "account-1", "inbox:account-1", uid_validity, uid, mode,
        system_flags, user_keywords, &error));
    g_assert_no_error (error);

    encoded = wyrebox_daemon_capnp_codec_encode_flag_keyword_update_request
            (&identity, &request, NULL, &error);
    g_assert_no_error (error);
    response = roundtrip_request (daemon_root->socket_path, encoded);
    g_assert_true (wyrebox_daemon_capnp_codec_decode_response_frame (response,
        out_frame, &error));
    g_assert_no_error (error);
    g_assert_cmpstr (out_frame->request_id, ==, "request-flags-1");
}

/*
 * Asserts the flags column of account-1's INBOX UID map, one
 * space-separated entry per UID in @expected_flags.
 */
static void
assert_inbox_flags (const DaemonRoot *daemon_root,
    const char *const *expected_flags)
{
    g_auto (WyreboxDovecotMailboxUidMapSnapshot) snapshot = { 0 };
    g_autoptr (GError) error = NULL;

    g_assert_true (wyrebox_dovecot_daemon_client_load_uid_map
            (daemon_root->socket_path, "account-1", "inbox:account-1",
        WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_ORDINARY, 1, &snapshot, &error));
    g_assert_no_error (error);
    g_assert_cmpuint (snapshot.rows->len, ==, g_strv_length ((char **)
        expected_flags));
    for (guint i = 0; i < snapshot.rows->len; i++) {
        const WyreboxDovecotMailboxUidMapRow *row =
            g_ptr_array_index (snapshot.rows, i);
        g_autofree char *joined = g_strjoinv (" ", row->flags);

        g_assert_cmpuint (row->uid, ==, i + 1);
        g_assert_cmpstr (joined, ==, expected_flags[i]);
    }
}

/*
 * Runs @template_id over the socket with one parameter, scoped to account-1.
 */
static void
query_template (const DaemonRoot *daemon_root, const char *caller_identity,
    const char *template_id, const char *parameter,
    WyreboxDaemonResponseFrame *out_frame)
{
    const char *parameters[] = { parameter, NULL };
    g_autoptr (GError) error = NULL;
    g_autoptr (GBytes) encoded = NULL;
    g_autoptr (GBytes) response = NULL;
    g_auto (WyreboxDaemonRequestIdentity) identity = { 0 };
    g_auto (WyreboxDaemonDuckDBQueryTemplateRequest) request = { 0 };

    g_assert_true (wyrebox_daemon_request_identity_init (&identity,
        "request-template-1", caller_identity, "account-1", "template-tool",
        "corr-template-1", &error));
    g_assert_no_error (error);
    g_assert_true (wyrebox_daemon_duckdb_query_template_request_init
            (&request, "query-1", template_id, "account-1", parameters,
        &error));
    g_assert_no_error (error);

    encoded = wyrebox_daemon_capnp_codec_encode_duckdb_query_template_request
            (&identity, &request, NULL, &error);
    g_assert_no_error (error);
    response = roundtrip_request (daemon_root->socket_path, encoded);
    g_assert_true (wyrebox_daemon_capnp_codec_decode_response_frame (response,
        out_frame, &error));
    g_assert_no_error (error);
    g_assert_cmpstr (out_frame->request_id, ==, "request-template-1");
}

/*
 * Counts DuckDB query-template audit records in the journal with
 * @outcome and @template_id.
 */
static guint
count_template_audit_records (const DaemonRoot *daemon_root,
    WyreboxDaemonAuditOutcome outcome, const char *template_id)
{
    g_autoptr (WyreboxJournalReader) reader = NULL;
    g_autoptr (GError) error = NULL;
    gboolean eof = FALSE;
    guint count = 0;

    reader = wyrebox_journal_reader_new (daemon_root->journal_dir, &error);
    g_assert_no_error (error);

    while (TRUE) {
        g_auto (WyreboxJournalRecord) record = { 0 };
        g_auto (WyreboxDaemonAuditPayload) payload = { 0 };

        if (!wyrebox_journal_reader_read_next (reader, &record, &eof,
            &error)) {
            g_assert_no_error (error);
            g_assert_true (eof);
            break;
        }
        if (record.event_type != WYREBOX_JOURNAL_EVENT_DAEMON_AUDIT_RECORDED)
            continue;

        g_assert_true (wyrebox_daemon_audit_payload_decode (record.payload,
            &payload, &error));
        g_assert_no_error (error);
        if (payload.operation ==
            WYREBOX_DAEMON_AUDIT_OPERATION_DUCKDB_QUERY_TEMPLATE &&
            payload.outcome == outcome &&
            g_strcmp0 (payload.template_id, template_id) == 0)
            count++;
    }

    return count;
}

/*
 * Reads @account_id's mail events after the given cursor as
 * @caller_identity acting for the same account.
 */
static void
read_mail_events (const DaemonRoot *daemon_root, const char *caller_identity,
    const char *account_id, guint64 after_offset, guint64 after_sequence,
    WyreboxDaemonResponseFrame *out_frame)
{
    g_autoptr (GError) error = NULL;
    g_autoptr (GBytes) encoded = NULL;
    g_autoptr (GBytes) response = NULL;
    g_auto (WyreboxDaemonRequestIdentity) identity = { 0 };
    g_auto (WyreboxDaemonMailEventStreamRequest) request = { 0 };

    g_assert_true (wyrebox_daemon_request_identity_init (&identity,
        "request-events-1", caller_identity, account_id, "event-tool",
        "corr-events-1", &error));
    g_assert_no_error (error);
    g_assert_true (wyrebox_daemon_mail_event_stream_request_init (&request,
        account_id, NULL, NULL, NULL, after_offset, after_sequence, 0, 0,
        &error));
    g_assert_no_error (error);

    encoded = wyrebox_daemon_capnp_codec_encode_mail_event_stream_request
            (&identity, &request, NULL, &error);
    g_assert_no_error (error);
    response = roundtrip_request (daemon_root->socket_path, encoded);
    g_assert_true (wyrebox_daemon_capnp_codec_decode_response_frame (response,
        out_frame, &error));
    g_assert_no_error (error);
    g_assert_cmpstr (out_frame->request_id, ==, "request-events-1");
}

static void
assert_mail_events_end (const DaemonRoot *daemon_root, const char *account_id,
    guint64 after_offset, guint64 after_sequence)
{
    g_auto (WyreboxDaemonResponseFrame) frame = { 0 };

    read_mail_events (daemon_root, "admin-cli", account_id, after_offset,
        after_sequence, &frame);
    g_assert_cmpint (frame.kind, ==,
        WYREBOX_DAEMON_RESPONSE_FRAME_STREAM_CHUNK);
    g_assert_true (frame.stream_chunk.end_of_stream);
    g_assert_cmpuint (g_bytes_get_size (frame.stream_chunk.bytes), ==, 0);
}

/*
 * Delivers a message to account-1 and returns its message id.
 */
static char *
deliver_message (const DaemonRoot *daemon_root, const char *delivery_id)
{
    g_autoptr (GBytes) request = build_delivery_request (delivery_id);
    g_autoptr (GBytes) response = roundtrip_request (daemon_root->socket_path,
            request);
    g_auto (WyreboxDaemonResponseFrame) frame = { 0 };
    g_autoptr (GError) error = NULL;

    g_assert_true (wyrebox_daemon_capnp_codec_decode_response_frame (response,
        &frame, &error));
    g_assert_no_error (error);
    g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_SUCCESS);

    return g_strdup_printf ("journal:%" G_GUINT64_FORMAT ":%" G_GUINT64_FORMAT,
               frame.success.journal_offset, frame.success.journal_sequence);
}

/*
 * Delivers @headers followed by a short body to account-1 and returns the
 * journal message id.
 */
static char *
deliver_headers (const DaemonRoot *daemon_root, const char *delivery_id,
    const char *headers)
{
    g_autofree char *text = g_strdup_printf ("%s\r\nbody\r\n", headers);
    gsize length = strlen (text);
    g_autoptr (GBytes) message = g_bytes_new_take (g_steal_pointer (&text),
            length);
    g_autoptr (GBytes) request = build_delivery_request_for_message
            (delivery_id, message);
    g_autoptr (GBytes) response = roundtrip_request (daemon_root->socket_path,
            request);
    g_auto (WyreboxDaemonResponseFrame) frame = { 0 };
    g_autoptr (GError) error = NULL;

    g_assert_true (wyrebox_daemon_capnp_codec_decode_response_frame (response,
        &frame, &error));
    g_assert_no_error (error);
    g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_SUCCESS);

    return g_strdup_printf ("journal:%" G_GUINT64_FORMAT ":%" G_GUINT64_FORMAT,
               frame.success.journal_offset, frame.success.journal_sequence);
}

/*
 * Inserts or retracts has_keyword(@message_id, @keyword) for account-1 over
 * the socket as a trusted tool.
 */
static void
mutate_keyword_fact (const DaemonRoot *daemon_root,
    WyreboxDaemonFactMutationKind kind, const char *message_id,
    const char *keyword)
{
    const char *arguments[] = { message_id, keyword, NULL };
    g_autoptr (GError) error = NULL;
    g_autoptr (GBytes) encoded = NULL;
    g_autoptr (GBytes) response = NULL;
    g_auto (WyreboxDaemonRequestIdentity) identity = { 0 };
    g_auto (WyreboxDaemonFactMutationRequest) request = { 0 };
    g_auto (WyreboxDaemonResponseFrame) frame = { 0 };

    g_assert_true (wyrebox_daemon_request_identity_init (&identity,
        "request-fact-1", "trusted-tool", "account-1", "fact-importer",
        "corr-fact-1", &error));
    g_assert_no_error (error);
    g_assert_true (wyrebox_daemon_fact_mutation_request_init (&request, kind,
        "has_keyword", "account-1", arguments, &error));
    g_assert_no_error (error);

    encoded = wyrebox_daemon_capnp_codec_encode_fact_mutation_request
            (&identity, &request, NULL, &error);
    g_assert_no_error (error);
    response = roundtrip_request (daemon_root->socket_path, encoded);
    g_assert_true (wyrebox_daemon_capnp_codec_decode_response_frame (response,
        &frame, &error));
    g_assert_no_error (error);
    g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_SUCCESS);
}

static void
query_predicate (const DaemonRoot *daemon_root, const char *predicate_id,
    WyreboxDaemonResponseFrame *out_frame)
{
    const char *bindings[] = { NULL };
    g_autoptr (GError) error = NULL;
    g_autoptr (GBytes) encoded = NULL;
    g_autoptr (GBytes) response = NULL;
    g_auto (WyreboxDaemonRequestIdentity) identity = { 0 };
    g_auto (WyreboxDaemonWirelogPredicateQueryRequest) request = { 0 };

    g_assert_true (wyrebox_daemon_request_identity_init (&identity,
        "request-predicate-1", "admin-cli", "account-1", "wyrebox-admin",
        "corr-predicate-1", &error));
    g_assert_no_error (error);
    g_assert_true (wyrebox_daemon_wirelog_predicate_query_request_init
            (&request, "query-1", predicate_id, "account-1", bindings,
        &error));
    g_assert_no_error (error);

    encoded =
        wyrebox_daemon_capnp_codec_encode_wirelog_predicate_query_request
            (&identity, &request, NULL, &error);
    g_assert_no_error (error);
    response = roundtrip_request (daemon_root->socket_path, encoded);
    g_assert_true (wyrebox_daemon_capnp_codec_decode_response_frame (response,
        out_frame, &error));
    g_assert_no_error (error);
}

static void
assert_projects_state (const DaemonRoot *daemon_root, guint32 uid_validity,
    guint32 uid_next, guint32 message_count)
{
    g_auto (WyreboxDaemonMailboxSelectResult) result = { 0 };
    g_autoptr (GError) error = NULL;

    g_assert_true (wyrebox_dovecot_daemon_client_select_mailbox
            (daemon_root->socket_path, "account-1", "Projects", &result,
        &error));
    g_assert_no_error (error);
    g_assert_cmpint (result.kind, ==,
        WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_VIRTUAL);
    g_assert_cmpstr (result.mailbox_id, ==, "projects");
    if (uid_validity != 0)
        g_assert_cmpuint (result.uid_validity, ==, uid_validity);
    g_assert_cmpuint (result.uid_next, ==, uid_next);
    g_assert_cmpuint (result.message_count, ==, message_count);
}

static guint32
projects_uid_validity (const DaemonRoot *daemon_root)
{
    g_auto (WyreboxDaemonMailboxSelectResult) result = { 0 };
    g_autoptr (GError) error = NULL;

    g_assert_true (wyrebox_dovecot_daemon_client_select_mailbox
            (daemon_root->socket_path, "account-1", "Projects", &result,
        &error));
    g_assert_no_error (error);
    g_assert_cmpuint (result.uid_validity, !=, 0);

    return result.uid_validity;
}

static void
assert_fetch_fails (const DaemonRoot *daemon_root,
    const char *envelope_account, const char *request_account,
    guint64 uid_validity, guint64 uid, WyreboxDaemonErrorClass error_class)
{
    g_auto (WyreboxDaemonResponseFrame) frame = { 0 };

    fetch_inbox_uid (daemon_root, envelope_account, request_account,
        uid_validity, uid, &frame);
    g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_ERROR);
    g_assert_cmpint (frame.error.error_class, ==, error_class);
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

static void
test_wyreboxd_exits_tempfail_on_uninitialized_storage (void)
{
    g_auto (DaemonRoot) daemon_root = { 0 };
    g_autofree char *segment = NULL;

    daemon_root_init_uninitialized (&daemon_root);
    segment = g_build_filename (daemon_root.journal_dir, JOURNAL_SEGMENT_NAME,
            NULL);

    assert_daemon_startup_fails (&daemon_root, EX_TEMPFAIL,
        "storage is not initialized");
    g_assert_false (objects_dir_exists (&daemon_root));
    g_assert_false (g_file_test (segment, G_FILE_TEST_EXISTS));
    g_assert_false (g_file_test (daemon_root.catalog_path, G_FILE_TEST_EXISTS));
    g_assert_false (storage_marker_exists (&daemon_root, TRUE));
    g_assert_false (storage_marker_exists (&daemon_root, FALSE));
}

static void
test_wyreboxd_exits_tempfail_when_storage_roots_are_missing (void)
{
    g_auto (DaemonRoot) daemon_root = { 0 };

    daemon_root_init_uninitialized (&daemon_root);
    remove_tree (daemon_root.journal_dir);
    remove_tree (daemon_root.object_dir);

    assert_daemon_startup_fails (&daemon_root, EX_TEMPFAIL,
        "storage is not initialized");
    g_assert_false (g_file_test (daemon_root.journal_dir, G_FILE_TEST_EXISTS));
    g_assert_false (g_file_test (daemon_root.object_dir, G_FILE_TEST_EXISTS));
}

/*
 * A torn first record must not be truncated while the object store volume is
 * not mounted, because recovery would discard the only trace of the delivery.
 */
static void
test_wyreboxd_exits_tempfail_on_torn_first_record_with_unmounted_object_store
    (void)
{
    g_auto (DaemonRoot) daemon_root = { 0 };
    g_autofree char *before = NULL;
    g_autofree char *after = NULL;
    gsize before_length = 0;
    gsize after_length = 0;

    daemon_root_init (&daemon_root);
    journal_delivery_offline (&daemon_root, "delivery-1", "account-1");
    before = read_segment (&daemon_root, &before_length);
    g_assert_cmpuint (before_length, >, 1);
    write_segment (&daemon_root, before, before_length - 1);
    g_clear_pointer (&before, g_free);
    before = read_segment (&daemon_root, &before_length);
    remove_tree (daemon_root.object_dir);
    g_assert_cmpint (g_mkdir (daemon_root.object_dir, 0750), ==, 0);

    assert_daemon_startup_fails (&daemon_root, EX_TEMPFAIL,
        "check that the volume holding");
    after = read_segment (&daemon_root, &after_length);
    g_assert_cmpmem (after, after_length, before, before_length);
    g_assert_false (objects_dir_exists (&daemon_root));
}

static void
test_wyreboxd_exits_dataerr_on_mismatched_storage_ids (void)
{
    g_auto (DaemonRoot) daemon_root = { 0 };

    daemon_root_init (&daemon_root);
    write_storage_marker (&daemon_root, FALSE, "[WyreBox Storage]\n"
        "format=1\nrole=object-store\nstorage_id=" OTHER_STORAGE_ID "\n", -1);

    assert_daemon_startup_fails (&daemon_root, EX_DATAERR,
        "storage markers do not match");
}

static void
test_wyreboxd_exits_dataerr_on_oversized_storage_marker (void)
{
    g_auto (DaemonRoot) daemon_root = { 0 };
    g_autofree char *padding = g_strnfill (8192, '#');

    daemon_root_init (&daemon_root);
    write_storage_marker (&daemon_root, TRUE, padding, -1);

    assert_daemon_startup_fails (&daemon_root, EX_DATAERR, "storage marker");
}

static void
test_wyreboxd_exits_tempfail_on_unreadable_storage_marker (void)
{
    g_auto (DaemonRoot) daemon_root = { 0 };
    g_autofree char *path = NULL;

    daemon_root_init (&daemon_root);
    path = storage_marker_path (&daemon_root, TRUE);
    g_assert_cmpint (g_remove (path), ==, 0);
    g_assert_cmpint (g_mkdir (path, 0700), ==, 0);

    assert_daemon_startup_fails (&daemon_root, EX_TEMPFAIL,
        "failed to read storage marker");
}

static void
test_wyreboxd_initialize_storage_then_starts (void)
{
    g_auto (DaemonRoot) daemon_root = { 0 };
    g_autoptr (GSubprocess) subprocess = NULL;
    g_autofree char *stdout_text = NULL;

    daemon_root_init_uninitialized (&daemon_root);
    remove_tree (daemon_root.journal_dir);
    remove_tree (daemon_root.object_dir);
    g_assert_cmpint (run_wyreboxd (&daemon_root, TRUE, &stdout_text, NULL), ==,
        EX_OK);
    g_assert_nonnull (strstr (stdout_text,
        "wyreboxd: storage initialized: storage ID "));
    g_assert_true (storage_marker_exists (&daemon_root, TRUE));
    g_assert_true (storage_marker_exists (&daemon_root, FALSE));
    g_assert_true (objects_dir_exists (&daemon_root));
    g_assert_false (g_file_test (daemon_root.catalog_path, G_FILE_TEST_EXISTS));
    g_assert_false (g_file_test (daemon_root.socket_path, G_FILE_TEST_EXISTS));

    g_clear_pointer (&stdout_text, g_free);
    g_assert_cmpint (run_wyreboxd (&daemon_root, TRUE, &stdout_text, NULL), ==,
        EX_OK);
    g_assert_nonnull (strstr (stdout_text,
        "wyreboxd: storage already initialized: storage ID "));

    subprocess = start_daemon (&daemon_root);
    stop_daemon (subprocess);
}

static void
test_wyreboxd_initialize_storage_adopts_existing_storage (void)
{
    g_auto (DaemonRoot) daemon_root = { 0 };
    g_autoptr (GSubprocess) subprocess = NULL;

    daemon_root_init_uninitialized (&daemon_root);
    journal_delivery_offline (&daemon_root, "delivery-1", "account-1");
    initialize_storage (&daemon_root);

    subprocess = start_daemon (&daemon_root);
    stop_daemon (subprocess);
}

static void
test_wyreboxd_initialize_storage_refuses_half_mounted_storage (void)
{
    g_auto (DaemonRoot) daemon_root = { 0 };
    g_autofree char *stderr_text = NULL;

    daemon_root_init_uninitialized (&daemon_root);
    journal_delivery_offline (&daemon_root, "delivery-1", "account-1");
    remove_tree (daemon_root.object_dir);
    g_assert_cmpint (g_mkdir (daemon_root.object_dir, 0750), ==, 0);

    g_assert_cmpint (run_wyreboxd (&daemon_root, TRUE, NULL, &stderr_text), ==,
        EX_TEMPFAIL);
    g_assert_nonnull (strstr (stderr_text,
        "check that the object store is mounted"));
    g_assert_false (storage_marker_exists (&daemon_root, TRUE));
    g_assert_false (storage_marker_exists (&daemon_root, FALSE));
    g_assert_false (objects_dir_exists (&daemon_root));
}

static void
test_wyreboxd_initialize_storage_refuses_mismatched_markers (void)
{
    g_auto (DaemonRoot) daemon_root = { 0 };
    g_autofree char *stderr_text = NULL;

    daemon_root_init (&daemon_root);
    write_storage_marker (&daemon_root, FALSE, "[WyreBox Storage]\n"
        "format=1\nrole=object-store\nstorage_id=" OTHER_STORAGE_ID "\n", -1);

    g_assert_cmpint (run_wyreboxd (&daemon_root, TRUE, NULL, &stderr_text), ==,
        EX_DATAERR);
    g_assert_nonnull (strstr (stderr_text, "storage markers do not match"));
}

static void
test_wyreboxd_initialize_storage_rejects_invalid_config (void)
{
    g_auto (DaemonRoot) daemon_root = { 0 };
    g_autoptr (GError) error = NULL;

    daemon_root_init_uninitialized (&daemon_root);
    g_assert_true (g_file_set_contents (daemon_root.config_path,
        "[daemon]\nunknown_key=1\n", -1, &error));
    g_assert_no_error (error);

    g_assert_cmpint (run_wyreboxd (&daemon_root, TRUE, NULL, NULL), ==,
        EX_CONFIG);
    g_assert_false (storage_marker_exists (&daemon_root, TRUE));
    g_assert_false (storage_marker_exists (&daemon_root, FALSE));
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
test_wyreboxd_fetches_message_bytes (void)
{
    g_auto (DaemonRoot) daemon_root = { 0 };
    g_autoptr (GSubprocess) subprocess = NULL;

    daemon_root_init (&daemon_root);
    journal_delivery_offline (&daemon_root, "delivery-1", "account-1");
    subprocess = start_daemon (&daemon_root);
    deliver (&daemon_root, "delivery-2");

    assert_fetches_message (&daemon_root, 1, "delivery-1");
    assert_fetches_message (&daemon_root, 2, "delivery-2");
    assert_fetch_fails (&daemon_root, "account-1", "account-1", 1, 99,
        WYREBOX_DAEMON_ERROR_NOT_FOUND);
    assert_fetch_fails (&daemon_root, "account-1", "account-1", 2, 1,
        WYREBOX_DAEMON_ERROR_CONFLICT);
    assert_fetch_fails (&daemon_root, "account-2", "account-2", 1, 1,
        WYREBOX_DAEMON_ERROR_NOT_FOUND);
    assert_fetch_fails (&daemon_root, "account-2", "account-1", 1, 1,
        WYREBOX_DAEMON_ERROR_PERMISSION_DENIED);

    stop_daemon (subprocess);
}

static void
test_wyreboxd_searches_messages (void)
{
    g_auto (DaemonRoot) daemon_root = { 0 };
    g_autoptr (GSubprocess) subprocess = NULL;
    g_auto (WyreboxDaemonResponseFrame) stale = { 0 };

    daemon_root_init (&daemon_root);
    journal_delivery_offline (&daemon_root, "delivery-1", "account-1");
    subprocess = start_daemon (&daemon_root);
    deliver (&daemon_root, "delivery-2");

    assert_search_finds (&daemon_root, NULL, "1\n2\n");
    assert_search_finds (&daemon_root, "DELIVERY-2", "2\n");
    assert_search_finds (&daemon_root, "delivery-3", "");

    search_inbox (&daemon_root, 2, NULL, &stale);
    g_assert_cmpint (stale.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_ERROR);
    g_assert_cmpint (stale.error.error_class, ==,
        WYREBOX_DAEMON_ERROR_CONFLICT);

    stop_daemon (subprocess);
}

static void
test_wyreboxd_queries_templates (void)
{
    g_auto (DaemonRoot) daemon_root = { 0 };
    g_autoptr (GSubprocess) subprocess = NULL;
    const char *const untouched[] = { "", NULL };

    daemon_root_init (&daemon_root);
    subprocess = start_daemon (&daemon_root);
    deliver (&daemon_root, "delivery-1");

    assert_inbox_flags (&daemon_root, untouched);
    g_assert_cmpuint (count_template_audit_records (&daemon_root,
        WYREBOX_DAEMON_AUDIT_OUTCOME_SUCCESS, "mailbox.uid_map.v1"), ==, 1);

    {
        g_auto (WyreboxDaemonResponseFrame) frame = { 0 };

        query_template (&daemon_root, "dovecot", "unknown.template.v1",
            "inbox:account-1", &frame);
        g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_ERROR);
        g_assert_cmpint (frame.error.error_class, ==,
            WYREBOX_DAEMON_ERROR_NOT_FOUND);
    }
    {
        g_auto (WyreboxDaemonResponseFrame) frame = { 0 };

        query_template (&daemon_root, "postfix-helper", "mailbox.uid_map.v1",
            "inbox:account-1", &frame);
        g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_ERROR);
        g_assert_cmpint (frame.error.error_class, ==,
            WYREBOX_DAEMON_ERROR_PERMISSION_DENIED);
    }
    g_assert_cmpuint (count_template_audit_records (&daemon_root,
        WYREBOX_DAEMON_AUDIT_OUTCOME_FAILURE, "unknown.template.v1"), ==, 1);
    g_assert_cmpuint (count_template_audit_records (&daemon_root,
        WYREBOX_DAEMON_AUDIT_OUTCOME_FAILURE, "mailbox.uid_map.v1"), ==, 1);

    stop_daemon (subprocess);
    g_clear_object (&subprocess);

    subprocess = start_daemon (&daemon_root);
    assert_inbox_flags (&daemon_root, untouched);
    stop_daemon (subprocess);
}

static void
test_wyreboxd_streams_mail_events (void)
{
    g_auto (DaemonRoot) daemon_root = { 0 };
    g_autoptr (GSubprocess) subprocess = NULL;
    g_autoptr (GBytes) request = NULL;
    g_autoptr (GBytes) response = NULL;
    g_auto (WyreboxDaemonResponseFrame) receipt = { 0 };
    g_autoptr (GError) error = NULL;
    g_autofree char *expected_offset = NULL;
    const char *data = NULL;
    gsize size = 0;

    daemon_root_init (&daemon_root);
    subprocess = start_daemon (&daemon_root);
    assert_mail_events_end (&daemon_root, "account-1", 0, 0);

    request = build_delivery_request ("delivery-1");
    response = roundtrip_request (daemon_root.socket_path, request);
    g_assert_true (wyrebox_daemon_capnp_codec_decode_response_frame (response,
        &receipt, &error));
    g_assert_no_error (error);
    g_assert_cmpint (receipt.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_SUCCESS);
    expected_offset = g_strdup_printf ("\noffset=%" G_GUINT64_FORMAT "\n",
            receipt.success.journal_offset);

    {
        g_auto (WyreboxDaemonResponseFrame) frame = { 0 };

        read_mail_events (&daemon_root, "admin-cli", "account-1", 0, 0,
            &frame);
        g_assert_cmpint (frame.kind, ==,
            WYREBOX_DAEMON_RESPONSE_FRAME_STREAM_CHUNK);
        g_assert_false (frame.stream_chunk.end_of_stream);
        g_assert_cmpuint (frame.stream_chunk.chunk_index, ==,
            receipt.success.journal_offset);
        data = g_bytes_get_data (frame.stream_chunk.bytes, &size);
        g_assert_true (g_strstr_len (data, size,
            "wyrebox-mail-event/1\n") == data);
        g_assert_nonnull (g_strstr_len (data, size, expected_offset));
        g_assert_nonnull (g_strstr_len (data, size,
            "\nevent_type=MessageDelivered\n"));
        g_assert_nonnull (g_strstr_len (data, size,
            "\ndelivery_id=delivery-1\n"));
        g_assert_null (g_strstr_len (data, size, "recipient@example.com"));
    }

    assert_mail_events_end (&daemon_root, "account-1",
        receipt.success.journal_offset, receipt.success.journal_sequence);
    assert_mail_events_end (&daemon_root, "account-2", 0, 0);

    {
        g_auto (WyreboxDaemonResponseFrame) frame = { 0 };

        read_mail_events (&daemon_root, "postfix-helper", "account-1", 0, 0,
            &frame);
        g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_ERROR);
        g_assert_cmpint (frame.error.error_class, ==,
            WYREBOX_DAEMON_ERROR_PERMISSION_DENIED);
    }

    stop_daemon (subprocess);
}

static void
test_wyreboxd_materializes_journaled_facts (void)
{
    g_auto (DaemonRoot) daemon_root = { 0 };
    g_autoptr (GSubprocess) subprocess = NULL;
    g_autofree char *catalog_wal_path = NULL;
    g_autofree char *before = NULL;
    g_autofree char *after_rebuild = NULL;
    const char *const projects[] = { "journal:0:1", "view-projects", NULL };
    const char *const ops[] = { "journal:0:1", "view-ops", NULL };
    const char *facts_sql =
        "SELECT string_agg(account_id || ' ' || message_id || ' ' || "
        "(object_id <> '') || ' ' || args_json || ' ' || "
        "retracted_at_unix_us || '@' || journal_offset || ':' || "
        "journal_sequence, '; ' ORDER BY fact_id) FROM message_facts;";

    daemon_root_init (&daemon_root);
    catalog_wal_path = g_strconcat (daemon_root.catalog_path, ".wal", NULL);
    journal_delivery_offline (&daemon_root, "delivery-1", "account-1");
    journal_fact_offline (&daemon_root, WYREBOX_DAEMON_FACT_MUTATION_INSERT,
        "account-1", projects);
    journal_fact_offline (&daemon_root, WYREBOX_DAEMON_FACT_MUTATION_INSERT,
        "account-1", ops);
    journal_fact_offline (&daemon_root, WYREBOX_DAEMON_FACT_MUTATION_RETRACT,
        "account-1", ops);

    subprocess = start_daemon (&daemon_root);
    {
        g_auto (WyreboxDaemonResponseFrame) frame = { 0 };
        const char *data = NULL;
        gsize size = 0;

        query_template (&daemon_root, "admin-cli",
            "message.facts_by_message_id.v1", "journal:0:1", &frame);
        g_assert_cmpint (frame.kind, ==,
            WYREBOX_DAEMON_RESPONSE_FRAME_STREAM_CHUNK);
        data = g_bytes_get_data (frame.stream_chunk.bytes, &size);
        g_assert_nonnull (g_strstr_len (data, size, "view-projects"));
        g_assert_nonnull (g_strstr_len (data, size, "view-ops"));
        g_assert_nonnull (g_strstr_len (data, size,
            "fact-mutation:account-1"));
    }
    stop_daemon (subprocess);
    g_clear_object (&subprocess);

    before = query_catalog_string (daemon_root.catalog_path, facts_sql);
    g_assert_nonnull (strstr (before, "account-1 journal:0:1 true "));
    g_assert_nonnull (strstr (before, "view-ops\"] 4@"));
    g_assert_nonnull (strstr (before, "view-projects\"] 0@"));

    g_assert_cmpint (g_remove (daemon_root.catalog_path), ==, 0);
    (void)g_remove (catalog_wal_path);
    subprocess = start_daemon (&daemon_root);
    stop_daemon (subprocess);
    after_rebuild = query_catalog_string (daemon_root.catalog_path,
            facts_sql);
    g_assert_cmpstr (after_rebuild, ==, before);
}

static void
test_wyreboxd_updates_flags (void)
{
    g_auto (DaemonRoot) daemon_root = { 0 };
    g_autoptr (GSubprocess) subprocess = NULL;
    const char *const set_flags[] = { "\\Seen", "\\Flagged", NULL };
    const char *const set_keywords[] = { "work", NULL };
    const char *const clear_flags[] = { "\\Flagged", NULL };
    const char *const updated[] = { "", "\\Seen work", NULL };
    const char *const untouched[] = { "", "", NULL };
    g_autofree char *catalog_wal_path = NULL;

    daemon_root_init (&daemon_root);
    catalog_wal_path = g_strconcat (daemon_root.catalog_path, ".wal", NULL);
    journal_delivery_offline (&daemon_root, "delivery-1", "account-1");
    subprocess = start_daemon (&daemon_root);
    deliver (&daemon_root, "delivery-2");
    assert_inbox_flags (&daemon_root, untouched);

    {
        g_auto (WyreboxDaemonResponseFrame) frame = { 0 };

        update_inbox_flags (&daemon_root, 1, 2,
            WYREBOX_DAEMON_FLAG_KEYWORD_UPDATE_MODE_SET, set_flags,
            set_keywords, &frame);
        g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_SUCCESS);
    }
    {
        g_auto (WyreboxDaemonResponseFrame) frame = { 0 };

        update_inbox_flags (&daemon_root, 1, 2,
            WYREBOX_DAEMON_FLAG_KEYWORD_UPDATE_MODE_CLEAR, clear_flags, NULL,
            &frame);
        g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_SUCCESS);
    }
    assert_inbox_flags (&daemon_root, updated);

    {
        g_auto (WyreboxDaemonResponseFrame) frame = { 0 };

        update_inbox_flags (&daemon_root, 1, 99,
            WYREBOX_DAEMON_FLAG_KEYWORD_UPDATE_MODE_SET, set_flags, NULL,
            &frame);
        g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_ERROR);
        g_assert_cmpint (frame.error.error_class, ==,
            WYREBOX_DAEMON_ERROR_NOT_FOUND);
    }
    {
        g_auto (WyreboxDaemonResponseFrame) frame = { 0 };

        update_inbox_flags (&daemon_root, 2, 1,
            WYREBOX_DAEMON_FLAG_KEYWORD_UPDATE_MODE_SET, set_flags, NULL,
            &frame);
        g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_ERROR);
        g_assert_cmpint (frame.error.error_class, ==,
            WYREBOX_DAEMON_ERROR_CONFLICT);
    }
    stop_daemon (subprocess);
    g_clear_object (&subprocess);

    subprocess = start_daemon (&daemon_root);
    assert_inbox_flags (&daemon_root, updated);
    stop_daemon (subprocess);
    g_clear_object (&subprocess);

    g_assert_cmpint (g_remove (daemon_root.catalog_path), ==, 0);
    (void)g_remove (catalog_wal_path);
    subprocess = start_daemon (&daemon_root);
    assert_inbox_flags (&daemon_root, updated);
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

static void
test_wyreboxd_derives_virtual_mailboxes_from_facts (void)
{
    g_auto (DaemonRoot) daemon_root = { 0 };
    g_autoptr (GSubprocess) subprocess = NULL;
    g_autofree char *first = NULL;
    g_autofree char *second = NULL;
    g_autofree char *third = NULL;
    g_autofree char *before_restart = NULL;
    g_autofree char *after_restart = NULL;
    g_autofree char *expected_members = NULL;
    const char *members_sql =
        "SELECT string_agg(message_id || '=' || uid || ':' || is_visible, "
        "', ' ORDER BY uid) FROM derived_view_memberships "
        "WHERE account_id = 'account-1' AND view_id = 'projects';";
    guint32 uid_validity = 0;

    daemon_root_init (&daemon_root);
    configure_projects_view (&daemon_root, PROJECTS_RULES);
    subprocess = start_daemon (&daemon_root);

    first = deliver_message (&daemon_root, "delivery-1");
    second = deliver_message (&daemon_root, "delivery-2");
    third = deliver_message (&daemon_root, "delivery-3");
    assert_projects_state (&daemon_root, 0, 1, 0);
    uid_validity = projects_uid_validity (&daemon_root);

    mutate_keyword_fact (&daemon_root, WYREBOX_DAEMON_FACT_MUTATION_INSERT,
        third, "project");
    mutate_keyword_fact (&daemon_root, WYREBOX_DAEMON_FACT_MUTATION_INSERT,
        second, "other");
    mutate_keyword_fact (&daemon_root, WYREBOX_DAEMON_FACT_MUTATION_INSERT,
        first, "project");
    assert_projects_state (&daemon_root, uid_validity, 3, 2);

    mutate_keyword_fact (&daemon_root, WYREBOX_DAEMON_FACT_MUTATION_RETRACT,
        third, "project");
    assert_projects_state (&daemon_root, uid_validity, 3, 1);
    mutate_keyword_fact (&daemon_root, WYREBOX_DAEMON_FACT_MUTATION_INSERT,
        third, "project");
    assert_projects_state (&daemon_root, uid_validity, 3, 2);

    {
        g_auto (WyreboxDaemonResponseFrame) frame = { 0 };
        gboolean first_sorts_first = g_strcmp0 (first, third) < 0;
        g_autofree char *expected = g_strdup_printf
                ("account_id,view_id,message_id\n"
                "account-1,projects,%s\naccount-1,projects,%s\n",
                first_sorts_first ? first : third,
                first_sorts_first ? third : first);
        g_autofree char *data = NULL;
        gsize size = 0;

        query_predicate (&daemon_root, "show_in_virtual_folder.v1", &frame);
        g_assert_cmpint (frame.kind, ==,
            WYREBOX_DAEMON_RESPONSE_FRAME_STREAM_CHUNK);
        data = g_strndup (g_bytes_get_data (frame.stream_chunk.bytes, &size),
                g_bytes_get_size (frame.stream_chunk.bytes));
        g_assert_cmpstr (data, ==, expected);
    }
    {
        const char *bindings[] = { NULL };
        g_auto (WyreboxDaemonWirelogPredicateQueryRequest) request = { 0 };
        g_auto (WyreboxDaemonResponseFrame) frame = { 0 };
        g_autoptr (GError) error = NULL;

        g_assert_false (wyrebox_daemon_wirelog_predicate_query_request_init
                (&request, "query-1",
            "show_in_virtual_folder(\"x\", m) :- has_keyword(m, \"other\").",
            "account-1", bindings, &error));
        g_assert_nonnull (error);

        query_predicate (&daemon_root, "has_keyword.v1", &frame);
        g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_ERROR);
        g_assert_cmpint (frame.error.error_class, ==,
            WYREBOX_DAEMON_ERROR_PERMANENT_FAILURE);
    }
    stop_daemon (subprocess);
    g_clear_object (&subprocess);

    before_restart = query_catalog_string (daemon_root.catalog_path,
            members_sql);
    expected_members = g_strdup_printf ("%s=1:true, %s=2:true", third, first);
    g_assert_cmpstr (before_restart, ==, expected_members);

    subprocess = start_daemon (&daemon_root);
    assert_projects_state (&daemon_root, uid_validity, 3, 2);
    stop_daemon (subprocess);
    after_restart = query_catalog_string (daemon_root.catalog_path,
            members_sql);
    g_assert_cmpstr (after_restart, ==, before_restart);
}
#define APOLLO_RULES \
        ".decl project_keyword(message_id: symbol, project: symbol)\n" \
        ".decl show_in_virtual_folder(view_id: symbol, message_id: symbol)\n" \
        "show_in_virtual_folder(\"projects\", m) :- " \
        "project_keyword(m, \"apollo\").\n"

#define APOLLO_EXTRACTION_RULES \
        "[dictionary:apollo]\nfield=subject\nmatch=apollo\nproject=apollo\n"

/*
 * Summarizes extracted facts, extraction markers, derived view memberships
 * and derived view UID state for comparison across a rebuild.
 */
static char *
derived_state_snapshot (const char *catalog_path)
{
    g_autofree char *facts = query_catalog_string (catalog_path,
            "SELECT COALESCE(string_agg(message_id || ' ' || predicate || ' ' "
            "|| args_json || ' ' || source || '@' || journal_sequence, ';' "
            "ORDER BY fact_id), '') FROM message_facts;");
    g_autofree char *markers = query_catalog_string (catalog_path,
            "SELECT COALESCE(string_agg(message_id || ' ' || fact_count || '@' "
            "|| journal_sequence, ';' ORDER BY message_id), '') "
            "FROM message_fact_extractions;");
    g_autofree char *members = query_catalog_string (catalog_path,
            "SELECT COALESCE(string_agg(view_id || ' ' || message_id || '=' || "
            "uid || ':' || is_visible, ';' ORDER BY view_id, uid), '') "
            "FROM derived_view_memberships;");
    g_autofree char *uid_state = query_catalog_string (catalog_path,
            "SELECT COALESCE(string_agg(namespace_id || ' next=' || uidnext || "
            "' validity=' || uidvalidity, ';' ORDER BY namespace_id), '') "
            "FROM mailbox_uid_state WHERE namespace_kind = 'derived_view';");

    return g_strdup_printf ("facts[%s] markers[%s] members[%s] uids[%s]",
               facts, markers, members, uid_state);
}

static void
test_wyreboxd_derives_virtual_mailboxes_from_delivery_headers (void)
{
    g_auto (DaemonRoot) daemon_root = { 0 };
    g_autoptr (GSubprocess) subprocess = NULL;
    g_autofree char *apollo = NULL;
    g_autofree char *weekly = NULL;
    g_autofree char *members = NULL;
    g_autofree char *expected_members = NULL;
    g_autofree char *before = NULL;
    g_autofree char *after = NULL;
    g_autofree char *catalog_wal_path = NULL;
    gsize journal_length = 0;
    gsize rebuilt_journal_length = 0;
    guint32 uid_validity = 0;

    daemon_root_init (&daemon_root);
    catalog_wal_path = g_strconcat (daemon_root.catalog_path, ".wal", NULL);
    configure_projects_view (&daemon_root, APOLLO_RULES);
    configure_extraction_rules (&daemon_root, APOLLO_EXTRACTION_RULES);
    subprocess = start_daemon (&daemon_root);

    weekly = deliver_message (&daemon_root, "weekly-report");
    assert_projects_state (&daemon_root, 0, 1, 0);
    apollo = deliver_message (&daemon_root, "Apollo launch plan");
    uid_validity = projects_uid_validity (&daemon_root);
    assert_projects_state (&daemon_root, uid_validity, 2, 1);
    stop_daemon (subprocess);
    g_clear_object (&subprocess);

    members = query_catalog_string (daemon_root.catalog_path,
            "SELECT string_agg(message_id || '=' || uid || ':' || is_visible, "
            "', ' ORDER BY uid) FROM derived_view_memberships;");
    expected_members = g_strdup_printf ("%s=1:true", apollo);
    g_assert_cmpstr (members, ==, expected_members);
    g_assert_cmpstr (weekly, !=, apollo);
    before = derived_state_snapshot (daemon_root.catalog_path);
    g_assert_nonnull (strstr (before, "dictionary:subject:apollo"));
    g_assert_nonnull (strstr (before, "header:from"));
    g_free (read_segment (&daemon_root, &journal_length));

    g_assert_cmpint (g_remove (daemon_root.catalog_path), ==, 0);
    (void)g_remove (catalog_wal_path);
    subprocess = start_daemon (&daemon_root);
    assert_projects_state (&daemon_root, uid_validity, 2, 1);
    stop_daemon (subprocess);
    after = derived_state_snapshot (daemon_root.catalog_path);
    g_assert_cmpstr (after, ==, before);
    g_free (read_segment (&daemon_root, &rebuilt_journal_length));
    g_assert_cmpuint (rebuilt_journal_length, ==, journal_length);
}

#define THREAD_RULES \
        ".decl message_id(message: symbol, rfc_id: symbol)\n" \
        ".decl replies_to(message: symbol, rfc_id: symbol)\n" \
        ".decl project_keyword(message: symbol, project: symbol)\n" \
        ".decl linked(a: symbol, b: symbol)\n" \
        ".decl in_thread(a: symbol, b: symbol)\n" \
        ".decl show_in_virtual_folder(view_id: symbol, message_id: symbol)\n" \
        "linked(a, b) :- replies_to(a, id), message_id(b, id).\n" \
        "linked(a, b) :- replies_to(b, id), message_id(a, id).\n" \
        "in_thread(a, a) :- project_keyword(a, \"apollo\").\n" \
        "in_thread(a, c) :- in_thread(a, b), linked(b, c).\n" \
        "show_in_virtual_folder(\"projects\", m) :- in_thread(r, m).\n"

static gint
compare_strings (gconstpointer a, gconstpointer b)
{
    return g_strcmp0 (*(const char * const *)a, *(const char * const *)b);
}

static void
test_wyreboxd_threads_replies_delivered_out_of_order (void)
{
    g_auto (DaemonRoot) daemon_root = { 0 };
    g_autoptr (GSubprocess) subprocess = NULL;
    g_autofree char *grandchild = NULL;
    g_autofree char *reply = NULL;
    g_autofree char *root = NULL;
    g_autofree char *unrelated = NULL;
    g_autofree char *members = NULL;
    g_autofree char *expected_members = NULL;

    daemon_root_init (&daemon_root);
    configure_projects_view (&daemon_root, THREAD_RULES);
    configure_extraction_rules (&daemon_root, APOLLO_EXTRACTION_RULES);
    subprocess = start_daemon (&daemon_root);

    grandchild = deliver_headers (&daemon_root, "delivery-grandchild",
            "From: carol@example.com\r\nTo: team@example.com\r\n"
            "Subject: Re: Re: notes\r\n"
            "Message-ID: <grandchild@example.com>\r\n"
            "In-Reply-To: <reply@example.com>\r\n");
    reply = deliver_headers (&daemon_root, "delivery-reply",
            "From: bob@example.com\r\nTo: team@example.com\r\n"
            "Subject: Re: notes\r\nMessage-ID: <reply@example.com>\r\n"
            "In-Reply-To: <root@example.com>\r\n");
    assert_projects_state (&daemon_root, 0, 1, 0);
    unrelated = deliver_headers (&daemon_root, "delivery-unrelated",
            "From: dave@example.com\r\nTo: team@example.com\r\n"
            "Subject: Lunch\r\nMessage-ID: <lunch@example.com>\r\n");
    root = deliver_headers (&daemon_root, "delivery-root",
            "From: alice@example.com\r\nTo: team@example.com\r\n"
            "Subject: Apollo notes\r\nMessage-ID: <root@example.com>\r\n");
    assert_projects_state (&daemon_root, 0, 4, 3);
    stop_daemon (subprocess);

    members = query_catalog_string (daemon_root.catalog_path,
            "SELECT string_agg(message_id, ', ' ORDER BY message_id) "
            "FROM derived_view_memberships WHERE is_visible;");
    {
        g_autoptr (GPtrArray) ids = g_ptr_array_new ();

        g_ptr_array_add (ids, grandchild);
        g_ptr_array_add (ids, reply);
        g_ptr_array_add (ids, root);
        g_ptr_array_sort (ids, compare_strings);
        expected_members = g_strdup_printf ("%s, %s, %s",
                (char *)g_ptr_array_index (ids, 0),
                (char *)g_ptr_array_index (ids, 1),
                (char *)g_ptr_array_index (ids, 2));
    }
    g_assert_cmpstr (members, ==, expected_members);
    g_assert_null (strstr (members, unrelated));
}
#endif

static void
test_wyreboxd_rejects_invalid_wirelog_config (void)
{
    g_auto (DaemonRoot) missing_rules = { 0 };
    g_auto (DaemonRoot) broken_rules = { 0 };
    g_auto (DaemonRoot) undeclared_relation = { 0 };
    g_auto (DaemonRoot) missing_extraction_rules = { 0 };
    g_auto (DaemonRoot) invalid_extraction_rules = { 0 };

    daemon_root_init (&missing_rules);
    configure_projects_view (&missing_rules, NULL);
    assert_daemon_startup_fails (&missing_rules, EX_CONFIG,
        "Wirelog rules file");
    assert_daemon_startup_fails (&missing_rules, EX_CONFIG, "cannot be read");

    daemon_root_init (&broken_rules);
    configure_projects_view (&broken_rules, "show_in_virtual_folder(");
    assert_daemon_startup_fails (&broken_rules, EX_CONFIG, "does not compile");

    daemon_root_init (&undeclared_relation);
    configure_projects_view (&undeclared_relation,
        ".decl has_keyword(message_id: symbol, keyword: symbol)\n");
    assert_daemon_startup_fails (&undeclared_relation, EX_CONFIG,
        "must declare show_in_virtual_folder");

    daemon_root_init (&missing_extraction_rules);
    configure_projects_view (&missing_extraction_rules, PROJECTS_RULES);
    configure_extraction_rules (&missing_extraction_rules, NULL);
    assert_daemon_startup_fails (&missing_extraction_rules, EX_CONFIG,
        "extraction rules file");

    daemon_root_init (&invalid_extraction_rules);
    configure_projects_view (&invalid_extraction_rules, PROJECTS_RULES);
    configure_extraction_rules (&invalid_extraction_rules,
        "[regex:invoice]\nfield=subject\npredicate=reference_candidate\n"
        "pattern=INV-(\n");
    assert_daemon_startup_fails (&invalid_extraction_rules, EX_CONFIG,
        "invalid regex pattern for rule 'invoice'");
}

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
    g_test_add_func
        ("/daemon-api/wyreboxd/exits-tempfail-on-uninitialized-storage",
        test_wyreboxd_exits_tempfail_on_uninitialized_storage);
    g_test_add_func
        ("/daemon-api/wyreboxd/exits-tempfail-when-storage-roots-are-missing",
        test_wyreboxd_exits_tempfail_when_storage_roots_are_missing);
    g_test_add_func ("/daemon-api/wyreboxd/"
        "exits-tempfail-on-torn-first-record-with-unmounted-object-store",
        test_wyreboxd_exits_tempfail_on_torn_first_record_with_unmounted_object_store);
    g_test_add_func
        ("/daemon-api/wyreboxd/exits-dataerr-on-mismatched-storage-ids",
        test_wyreboxd_exits_dataerr_on_mismatched_storage_ids);
    g_test_add_func
        ("/daemon-api/wyreboxd/exits-dataerr-on-oversized-storage-marker",
        test_wyreboxd_exits_dataerr_on_oversized_storage_marker);
    g_test_add_func
        ("/daemon-api/wyreboxd/exits-tempfail-on-unreadable-storage-marker",
        test_wyreboxd_exits_tempfail_on_unreadable_storage_marker);
    g_test_add_func ("/daemon-api/wyreboxd/initialize-storage-then-starts",
        test_wyreboxd_initialize_storage_then_starts);
    g_test_add_func
        ("/daemon-api/wyreboxd/initialize-storage-adopts-existing-storage",
        test_wyreboxd_initialize_storage_adopts_existing_storage);
    g_test_add_func
        ("/daemon-api/wyreboxd/initialize-storage-refuses-half-mounted-storage",
        test_wyreboxd_initialize_storage_refuses_half_mounted_storage);
    g_test_add_func
        ("/daemon-api/wyreboxd/initialize-storage-refuses-mismatched-markers",
        test_wyreboxd_initialize_storage_refuses_mismatched_markers);
    g_test_add_func
        ("/daemon-api/wyreboxd/initialize-storage-rejects-invalid-config",
        test_wyreboxd_initialize_storage_rejects_invalid_config);
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
    g_test_add_func ("/daemon-api/wyreboxd/fetches-message-bytes",
        test_wyreboxd_fetches_message_bytes);
    g_test_add_func ("/daemon-api/wyreboxd/searches-messages",
        test_wyreboxd_searches_messages);
    g_test_add_func ("/daemon-api/wyreboxd/updates-flags",
        test_wyreboxd_updates_flags);
    g_test_add_func ("/daemon-api/wyreboxd/queries-templates",
        test_wyreboxd_queries_templates);
    g_test_add_func ("/daemon-api/wyreboxd/streams-mail-events",
        test_wyreboxd_streams_mail_events);
    g_test_add_func ("/daemon-api/wyreboxd/materializes-journaled-facts",
        test_wyreboxd_materializes_journaled_facts);
    g_test_add_func
        ("/daemon-api/wyreboxd/rebuilds-identical-catalog-after-restart",
        test_wyreboxd_rebuilds_identical_catalog_after_restart);
    g_test_add_func ("/daemon-api/wyreboxd/recovers-torn-journal-suffix",
        test_wyreboxd_recovers_torn_journal_suffix);
    g_test_add_func
        ("/daemon-api/wyreboxd/acknowledges-delivery-when-materialization-fails",
        test_wyreboxd_acknowledges_delivery_when_materialization_fails);
    g_test_add_func
        ("/daemon-api/wyreboxd/derives-virtual-mailboxes-from-facts",
        test_wyreboxd_derives_virtual_mailboxes_from_facts);
    g_test_add_func ("/daemon-api/wyreboxd/"
        "derives-virtual-mailboxes-from-delivery-headers",
        test_wyreboxd_derives_virtual_mailboxes_from_delivery_headers);
    g_test_add_func ("/daemon-api/wyreboxd/"
        "threads-replies-delivered-out-of-order",
        test_wyreboxd_threads_replies_delivered_out_of_order);
#endif
    g_test_add_func ("/daemon-api/wyreboxd/rejects-invalid-wirelog-config",
        test_wyreboxd_rejects_invalid_wirelog_config);

    return g_test_run ();
}
