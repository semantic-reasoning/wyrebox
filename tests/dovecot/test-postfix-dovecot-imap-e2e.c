#include "wyrebox-dovecot-daemon-client.h"
#include "wyrebox-schema-metadata-store.h"

#include <gio/gio.h>
#include <gio/gunixsocketaddress.h>
#include <glib.h>
#include <glib/gstdio.h>

#include <grp.h>
#include <pwd.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/un.h>
#include <unistd.h>

#define MESON_TEST_SKIP 77
#define TEST_ACCOUNT "account-1"
#define TEST_PASSWORD "secret"
#define PIPE_FIXTURE "simple-crlf.eml"
#define LMTP_FIXTURE "multipart-attachment-like.eml"
#define PROCESS_DEADLINE_SECONDS 10
#define IMAP_TIMEOUT_SECONDS 30

typedef struct
{
    char *root;
    char *socket_path;
    char *config_path;
    char *dovecot_dir;
    char *imap_socket_path;
    GSubprocess *daemon;
    GSubprocess *master;
} E2eFixture;

typedef struct
{
    GSocketConnection *connection;
    GDataInputStream *input;
    GOutputStream *output;
} ImapClient;

typedef struct
{
    GString *text;
    /* Each IMAP literal of the response, in order, as GBytes. */
    GPtrArray *literals;
    /* The tagged completion line, without CRLF. */
    char *tagged;
} ImapResponse;

/* Set for the duration of each test so failure paths can dump the Dovecot
 * logs before the assertion aborts the process. */
static E2eFixture *current_fixture;

static void
imap_client_clear (ImapClient *client)
{
    g_clear_object (&client->input);
    client->output = NULL;
    g_clear_object (&client->connection);
}

static void
imap_response_clear (ImapResponse *response)
{
    if (response->text != NULL) {
        g_string_free (response->text, TRUE);
        response->text = NULL;
    }
    g_clear_pointer (&response->literals, g_ptr_array_unref);
    g_clear_pointer (&response->tagged, g_free);
}

/* *INDENT-OFF* */
G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (ImapClient, imap_client_clear)
G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (ImapResponse, imap_response_clear)
/* *INDENT-ON* */

static const char *
required_env (const char *name)
{
    const char *value = g_getenv (name);

    g_assert_nonnull (value);
    g_assert_cmpstr (value, !=, "");
    return value;
}

static void
write_file (const char *path, const char *contents)
{
    g_autoptr (GError) error = NULL;

    g_assert_true (g_file_set_contents (path, contents, -1, &error));
    g_assert_no_error (error);
}

static void
dump_dovecot_logs (void)
{
    const char *const names[] = { "master.out", "dovecot.log" };

    if (current_fixture == NULL || current_fixture->dovecot_dir == NULL)
        return;

    for (guint i = 0; i < G_N_ELEMENTS (names); i++) {
        g_autofree char *path = g_build_filename (current_fixture->dovecot_dir,
                names[i], NULL);
        g_autofree char *contents = NULL;

        if (g_file_get_contents (path, &contents, NULL, NULL))
            g_test_message ("%s:\n%s", names[i], contents);
    }
}

static GBytes *
load_fixture (const char *name)
{
    g_autofree char *path = g_build_filename (required_env
                ("WYREBOX_EML_FIXTURE_DIR"), name, NULL);
    g_autofree char *contents = NULL;
    g_autoptr (GError) error = NULL;
    gsize size = 0;

    g_assert_true (g_file_get_contents (path, &contents, &size, &error));
    g_assert_no_error (error);
    return g_bytes_new_take (g_steal_pointer (&contents), size);
}

static void
die_with_parent (gpointer user_data)
{
    (void)user_data;
    prctl (PR_SET_PDEATHSIG, SIGTERM);
}

static void
on_wait_done (GObject *source, GAsyncResult *result, gpointer user_data)
{
    gboolean *done = user_data;

    g_subprocess_wait_finish (G_SUBPROCESS (source), result, NULL);
    *done = TRUE;
}

static gboolean
on_deadline (gpointer user_data)
{
    gboolean *expired = user_data;

    *expired = TRUE;
    return G_SOURCE_REMOVE;
}

static gboolean
wait_with_deadline (GSubprocess *process, guint seconds)
{
    gboolean done = FALSE;
    gboolean expired = FALSE;
    guint timeout_id;

    g_subprocess_wait_async (process, NULL, on_wait_done, &done);
    timeout_id = g_timeout_add_seconds (seconds, on_deadline, &expired);
    while (!done && !expired)
        g_main_context_iteration (NULL, TRUE);
    if (!expired)
        g_source_remove (timeout_id);
    while (!done && expired) {
        /* The async wait still references the stack flag; let it finish. */
        g_subprocess_force_exit (process);
        g_main_context_iteration (NULL, TRUE);
    }

    return !expired;
}

/* Stops @process with SIGTERM, escalating to SIGKILL after the deadline.
 * Returns TRUE when it exited on SIGTERM with status 0; otherwise logs why,
 * naming the process as @name. */
static gboolean
stop_process (GSubprocess **process, const char *name)
{
    gboolean clean;

    if (*process == NULL)
        return TRUE;

    g_subprocess_send_signal (*process, SIGTERM);
    clean = FALSE;
    if (!wait_with_deadline (*process, PROCESS_DEADLINE_SECONDS)) {
        g_test_message ("%s ignored SIGTERM for %d s and was killed",
            name, PROCESS_DEADLINE_SECONDS);
    } else if (g_subprocess_get_if_signaled (*process)) {
        int signal_number = g_subprocess_get_term_sig (*process);

        g_test_message ("%s terminated by signal %d (%s)",
            name, signal_number, g_strsignal (signal_number));
    } else if (g_subprocess_get_exit_status (*process) != 0) {
        g_test_message ("%s exited with status %d",
            name, g_subprocess_get_exit_status (*process));
    } else {
        clean = TRUE;
    }
    g_clear_object (process);
    return clean;
}

/* When @output_path is set, the child's stdout and stderr share that file. */
static GSubprocess *
spawn_with_parent_death (const char *const *argv, GSubprocessFlags flags,
    const char *output_path)
{
    g_autoptr (GSubprocessLauncher) launcher = NULL;
    g_autoptr (GError) error = NULL;
    GSubprocess *process;

    if (output_path != NULL)
        flags |= G_SUBPROCESS_FLAGS_STDERR_MERGE;
    launcher = g_subprocess_launcher_new (flags);
    if (output_path != NULL)
        g_subprocess_launcher_set_stdout_file_path (launcher, output_path);
    g_subprocess_launcher_set_child_setup (launcher, die_with_parent, NULL,
        NULL);
    process = g_subprocess_launcher_spawnv (launcher, argv, &error);
    g_assert_no_error (error);
    return process;
}

static void
run_checked (const char *const *argv, GBytes *stdin_bytes)
{
    g_autoptr (GSubprocess) subprocess = NULL;
    g_autoptr (GBytes) stdout_bytes = NULL;
    g_autoptr (GBytes) stderr_bytes = NULL;
    g_autoptr (GError) error = NULL;
    GSubprocessFlags flags = G_SUBPROCESS_FLAGS_STDOUT_PIPE |
        G_SUBPROCESS_FLAGS_STDERR_PIPE;

    if (stdin_bytes != NULL)
        flags |= G_SUBPROCESS_FLAGS_STDIN_PIPE;

    subprocess = spawn_with_parent_death (argv, flags, NULL);
    g_assert_true (g_subprocess_communicate (subprocess, stdin_bytes, NULL,
        &stdout_bytes, &stderr_bytes, &error));
    g_assert_no_error (error);
    if (!g_subprocess_get_if_exited (subprocess)
        || g_subprocess_get_exit_status (subprocess) != 0) {
        g_test_message ("%s failed: %.*s", argv[0],
            (int)g_bytes_get_size (stderr_bytes),
            (const char *)g_bytes_get_data (stderr_bytes, NULL));
        g_assert_not_reached ();
    }
}

static void
bootstrap_daemon_storage (E2eFixture *fixture)
{
    g_autofree char *journal_dir = g_build_filename (fixture->root, "journal",
            NULL);
    g_autofree char *object_dir = g_build_filename (fixture->root, "objects",
            NULL);
    g_autofree char *catalog_path = g_build_filename (fixture->root,
            "catalog.duckdb", NULL);
    g_autofree char *run_dir = g_path_get_dirname (fixture->socket_path);
    g_autofree char *contents = NULL;
    g_autoptr (WyreboxSchemaMetadataStore) store = NULL;
    g_autoptr (GError) error = NULL;

    g_assert_cmpint (g_mkdir_with_parents (run_dir, 0750), ==, 0);
    g_assert_cmpint (g_mkdir_with_parents (journal_dir, 0750), ==, 0);
    g_assert_cmpint (g_mkdir_with_parents (object_dir, 0750), ==, 0);

    store = wyrebox_schema_metadata_store_new_duckdb (catalog_path, &error);
    g_assert_no_error (error);
    g_assert_true (wyrebox_schema_metadata_store_apply_migration_operation
            (store,
        WYREBOX_SCHEMA_METADATA_STORE_MIGRATION_OPERATION_LEGACY_BOOTSTRAP,
        0, 1, &error));
    g_assert_no_error (error);
    g_clear_object (&store);

    fixture->config_path = g_build_filename (fixture->root, "wyrebox.conf",
            NULL);
    contents = g_strdup_printf ("[daemon]\n" "socket_path=%s\n"
            "journal_root_dir=%s\n" "object_root_dir=%s\n" "catalog_path=%s\n",
            fixture->socket_path, journal_dir, object_dir, catalog_path);
    write_file (fixture->config_path, contents);
    g_assert_cmpint (g_chmod (fixture->config_path, 0600), ==, 0);

    const char *argv[] = {
        required_env ("WYREBOXD_EXECUTABLE"),
        "--initialize-storage", "--config", fixture->config_path, NULL
    };
    run_checked (argv, NULL);
}

static void
start_daemon (E2eFixture *fixture)
{
    const char *argv[] = {
        required_env ("WYREBOXD_EXECUTABLE"), "--config",
        fixture->config_path, NULL
    };

    g_assert_null (fixture->daemon);
    fixture->daemon = spawn_with_parent_death (argv, G_SUBPROCESS_FLAGS_NONE,
            NULL);
    for (guint i = 0; i < 500; i++) {
        if (g_file_test (fixture->socket_path, G_FILE_TEST_EXISTS))
            return;
        g_usleep (10 * 1000);
    }
    g_assert_not_reached ();
}

static void
deliver_pipe (E2eFixture *fixture, const char *delivery_id,
    const char *eml_name)
{
    g_autoptr (GBytes) message = load_fixture (eml_name);
    const char *argv[] = {
        required_env ("WYREBOX_POSTFIX_PIPE_EXECUTABLE"),
        "--account-id", TEST_ACCOUNT,
        "--delivery-id", delivery_id,
        "--queue-id", "queue-1",
        "--sender", "sender@example.com",
        "--recipient", "alice@example.com",
        "--socket", fixture->socket_path,
        NULL
    };

    run_checked (argv, message);
}

static void
dump_file (const char *path)
{
    g_autofree char *contents = NULL;

    if (g_file_get_contents (path, &contents, NULL, NULL))
        g_test_message ("%s:\n%s", path, contents);
}

/* Reads one LMTP reply, skipping "NNN-" continuation lines, and asserts its
 * final line starts with @prefix. */
static void
lmtp_expect (GDataInputStream *input, const char *prefix,
    const char *listener_output)
{
    for (;;) {
        g_autoptr (GError) error = NULL;
        g_autofree char *line = g_data_input_stream_read_line (input, NULL,
                NULL, &error);

        if (line != NULL && strlen (line) > 3 && line[3] == '-')
            continue;
        if (line == NULL || !g_str_has_prefix (line, prefix)) {
            g_test_message ("expected LMTP reply %s, got %s", prefix,
                line != NULL ? line : "EOF");
            dump_file (listener_output);
        }
        g_assert_no_error (error);
        g_assert_nonnull (line);
        g_assert_true (g_str_has_prefix (line, prefix));
        return;
    }
}

static void
lmtp_send (GOutputStream *output, const char *data, gssize size)
{
    g_autoptr (GError) error = NULL;

    if (size < 0)
        size = strlen (data);
    g_assert_true (g_output_stream_write_all (output, data, size, NULL, NULL,
        &error));
    g_assert_no_error (error);
}

/* Returns @message as an LMTP DATA payload: dot-stuffed and terminated by
 * ".\r\n". @message must end with CRLF so its bytes are stored unchanged. */
static GString *
lmtp_data_payload (GBytes *message)
{
    gsize size = 0;
    const char *data = g_bytes_get_data (message, &size);
    GString *payload = g_string_sized_new (size + 8);
    gboolean line_start = TRUE;

    for (gsize i = 0; i < size; i++) {
        if (line_start && data[i] == '.')
            g_string_append_c (payload, '.');
        g_string_append_c (payload, data[i]);
        line_start = data[i] == '\n';
    }
    g_assert_true (g_str_has_suffix (payload->str, "\r\n"));
    g_string_append (payload, ".\r\n");
    return payload;
}

/* Delivers @eml_name through the one-shot wyrebox-postfix-lmtp-delivery
 * listener, speaking the LMTP dialog Postfix lmtp(8) would. */
static void
deliver_lmtp (E2eFixture *fixture, const char *delivery_id,
    const char *eml_name)
{
    g_autoptr (GBytes) message = load_fixture (eml_name);
    g_autofree char *listen_socket = g_build_filename (fixture->root, "run",
            "wyrebox-lmtp.sock", NULL);
    g_autofree char *listener_output = g_build_filename (fixture->root,
            "lmtp.out", NULL);
    const char *argv[] = {
        required_env ("WYREBOX_POSTFIX_LMTP_DELIVERY_EXECUTABLE"),
        "--account-id", TEST_ACCOUNT,
        "--delivery-id", delivery_id,
        "--listen-socket", listen_socket,
        "--daemon-socket", fixture->socket_path,
        NULL
    };
    g_autoptr (GSubprocess) listener = NULL;
    g_autoptr (GSocketClient) socket_client = g_socket_client_new ();
    g_autoptr (GSocketAddress) address = NULL;
    g_autoptr (GSocketConnection) connection = NULL;
    g_autoptr (GDataInputStream) input = NULL;
    g_autoptr (GString) payload = lmtp_data_payload (message);
    g_autoptr (GError) error = NULL;
    GOutputStream *output;
    gint64 deadline;
    gboolean exited;

    listener = spawn_with_parent_death (argv, G_SUBPROCESS_FLAGS_NONE,
            listener_output);
    deadline = g_get_monotonic_time () +
        PROCESS_DEADLINE_SECONDS * G_TIME_SPAN_SECOND;
    while (!g_file_test (listen_socket, G_FILE_TEST_EXISTS)) {
        if (g_get_monotonic_time () >= deadline
            || g_subprocess_get_identifier (listener) == NULL) {
            g_test_message ("LMTP listener did not create %s", listen_socket);
            dump_file (listener_output);
            g_assert_not_reached ();
        }
        g_usleep (10 * 1000);
    }

    address = g_unix_socket_address_new (listen_socket);
    g_socket_client_set_timeout (socket_client, IMAP_TIMEOUT_SECONDS);
    connection = g_socket_client_connect (socket_client,
            G_SOCKET_CONNECTABLE (address), NULL, &error);
    g_assert_no_error (error);
    input = g_data_input_stream_new (g_io_stream_get_input_stream
                (G_IO_STREAM (connection)));
    g_data_input_stream_set_newline_type (input,
        G_DATA_STREAM_NEWLINE_TYPE_CR_LF);
    output = g_io_stream_get_output_stream (G_IO_STREAM (connection));

    lmtp_expect (input, "220 ", listener_output);
    lmtp_send (output, "LHLO localhost\r\n", -1);
    lmtp_expect (input, "250 ", listener_output);
    lmtp_send (output, "MAIL FROM:<sender@example.com>\r\n", -1);
    lmtp_expect (input, "250 ", listener_output);
    lmtp_send (output, "RCPT TO:<alice@example.com>\r\n", -1);
    lmtp_expect (input, "250 ", listener_output);
    lmtp_send (output, "DATA\r\n", -1);
    lmtp_expect (input, "354 ", listener_output);
    lmtp_send (output, payload->str, payload->len);
    lmtp_expect (input, "250 2.0.0 ", listener_output);
    lmtp_expect (input, "221 ", listener_output);
    g_assert_true (g_io_stream_close (G_IO_STREAM (connection), NULL, NULL));

    exited = wait_with_deadline (listener, PROCESS_DEADLINE_SECONDS);
    if (!exited || !g_subprocess_get_if_exited (listener)
        || g_subprocess_get_exit_status (listener) != 0)
        dump_file (listener_output);
    g_assert_true (exited);
    g_assert_true (g_subprocess_get_if_exited (listener));
    g_assert_cmpint (g_subprocess_get_exit_status (listener), ==, 0);
}

static void
write_dovecot_config (E2eFixture *fixture)
{
    struct passwd *pw = getpwuid (geteuid ());
    struct group *gr = getgrgid (getegid ());
    g_autofree char *run_dir = g_build_filename (fixture->dovecot_dir, "run",
            NULL);
    g_autofree char *state_dir = g_build_filename (fixture->dovecot_dir,
            "state", NULL);
    g_autofree char *home_root = g_build_filename (fixture->dovecot_dir,
            "home", NULL);
    g_autofree char *home = g_build_filename (home_root, TEST_ACCOUNT, NULL);
    g_autofree char *passwd_path = g_build_filename (fixture->dovecot_dir,
            "passwd", NULL);
    g_autofree char *log_path = g_build_filename (fixture->dovecot_dir,
            "dovecot.log", NULL);
    g_autofree char *plugin_dir = g_build_filename (fixture->root, "plugins",
            NULL);
    g_autofree char *conf_path = g_build_filename (fixture->dovecot_dir,
            "dovecot.conf", NULL);
    g_autofree char *passwd = NULL;
    g_autofree char *contents = NULL;

    g_assert_nonnull (pw);
    g_assert_nonnull (gr);
    g_assert_cmpint (g_mkdir_with_parents (run_dir, 0750), ==, 0);
    g_assert_cmpint (g_mkdir_with_parents (state_dir, 0750), ==, 0);
    g_assert_cmpint (g_mkdir_with_parents (home, 0750), ==, 0);

    passwd = g_strdup_printf ("%s:{PLAIN}%s:%u:%u::%s::\n", TEST_ACCOUNT,
            TEST_PASSWORD, (guint)geteuid (), (guint)getegid (), home);
    write_file (passwd_path, passwd);

    contents = g_strdup_printf ("dovecot_config_version = 2.4.2\n"
            "dovecot_storage_version = 2.4.2\n"
            "base_dir = %s\n"
            "state_dir = %s\n"
            "log_path = %s\n"
            "ssl = no\n"
            "protocols = imap\n"
            "default_internal_user = %s\n"
            "default_internal_group = %s\n"
            "default_login_user = %s\n"
            "first_valid_uid = %u\n"
            "first_valid_gid = %u\n"
            "auth_allow_cleartext = yes\n"
            "mail_home = %s/%%{user}\n"
            "mail_driver = wyrebox\n"
            "mailbox_list_layout = wyrebox\n"
            "mail_path =\n"
            "mail_attachment_detection_options =\n"
            "mail_plugin_dir = %s\n"
            "mail_plugins {\n"
            "  wyrebox = yes\n"
            "}\n"
            "namespace inbox {\n"
            "  inbox = yes\n"
            "  separator = /\n"
            "}\n"
            "import_environment {\n"
            "  WYREBOX_SOCKET = %s\n"
            "}\n"
            "passdb passwd-file {\n"
            "  passwd_file_path = %s\n"
            "}\n"
            "userdb passwd-file {\n"
            "  passwd_file_path = %s\n"
            "}\n"
            "service imap-login {\n"
            "  chroot =\n"
            "  inet_listener imap {\n"
            "    port = 0\n"
            "  }\n"
            "  inet_listener imaps {\n"
            "    port = 0\n"
            "  }\n"
            "  unix_listener imap-e2e {\n"
            "    mode = 0600\n"
            "  }\n"
            "}\n"
            "service imap-urlauth-login {\n"
            "  chroot =\n"
            "}\n", run_dir, state_dir, log_path, pw->pw_name, gr->gr_name,
            pw->pw_name, (guint)geteuid (), (guint)getegid (), home_root,
            plugin_dir, fixture->socket_path, passwd_path, passwd_path);
    write_file (conf_path, contents);

    fixture->imap_socket_path = g_build_filename (run_dir, "imap-e2e", NULL);
}

static gboolean
imap_client_connect (ImapClient *client, const char *socket_path,
    GError **error)
{
    g_autoptr (GSocketClient) socket_client = g_socket_client_new ();
    g_autoptr (GSocketAddress) address =
        g_unix_socket_address_new (socket_path);

    g_socket_client_set_timeout (socket_client, IMAP_TIMEOUT_SECONDS);
    client->connection = g_socket_client_connect (socket_client,
            G_SOCKET_CONNECTABLE (address), NULL, error);
    if (client->connection == NULL)
        return FALSE;

    client->input = g_data_input_stream_new (g_io_stream_get_input_stream
                (G_IO_STREAM (client->connection)));
    g_data_input_stream_set_newline_type (client->input,
        G_DATA_STREAM_NEWLINE_TYPE_CR_LF);
    client->output =
        g_io_stream_get_output_stream (G_IO_STREAM (client->connection));
    return TRUE;
}

/* Reads one response line, appending it with its CRLF to @response. When the
 * line announces a literal, its bytes are read exactly, kept as a literal,
 * and the rest of the line is read too. Returns the final line, or NULL on
 * EOF. */
static char *
imap_read_line (ImapClient *client, ImapResponse *response)
{
    g_autoptr (GError) error = NULL;
    g_autofree char *line = NULL;

    for (;;) {
        g_autofree char *part = NULL;
        gsize length = 0;
        const char *brace;
        guint64 literal_size;
        char *end = NULL;

        part = g_data_input_stream_read_line (client->input, &length, NULL,
                &error);
        g_assert_no_error (error);
        if (part == NULL)
            return NULL;

        g_string_append_len (response->text, part, length);
        g_string_append (response->text, "\r\n");
        g_free (line);
        line = g_steal_pointer (&part);

        brace = length > 0 && line[length - 1] == '}' ?
            strrchr (line, '{') : NULL;
        if (brace == NULL || !g_ascii_isdigit (brace[1]))
            return g_steal_pointer (&line);

        literal_size = g_ascii_strtoull (brace + 1, &end, 10);
        if (end != line + length - 1 || end == brace + 1)
            return g_steal_pointer (&line);
        g_assert_cmpuint (literal_size, <, 64 * 1024 * 1024);
        {
            g_autofree guint8 *data = g_malloc (literal_size + 1);
            gsize read = 0;

            g_assert_true (g_input_stream_read_all (G_INPUT_STREAM
                    (client->input), data, literal_size, &read, NULL, &error));
            g_assert_no_error (error);
            g_assert_cmpuint (read, ==, literal_size);
            g_string_append_len (response->text, (const char *)data, read);
            g_ptr_array_add (response->literals, g_bytes_new_take
                    (g_steal_pointer (&data), read));
        }
    }
}

static void
imap_response_init (ImapResponse *response)
{
    response->text = g_string_new (NULL);
    response->literals = g_ptr_array_new_with_free_func ((GDestroyNotify)
            g_bytes_unref);
}

static void
expect_tagged_ok (const ImapResponse *response, const char *tag)
{
    g_autofree char *ok = g_strdup_printf ("%s OK", tag);

    if (response->tagged == NULL || !g_str_has_prefix (response->tagged, ok)) {
        g_test_message ("expected %s, got:\n%s", ok, response->text->str);
        dump_dovecot_logs ();
    }
    g_assert_nonnull (response->tagged);
    g_assert_true (g_str_has_prefix (response->tagged, ok));
}

static void
imap_read_greeting (ImapClient *client)
{
    g_auto (ImapResponse) response = { 0 };
    g_autofree char *line = NULL;

    imap_response_init (&response);
    line = imap_read_line (client, &response);
    if (line == NULL || !g_str_has_prefix (line, "* OK"))
        dump_dovecot_logs ();
    g_assert_nonnull (line);
    g_assert_true (g_str_has_prefix (line, "* OK"));
}

/* Sends "@tag @command" and collects the response up to the tagged line.
 * EOF, or an untagged BYE outside LOGOUT, fails the test. */
static void
imap_command (ImapClient *client, const char *tag, const char *command,
    ImapResponse *response)
{
    g_autofree char *request = g_strdup_printf ("%s %s\r\n", tag, command);
    g_autofree char *tag_prefix = g_strdup_printf ("%s ", tag);
    gboolean logout = g_ascii_strcasecmp (command, "LOGOUT") == 0;
    g_autoptr (GError) error = NULL;

    imap_response_init (response);
    g_assert_true (g_output_stream_write_all (client->output, request,
        strlen (request), NULL, NULL, &error));
    g_assert_no_error (error);

    for (;;) {
        g_autofree char *line = imap_read_line (client, response);

        if (line == NULL || (!logout && g_str_has_prefix (line, "* BYE"))) {
            g_test_message ("IMAP response so far:\n%s", response->text->str);
            dump_dovecot_logs ();
            g_assert_not_reached ();
        }
        if (g_str_has_prefix (line, tag_prefix)) {
            response->tagged = g_steal_pointer (&line);
            break;
        }
    }
    g_test_message ("%s%s", request, response->text->str);
}

static void
start_master (E2eFixture *fixture)
{
    g_autofree char *conf_path = g_build_filename (fixture->dovecot_dir,
            "dovecot.conf", NULL);
    g_autofree char *output_path = g_build_filename (fixture->dovecot_dir,
            "master.out", NULL);
    const char *argv[] = {
        required_env ("DOVECOT_MASTER"), "-F", "-c", conf_path, NULL
    };
    gint64 deadline;

    g_assert_null (fixture->master);
    /* Dovecot does not remove its listener sockets on exit. */
    g_remove (fixture->imap_socket_path);
    fixture->master = spawn_with_parent_death (argv, G_SUBPROCESS_FLAGS_NONE,
            output_path);

    deadline = g_get_monotonic_time () +
        PROCESS_DEADLINE_SECONDS * G_TIME_SPAN_SECOND;
    while (g_get_monotonic_time () < deadline) {
        g_auto (ImapClient) client = { 0 };

        if (imap_client_connect (&client, fixture->imap_socket_path, NULL)) {
            imap_read_greeting (&client);
            return;
        }
        /* The identifier becomes NULL once the master has been reaped. */
        if (g_subprocess_get_identifier (fixture->master) == NULL)
            break;
        g_usleep (50 * 1000);
    }
    g_test_message ("Dovecot master did not accept IMAP connections");
    dump_dovecot_logs ();
    g_assert_not_reached ();
}

static void
imap_login (ImapClient *client, E2eFixture *fixture)
{
    g_auto (ImapResponse) response = { 0 };
    g_autoptr (GError) error = NULL;
    gboolean connected;

    connected = imap_client_connect (client, fixture->imap_socket_path,
            &error);
    g_assert_no_error (error);
    g_assert_true (connected);
    imap_read_greeting (client);
    imap_command (client, "a", "LOGIN " TEST_ACCOUNT " " TEST_PASSWORD,
        &response);
    expect_tagged_ok (&response, "a");
}

static void
imap_logout (ImapClient *client)
{
    g_auto (ImapResponse) response = { 0 };

    imap_command (client, "z", "LOGOUT", &response);
    expect_tagged_ok (&response, "z");
}

static void
remove_tree (const char *path)
{
    g_autoptr (GDir) dir = g_dir_open (path, 0, NULL);
    const char *name;

    if (dir != NULL && !g_file_test (path, G_FILE_TEST_IS_SYMLINK)) {
        while ((name = g_dir_read_name (dir)) != NULL) {
            g_autofree char *child = g_build_filename (path, name, NULL);

            remove_tree (child);
        }
    }

    g_remove (path);
}

static void
fixture_set_up (E2eFixture *fixture, gconstpointer user_data)
{
    g_autoptr (GError) error = NULL;
    g_autofree char *plugin_dir = NULL;
    g_autofree char *plugin_link = NULL;

    (void)user_data;

    current_fixture = fixture;
    fixture->root = g_dir_make_tmp ("wb-e2e-XXXXXX", &error);
    g_assert_no_error (error);
    fixture->socket_path = g_build_filename (fixture->root, "run",
            "wyrebox.sock", NULL);
    fixture->dovecot_dir = g_build_filename (fixture->root, "dc", NULL);
    /* Dovecot listeners live under dc/run; keep the longest one within
     * sun_path. */
    g_assert_cmpuint (strlen (fixture->dovecot_dir) + strlen ("/run/") + 32,
        <, sizeof (((struct sockaddr_un *)NULL)->sun_path));

    plugin_dir = g_build_filename (fixture->root, "plugins", NULL);
    g_assert_cmpint (g_mkdir_with_parents (plugin_dir, 0750), ==, 0);
    plugin_link = g_build_filename (plugin_dir, "wyrebox_plugin.so", NULL);
    g_assert_cmpint (symlink (required_env ("WYREBOX_DOVECOT_PLUGIN"),
        plugin_link), ==, 0);

    bootstrap_daemon_storage (fixture);
    start_daemon (fixture);
    deliver_pipe (fixture, "pipe-simple", PIPE_FIXTURE);
    deliver_lmtp (fixture, "lmtp-multipart", LMTP_FIXTURE);

    write_dovecot_config (fixture);
    start_master (fixture);
}

static void
fixture_tear_down (E2eFixture *fixture, gconstpointer user_data)
{
    (void)user_data;

    stop_process (&fixture->master, "dovecot");
    g_assert_true (stop_process (&fixture->daemon, "wyreboxd"));

    if (g_getenv ("WYREBOX_KEEP_TEST_ROOT") == NULL)
        remove_tree (fixture->root);

    g_clear_pointer (&fixture->root, g_free);
    g_clear_pointer (&fixture->socket_path, g_free);
    g_clear_pointer (&fixture->config_path, g_free);
    g_clear_pointer (&fixture->dovecot_dir, g_free);
    g_clear_pointer (&fixture->imap_socket_path, g_free);
    current_fixture = NULL;
}

static guint
count_occurrences (const char *haystack, const char *needle)
{
    guint count = 0;

    for (const char *at = strstr (haystack, needle); at != NULL;
        at = strstr (at + 1, needle))
        count++;
    return count;
}

static void
test_login_list_select (E2eFixture *fixture, gconstpointer user_data)
{
    g_auto (ImapClient) client = { 0 };
    g_auto (ImapResponse) list = { 0 };
    g_auto (ImapResponse) select = { 0 };
    g_auto (WyreboxDaemonMailboxSelectResult) daemon_state = { 0 };
    g_autoptr (GError) error = NULL;
    g_autofree char *uidvalidity = NULL;
    gboolean selected;

    (void)user_data;

    selected = wyrebox_dovecot_daemon_client_select_mailbox
            (fixture->socket_path, TEST_ACCOUNT, "INBOX", &daemon_state,
            &error);
    g_assert_no_error (error);
    g_assert_true (selected);

    imap_login (&client, fixture);
    imap_command (&client, "b", "LIST \"\" \"*\"", &list);
    g_assert_cmpuint (count_occurrences (list.text->str, "\"/\" INBOX\r\n"),
        ==, 1);
    expect_tagged_ok (&list, "b");

    imap_command (&client, "c", "SELECT INBOX", &select);
    uidvalidity = g_strdup_printf ("[UIDVALIDITY %u]",
            daemon_state.uid_validity);
    g_assert_nonnull (strstr (select.text->str, "* 2 EXISTS\r\n"));
    g_assert_nonnull (strstr (select.text->str, "[UIDNEXT 3]"));
    g_assert_nonnull (strstr (select.text->str, uidvalidity));
    expect_tagged_ok (&select, "c");
    g_assert_true (g_str_has_prefix (select.tagged, "c OK [READ-WRITE]"));
    imap_logout (&client);
}

/* Fetches message @uid (also its sequence number) and compares it
 * byte-for-byte with fixture @eml_name. */
static void
expect_fetch_matches_fixture (E2eFixture *fixture, guint uid,
    const char *eml_name)
{
    g_auto (ImapClient) client = { 0 };
    g_auto (ImapResponse) select = { 0 };
    g_auto (ImapResponse) fetch = { 0 };
    g_autoptr (GBytes) expected = load_fixture (eml_name);
    g_autofree char *command = NULL;
    g_autofree char *prefix = NULL;

    imap_login (&client, fixture);
    imap_command (&client, "b", "SELECT INBOX", &select);
    expect_tagged_ok (&select, "b");
    command = g_strdup_printf ("UID FETCH %u (BODY.PEEK[])", uid);
    imap_command (&client, "c", command, &fetch);

    prefix = g_strdup_printf ("* %u FETCH (UID %u BODY[] {%" G_GSIZE_FORMAT
            "}\r\n", uid, uid, g_bytes_get_size (expected));
    g_assert_true (g_str_has_prefix (fetch.text->str, prefix));
    g_assert_cmpuint (fetch.literals->len, ==, 1);
    g_assert_true (g_bytes_equal (g_ptr_array_index (fetch.literals, 0),
        expected));
    expect_tagged_ok (&fetch, "c");
    imap_logout (&client);
}

static void
test_fetch_pipe_delivery (E2eFixture *fixture, gconstpointer user_data)
{
    (void)user_data;
    expect_fetch_matches_fixture (fixture, 1, PIPE_FIXTURE);
}

static void
test_fetch_lmtp_delivery (E2eFixture *fixture, gconstpointer user_data)
{
    (void)user_data;
    expect_fetch_matches_fixture (fixture, 2, LMTP_FIXTURE);
}

typedef struct
{
    guint uid_validity;
    guint uid_next;
    guint exists;
    /* Space-separated UIDs from "UID FETCH 1:* (UID)", in response order. */
    char *uids;
} MailboxSnapshot;

static void
mailbox_snapshot_clear (MailboxSnapshot *snapshot)
{
    g_clear_pointer (&snapshot->uids, g_free);
}

/* *INDENT-OFF* */
G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (MailboxSnapshot, mailbox_snapshot_clear)
/* *INDENT-ON* */

static GRegex *
multiline_regex (const char *pattern)
{
    g_autoptr (GError) error = NULL;
    GRegex *regex = g_regex_new (pattern, G_REGEX_MULTILINE, 0, &error);

    g_assert_no_error (error);
    g_assert_nonnull (regex);
    return regex;
}

/* Returns the number captured by the first group of @pattern in @text. */
static guint
match_uint (const char *text, const char *pattern)
{
    g_autoptr (GRegex) regex = multiline_regex (pattern);
    g_autoptr (GMatchInfo) match = NULL;
    g_autofree char *number = NULL;

    if (!g_regex_match (regex, text, 0, &match))
        g_test_message ("no match for %s in:\n%s", pattern, text);
    g_assert_true (g_match_info_matches (match));
    number = g_match_info_fetch (match, 1);
    return (guint)g_ascii_strtoull (number, NULL, 10);
}

static void
take_mailbox_snapshot (E2eFixture *fixture, MailboxSnapshot *snapshot)
{
    g_auto (ImapClient) client = { 0 };
    g_auto (ImapResponse) select = { 0 };
    g_auto (ImapResponse) fetch = { 0 };
    g_autoptr (GRegex) uid_regex =
        multiline_regex ("^\\* \\d+ FETCH \\(UID (\\d+)\\)\r$");
    g_autoptr (GMatchInfo) match = NULL;
    g_autoptr (GString) uids = g_string_new (NULL);

    imap_login (&client, fixture);
    imap_command (&client, "b", "SELECT INBOX", &select);
    expect_tagged_ok (&select, "b");
    snapshot->uid_validity = match_uint (select.text->str,
            "\\[UIDVALIDITY (\\d+)\\]");
    snapshot->uid_next = match_uint (select.text->str,
            "\\[UIDNEXT (\\d+)\\]");
    snapshot->exists = match_uint (select.text->str,
            "^\\* (\\d+) EXISTS\r$");

    imap_command (&client, "c", "UID FETCH 1:* (UID)", &fetch);
    expect_tagged_ok (&fetch, "c");
    g_regex_match (uid_regex, fetch.text->str, 0, &match);
    while (g_match_info_matches (match)) {
        g_autofree char *uid = g_match_info_fetch (match, 1);

        g_string_append_printf (uids, "%s%s", uids->len > 0 ? " " : "", uid);
        g_match_info_next (match, NULL);
    }
    snapshot->uids = g_string_free (g_steal_pointer (&uids), FALSE);
    imap_logout (&client);
}

static void
restart_daemon_and_master (E2eFixture *fixture)
{
    gboolean master_stopped = stop_process (&fixture->master, "dovecot");
    gboolean daemon_stopped = stop_process (&fixture->daemon, "wyreboxd");

    if (!master_stopped || !daemon_stopped)
        dump_dovecot_logs ();
    g_assert_true (master_stopped);
    g_assert_true (daemon_stopped);
    g_assert_false (g_file_test (fixture->socket_path, G_FILE_TEST_EXISTS));

    start_daemon (fixture);
    start_master (fixture);
}

static void
test_restart_preserves_uid_state (E2eFixture *fixture,
    gconstpointer user_data)
{
    g_auto (MailboxSnapshot) before = { 0 };
    g_auto (MailboxSnapshot) after = { 0 };
    g_auto (WyreboxDaemonMailboxSelectResult) daemon_state = { 0 };
    g_autoptr (GError) error = NULL;
    gboolean selected;

    (void)user_data;

    take_mailbox_snapshot (fixture, &before);
    g_assert_cmpuint (before.exists, ==, 2);
    g_assert_cmpuint (before.uid_next, ==, 3);
    g_assert_cmpstr (before.uids, ==, "1 2");

    restart_daemon_and_master (fixture);

    take_mailbox_snapshot (fixture, &after);
    g_assert_cmpuint (after.uid_validity, ==, before.uid_validity);
    g_assert_cmpuint (after.uid_next, ==, before.uid_next);
    g_assert_cmpuint (after.exists, ==, before.exists);
    g_assert_cmpstr (after.uids, ==, before.uids);

    selected = wyrebox_dovecot_daemon_client_select_mailbox
            (fixture->socket_path, TEST_ACCOUNT, "INBOX", &daemon_state,
            &error);
    g_assert_no_error (error);
    g_assert_true (selected);
    g_assert_cmpuint (before.uid_validity, !=, 0);
    g_assert_cmpuint (daemon_state.uid_validity, ==, before.uid_validity);
    g_assert_cmpuint (daemon_state.uid_validity, ==, after.uid_validity);

    expect_fetch_matches_fixture (fixture, 1, PIPE_FIXTURE);
    expect_fetch_matches_fixture (fixture, 2, LMTP_FIXTURE);
}

static gint
compare_strings (gconstpointer a, gconstpointer b)
{
    return g_strcmp0 (*(const char *const *)a, *(const char *const *)b);
}

/* Returns @flags as a sorted, space-separated set. */
static char *
flag_set_string (const char *const *flags)
{
    g_autoptr (GPtrArray) sorted = g_ptr_array_new ();

    for (guint i = 0; flags != NULL && flags[i] != NULL; i++) {
        if (flags[i][0] != '\0' && g_strcmp0 (flags[i], "\\Recent") != 0)
            g_ptr_array_add (sorted, (gpointer)flags[i]);
    }
    g_ptr_array_sort (sorted, compare_strings);
    g_ptr_array_add (sorted, NULL);
    return g_strjoinv (" ", (char **)sorted->pdata);
}

/* Fetches the flags of every message and compares them, as sets, with
 * @expected, one entry per UID starting at UID 1. */
static void
expect_imap_flags (E2eFixture *fixture, const char *const *expected)
{
    g_auto (ImapClient) client = { 0 };
    g_auto (ImapResponse) select = { 0 };
    g_auto (ImapResponse) fetch = { 0 };
    g_autoptr (GRegex) regex =
        multiline_regex (
        "^\\* \\d+ FETCH \\(UID (\\d+) FLAGS \\(([^)]*)\\)\\)\r$");
    g_autoptr (GMatchInfo) match = NULL;
    guint count = 0;

    imap_login (&client, fixture);
    imap_command (&client, "b", "SELECT INBOX", &select);
    expect_tagged_ok (&select, "b");
    g_assert_null (strstr (select.text->str, "$Has"));
    imap_command (&client, "c", "UID FETCH 1:* (UID FLAGS)", &fetch);
    expect_tagged_ok (&fetch, "c");

    g_regex_match (regex, fetch.text->str, 0, &match);
    while (g_match_info_matches (match)) {
        g_autofree char *uid = g_match_info_fetch (match, 1);
        g_autofree char *flags = g_match_info_fetch (match, 2);
        g_auto (GStrv) tokens = g_strsplit (flags, " ", -1);
        g_autofree char *actual = flag_set_string ((const char *const *)
                tokens);
        guint index = (guint)g_ascii_strtoull (uid, NULL, 10) - 1;

        g_assert_cmpuint (index, <, g_strv_length ((char **)expected));
        g_assert_cmpstr (actual, ==, expected[index]);
        count++;
        g_match_info_next (match, NULL);
    }
    g_assert_cmpuint (count, ==, g_strv_length ((char **)expected));
    imap_logout (&client);
}

/* Compares the daemon flags of every INBOX message, as sets, with
 * @expected, one entry per UID starting at UID 1. */
static void
expect_daemon_flags (E2eFixture *fixture, const char *const *expected)
{
    g_auto (WyreboxDaemonMailboxSelectResult) daemon_state = { 0 };
    g_auto (WyreboxDovecotMailboxUidMapSnapshot) snapshot = { 0 };
    g_autoptr (GError) error = NULL;

    g_assert_true (wyrebox_dovecot_daemon_client_select_mailbox
            (fixture->socket_path, TEST_ACCOUNT, "INBOX", &daemon_state,
        &error));
    g_assert_no_error (error);
    g_assert_true (wyrebox_dovecot_daemon_client_load_uid_map
            (fixture->socket_path, TEST_ACCOUNT, daemon_state.mailbox_id,
        daemon_state.kind, daemon_state.uid_validity, &snapshot, &error));
    g_assert_no_error (error);
    g_assert_cmpuint (snapshot.rows->len, ==,
        g_strv_length ((char **)expected));
    for (guint i = 0; i < snapshot.rows->len; i++) {
        const WyreboxDovecotMailboxUidMapRow *row =
            g_ptr_array_index (snapshot.rows, i);
        g_autofree char *actual =
            flag_set_string ((const char *const *)row->flags);

        g_assert_cmpuint (row->uid, >=, 1);
        g_assert_cmpuint (row->uid, <=, snapshot.rows->len);
        g_assert_cmpstr (actual, ==, expected[row->uid - 1]);
    }
}

static void
test_store_flags_survive_restart (E2eFixture *fixture,
    gconstpointer user_data)
{
    const char *const expected[] = {
        "Todo \\Flagged",
        "\\Flagged \\Seen",
        NULL
    };
    g_auto (ImapClient) client = { 0 };
    g_auto (ImapResponse) select = { 0 };
    g_auto (ImapResponse) store_both = { 0 };
    g_auto (ImapResponse) store_one = { 0 };
    g_auto (ImapResponse) fetch = { 0 };

    (void)user_data;

    imap_login (&client, fixture);
    imap_command (&client, "b", "SELECT INBOX", &select);
    expect_tagged_ok (&select, "b");
    imap_command (&client, "c", "UID STORE 1:2 +FLAGS (\\Flagged Todo)",
        &store_both);
    expect_tagged_ok (&store_both, "c");
    imap_command (&client, "d", "UID STORE 2 -FLAGS (Todo)", &store_one);
    expect_tagged_ok (&store_one, "d");
    imap_command (&client, "e", "UID FETCH 2 (BODY[])", &fetch);
    expect_tagged_ok (&fetch, "e");
    g_assert_null (strstr (fetch.text->str, "$Has"));
    imap_logout (&client);

    expect_daemon_flags (fixture, expected);
    expect_imap_flags (fixture, expected);

    restart_daemon_and_master (fixture);

    expect_daemon_flags (fixture, expected);
    expect_imap_flags (fixture, expected);
    expect_fetch_matches_fixture (fixture, 1, PIPE_FIXTURE);
    expect_fetch_matches_fixture (fixture, 2, LMTP_FIXTURE);
}

static gboolean
env_file_is (const char *name, GFileTest test)
{
    const char *value = g_getenv (name);

    return value != NULL && value[0] != '\0' && g_file_test (value, test);
}

static const char *
skip_reason (void)
{
    if (!env_file_is ("WYREBOX_DOVECOT_PLUGIN", G_FILE_TEST_IS_REGULAR))
        return "the WyreBox Dovecot plugin was not built";
    if (!env_file_is ("DOVECOT_MASTER", G_FILE_TEST_IS_EXECUTABLE))
        return "the Dovecot master binary is not installed";
    if (!env_file_is ("DOVECOT_IMAP_LOGIN", G_FILE_TEST_IS_EXECUTABLE))
        return "the Dovecot imap-login binary is not installed";
    if (!env_file_is ("DOVECOT_IMAP", G_FILE_TEST_IS_EXECUTABLE))
        return "the Dovecot imap binary is not installed";
    if (!env_file_is ("WYREBOXD_EXECUTABLE", G_FILE_TEST_IS_EXECUTABLE)
        || !env_file_is ("WYREBOX_POSTFIX_PIPE_EXECUTABLE",
        G_FILE_TEST_IS_EXECUTABLE)
        || !env_file_is ("WYREBOX_POSTFIX_LMTP_DELIVERY_EXECUTABLE",
        G_FILE_TEST_IS_EXECUTABLE))
        return "the WyreBox daemon or delivery helpers were not built";
    if (geteuid () == 0)
        return "the Dovecot master runs as the test user, not as root";
    if (getpwuid (geteuid ()) == NULL || getgrgid (getegid ()) == NULL)
        return "the test user has no passwd or group entry for Dovecot";

    return NULL;
}

int
main (int argc, char **argv)
{
    const char *reason = skip_reason ();

    if (reason != NULL
        && g_strcmp0 (g_getenv ("WYREBOX_E2E_REQUIRE_RUNTIME"), "1") == 0) {
        g_printerr ("required e2e runtime is missing: %s\n", reason);
        return EXIT_FAILURE;
    }
    if (reason != NULL) {
        g_print ("skipping: %s\n", reason);
        return MESON_TEST_SKIP;
    }

    g_test_init (&argc, &argv, NULL);

    g_test_add ("/e2e/login-list-select", E2eFixture, NULL, fixture_set_up,
        test_login_list_select, fixture_tear_down);
    g_test_add ("/e2e/fetch-pipe-delivery", E2eFixture, NULL, fixture_set_up,
        test_fetch_pipe_delivery, fixture_tear_down);
    g_test_add ("/e2e/fetch-lmtp-delivery", E2eFixture, NULL, fixture_set_up,
        test_fetch_lmtp_delivery, fixture_tear_down);
    g_test_add ("/e2e/restart-preserves-uid-state", E2eFixture, NULL,
        fixture_set_up, test_restart_preserves_uid_state, fixture_tear_down);
    g_test_add ("/e2e/store-flags-survive-restart", E2eFixture, NULL,
        fixture_set_up, test_store_flags_survive_restart, fixture_tear_down);

    return g_test_run ();
}
