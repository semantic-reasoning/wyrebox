#include "wyrebox-dovecot-daemon-client.h"
#include "wyrebox-schema-metadata-store.h"

#include <gio/gio.h>
#include <glib.h>
#include <glib/gstdio.h>

#include <signal.h>
#include <string.h>
#include <sys/prctl.h>
#include <unistd.h>

#define MESON_TEST_SKIP 77
#define TEST_ACCOUNT "account-1"

typedef struct
{
    char *root;
    char *socket_path;
    char *plugin_dir;
    GSubprocess *daemon;
} ImapRealDaemonFixture;

typedef struct
{
    char *output;
    char *errors;
} ImapSession;

static void
imap_session_clear (ImapSession *session)
{
    g_clear_pointer (&session->output, g_free);
    g_clear_pointer (&session->errors, g_free);
}

/* *INDENT-OFF* */
G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (ImapSession, imap_session_clear)
/* *INDENT-ON* */

static const char *
required_env (const char *name)
{
    const char *value = g_getenv (name);

    g_assert_nonnull (value);
    g_assert_cmpstr (value, !=, "");
    return value;
}

static gboolean
wait_for_socket (const char *socket_path)
{
    for (guint i = 0; i < 500; i++) {
        if (g_file_test (socket_path, G_FILE_TEST_EXISTS))
            return TRUE;
        g_usleep (10 * 1000);
    }

    return FALSE;
}

static void
write_file (const char *path, const char *contents)
{
    g_autoptr (GError) error = NULL;

    g_assert_true (g_file_set_contents (path, contents, -1, &error));
    g_assert_no_error (error);
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

    subprocess = g_subprocess_newv (argv, flags, &error);
    g_assert_no_error (error);
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
bootstrap_daemon_storage (const char *root, const char *socket_path,
    char **out_config_path)
{
    g_autofree char *journal_dir = g_build_filename (root, "journal", NULL);
    g_autofree char *object_dir = g_build_filename (root, "objects", NULL);
    g_autofree char *catalog_path = g_build_filename (root, "catalog.duckdb",
            NULL);
    g_autofree char *config_path = g_build_filename (root, "wyrebox.conf",
            NULL);
    g_autofree char *run_dir = g_path_get_dirname (socket_path);
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

    contents = g_strdup_printf ("[daemon]\n" "socket_path=%s\n"
            "journal_root_dir=%s\n" "object_root_dir=%s\n" "catalog_path=%s\n",
            socket_path, journal_dir, object_dir, catalog_path);
    write_file (config_path, contents);
    g_assert_cmpint (g_chmod (config_path, 0600), ==, 0);

    const char *argv[] = {
        required_env ("WYREBOXD_EXECUTABLE"),
        "--initialize-storage", "--config", config_path, NULL
    };
    run_checked (argv, NULL);

    *out_config_path = g_steal_pointer (&config_path);
}

static void
deliver_fixture (const char *socket_path, const char *eml_name)
{
    g_autofree char *eml_path = g_build_filename (required_env
                ("WYREBOX_EML_FIXTURE_DIR"), eml_name, NULL);
    g_autofree char *contents = NULL;
    gsize size = 0;
    g_autoptr (GBytes) message = NULL;
    g_autoptr (GError) error = NULL;
    const char *argv[] = {
        required_env ("WYREBOX_POSTFIX_PIPE_EXECUTABLE"),
        "--account-id", TEST_ACCOUNT,
        "--delivery-id", eml_name,
        "--queue-id", "queue-1",
        "--sender", "sender@example.com",
        "--recipient", "alice@example.com",
        "--socket", socket_path,
        NULL
    };

    g_assert_true (g_file_get_contents (eml_path, &contents, &size, &error));
    g_assert_no_error (error);
    message = g_bytes_new_take (g_steal_pointer (&contents), size);
    run_checked (argv, message);
}

static void
die_with_parent (gpointer user_data)
{
    (void)user_data;
    prctl (PR_SET_PDEATHSIG, SIGTERM);
}

static void
fixture_set_up (ImapRealDaemonFixture *fixture, gconstpointer user_data)
{
    g_autoptr (GError) error = NULL;
    g_autofree char *config_path = NULL;
    g_autofree char *plugin_link = NULL;
    g_autoptr (GSubprocessLauncher) launcher = NULL;

    (void)user_data;

    fixture->root = g_dir_make_tmp ("wyrebox-dovecot-imap-XXXXXX", &error);
    g_assert_no_error (error);
    fixture->socket_path = g_build_filename (fixture->root, "run",
            "wyrebox.sock", NULL);
    fixture->plugin_dir = g_build_filename (fixture->root, "plugins", NULL);

    g_assert_cmpint (g_mkdir_with_parents (fixture->plugin_dir, 0750), ==, 0);
    plugin_link = g_build_filename (fixture->plugin_dir, "wyrebox_plugin.so",
            NULL);
    g_assert_cmpint (symlink (required_env ("WYREBOX_DOVECOT_PLUGIN"),
        plugin_link), ==, 0);

    bootstrap_daemon_storage (fixture->root, fixture->socket_path,
        &config_path);

    const char *argv[] = {
        required_env ("WYREBOXD_EXECUTABLE"), "--config", config_path, NULL
    };
    /* An aborted assertion must not leave wyreboxd holding the test's
     * output pipes open. */
    launcher = g_subprocess_launcher_new (G_SUBPROCESS_FLAGS_NONE);
    g_subprocess_launcher_set_child_setup (launcher, die_with_parent, NULL,
        NULL);
    fixture->daemon = g_subprocess_launcher_spawnv (launcher, argv, &error);
    g_assert_no_error (error);
    g_assert_true (wait_for_socket (fixture->socket_path));

    deliver_fixture (fixture->socket_path, "simple-crlf.eml");
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
stop_daemon (ImapRealDaemonFixture *fixture)
{
    if (fixture->daemon != NULL) {
        g_subprocess_send_signal (fixture->daemon, SIGTERM);
        g_subprocess_wait (fixture->daemon, NULL, NULL);
        g_clear_object (&fixture->daemon);
    }
}

static void
fixture_tear_down (ImapRealDaemonFixture *fixture, gconstpointer user_data)
{
    (void)user_data;

    stop_daemon (fixture);

    if (g_getenv ("WYREBOX_KEEP_TEST_ROOT") == NULL)
        remove_tree (fixture->root);

    g_clear_pointer (&fixture->root, g_free);
    g_clear_pointer (&fixture->socket_path, g_free);
    g_clear_pointer (&fixture->plugin_dir, g_free);
}

static char *
write_dovecot_config (ImapRealDaemonFixture *fixture, const char *socket_path,
    const char *list_settings)
{
    g_autofree char *base_dir = g_build_filename (fixture->root, "dovecot-run",
            NULL);
    g_autofree char *state_dir = g_build_filename (fixture->root,
            "dovecot-state", NULL);
    g_autofree char *home_root = g_build_filename (fixture->root, "home",
            NULL);
    g_autofree char *contents = NULL;
    char *path = g_build_filename (fixture->root, "dovecot.conf", NULL);

    g_assert_cmpint (g_mkdir_with_parents (base_dir, 0750), ==, 0);
    g_assert_cmpint (g_mkdir_with_parents (state_dir, 0750), ==, 0);
    g_assert_cmpint (g_mkdir_with_parents (home_root, 0750), ==, 0);

    contents = g_strdup_printf ("dovecot_config_version = 2.4.2\n"
            "dovecot_storage_version = 2.4.2\n"
            "base_dir = %s\n"
            "state_dir = %s\n"
            "log_path = /dev/stderr\n"
            "ssl = no\n"
            "mail_home = %s/%%{user}\n"
            "mail_driver = wyrebox\n"
            "%s"
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
            "}\n", base_dir, state_dir, home_root, list_settings,
            fixture->plugin_dir, socket_path);
    write_file (path, contents);
    return path;
}

static void
run_imap (const char *config_path, const char *commands, ImapSession *session)
{
    g_autoptr (GSubprocessLauncher) launcher = NULL;
    g_autoptr (GSubprocess) subprocess = NULL;
    g_autoptr (GError) error = NULL;
    const char *argv[] = {
        required_env ("DOVECOT_IMAP"), "-c", config_path, NULL
    };

    imap_session_clear (session);
    launcher = g_subprocess_launcher_new (G_SUBPROCESS_FLAGS_STDIN_PIPE |
            G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE);
    g_subprocess_launcher_setenv (launcher, "USER", TEST_ACCOUNT, TRUE);
    subprocess = g_subprocess_launcher_spawnv (launcher, argv, &error);
    g_assert_no_error (error);
    g_assert_true (g_subprocess_communicate_utf8 (subprocess, commands, NULL,
        &session->output, &session->errors, &error));
    g_assert_no_error (error);
    g_test_message ("imap stdout:\n%s", session->output);
    g_test_message ("imap stderr:\n%s", session->errors);
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
test_list_shows_daemon_inbox (ImapRealDaemonFixture *fixture,
    gconstpointer user_data)
{
    g_autofree char *config_path = NULL;
    g_auto (ImapSession) session = { 0 };

    (void)user_data;

    config_path = write_dovecot_config (fixture, fixture->socket_path,
            "mailbox_list_layout = wyrebox\nmail_path =\n");
    run_imap (config_path, "a LIST \"\" \"*\"\r\nz LOGOUT\r\n", &session);

    g_assert_nonnull (strstr (session.output, "* PREAUTH"));
    g_assert_cmpuint (count_occurrences (session.output, "\"/\" INBOX\r\n"),
        ==, 1);
    g_assert_nonnull (strstr (session.output, "\r\na OK"));
    g_assert_nonnull (strstr (session.output, "\r\nz OK"));
}

static void
test_list_uses_configured_socket (ImapRealDaemonFixture *fixture,
    gconstpointer user_data)
{
    g_autofree char *config_path = NULL;
    g_autofree char *missing_socket = NULL;
    g_auto (ImapSession) session = { 0 };

    (void)user_data;

    missing_socket = g_build_filename (fixture->root, "run", "missing.sock",
            NULL);
    config_path = write_dovecot_config (fixture, missing_socket,
            "mailbox_list_layout = wyrebox\nmail_path =\n");
    run_imap (config_path, "a LIST \"\" \"*\"\r\nz LOGOUT\r\n", &session);

    g_assert_nonnull (strstr (session.output, "\r\na NO [UNAVAILABLE]"));
}

static void
test_storage_requires_wyrebox_layout (ImapRealDaemonFixture *fixture,
    gconstpointer user_data)
{
    g_autofree char *config_path = NULL;
    g_autofree char *list_settings = NULL;
    g_autofree char *mail_dir = NULL;
    g_auto (ImapSession) session = { 0 };

    (void)user_data;

    mail_dir = g_build_filename (fixture->root, "mail", NULL);
    list_settings = g_strdup_printf ("mailbox_list_layout = fs\n"
            "mail_path = %s\n", mail_dir);
    config_path = write_dovecot_config (fixture, fixture->socket_path,
            list_settings);
    run_imap (config_path, "a LIST \"\" \"*\"\r\nz LOGOUT\r\n", &session);

    g_assert_null (strstr (session.output, "\r\na OK"));
    g_assert_nonnull (strstr (session.errors,
        "requires mailbox_list_layout = wyrebox"));
}

static char *
write_wyrebox_config (ImapRealDaemonFixture *fixture, const char *socket_path)
{
    return write_dovecot_config (fixture, socket_path,
               "mailbox_list_layout = wyrebox\nmail_path =\n");
}

static void
test_select_matches_daemon_state (ImapRealDaemonFixture *fixture,
    gconstpointer user_data)
{
    g_autofree char *config_path = NULL;
    g_autofree char *uidvalidity = NULL;
    g_autofree char *uidnext = NULL;
    g_auto (WyreboxDaemonMailboxSelectResult) daemon_state = { 0 };
    g_auto (ImapSession) session = { 0 };
    g_autoptr (GError) error = NULL;

    (void)user_data;

    g_assert_true (wyrebox_dovecot_daemon_client_select_mailbox
            (fixture->socket_path, TEST_ACCOUNT, "INBOX", &daemon_state,
        &error));
    g_assert_no_error (error);
    g_assert_cmpuint (daemon_state.message_count, ==, 1);

    config_path = write_wyrebox_config (fixture, fixture->socket_path);
    run_imap (config_path, "a SELECT INBOX\r\nz LOGOUT\r\n", &session);

    uidvalidity = g_strdup_printf ("[UIDVALIDITY %u]",
            daemon_state.uid_validity);
    uidnext = g_strdup_printf ("[UIDNEXT %u]", daemon_state.uid_next);
    g_assert_nonnull (strstr (session.output, "* 1 EXISTS\r\n"));
    g_assert_nonnull (strstr (session.output, uidvalidity));
    g_assert_nonnull (strstr (session.output, uidnext));
    g_assert_nonnull (strstr (session.output, "[UNSEEN 1]"));
    g_assert_nonnull (strstr (session.output, "\r\na OK [READ-ONLY]"));
}

static gint
compare_strings (gconstpointer a, gconstpointer b)
{
    return g_strcmp0 (*(const char *const *)a, *(const char *const *)b);
}

static void
assert_flag_set (const char *const *actual, const char *const *expected)
{
    g_autoptr (GPtrArray) got = g_ptr_array_new ();
    g_autoptr (GPtrArray) wanted = g_ptr_array_new ();

    for (guint i = 0; actual[i] != NULL; i++)
        g_ptr_array_add (got, (gpointer)actual[i]);
    for (guint i = 0; expected[i] != NULL; i++)
        g_ptr_array_add (wanted, (gpointer)expected[i]);
    g_ptr_array_sort (got, compare_strings);
    g_ptr_array_sort (wanted, compare_strings);

    g_assert_cmpuint (got->len, ==, wanted->len);
    for (guint i = 0; i < got->len; i++) {
        g_assert_cmpstr (g_ptr_array_index (got, i), ==,
            g_ptr_array_index (wanted, i));
    }
}

/*
 * Compares the last FLAGS reported for message @seq as a set. Pipelined
 * commands may report flag changes before earlier tagged replies, and
 * \Recent depends on the session.
 */
static void
assert_fetch_flags (const char *output, guint seq,
    const char *const *expected)
{
    g_autofree char *pattern = NULL;
    g_autoptr (GRegex) regex = NULL;
    g_autoptr (GMatchInfo) match = NULL;
    g_autofree char *flags = NULL;
    g_auto (GStrv) tokens = NULL;
    g_autoptr (GPtrArray) actual = g_ptr_array_new ();

    pattern =
        g_strdup_printf ("^\\* %u FETCH \\([^\\r\\n]*?FLAGS \\(([^)]*)\\)",
            seq);
    regex = g_regex_new (pattern, G_REGEX_MULTILINE, 0, NULL);
    g_assert_nonnull (regex);
    g_assert_true (g_regex_match (regex, output, 0, &match));
    while (g_match_info_matches (match)) {
        g_free (flags);
        flags = g_match_info_fetch (match, 1);
        g_match_info_next (match, NULL);
    }
    tokens = g_strsplit (flags, " ", -1);

    for (guint i = 0; tokens[i] != NULL; i++) {
        if (tokens[i][0] != '\0' && g_strcmp0 (tokens[i], "\\Recent") != 0)
            g_ptr_array_add (actual, tokens[i]);
    }
    g_ptr_array_add (actual, NULL);
    assert_flag_set ((const char *const *)actual->pdata, expected);
}

static void
test_select_reflects_daemon_flags (ImapRealDaemonFixture *fixture,
    gconstpointer user_data)
{
    const char *const system_flags[] = { "\\Seen", "\\Flagged", NULL };
    const char *const keywords[] = { "Project-X", NULL };
    const char *const expected[] = { "\\Seen", "\\Flagged", "Project-X", NULL };
    g_autofree char *config_path = NULL;
    g_auto (WyreboxDaemonMailboxSelectResult) daemon_state = { 0 };
    g_auto (ImapSession) session = { 0 };
    g_autoptr (GError) error = NULL;
    g_autofree char *flags_line = NULL;
    const char *flags_start;

    (void)user_data;

    g_assert_true (wyrebox_dovecot_daemon_client_select_mailbox
            (fixture->socket_path, TEST_ACCOUNT, "INBOX", &daemon_state,
        &error));
    g_assert_no_error (error);
    g_assert_true (wyrebox_dovecot_daemon_client_update_flags
            (fixture->socket_path, TEST_ACCOUNT, daemon_state.mailbox_id,
        daemon_state.uid_validity, 1,
        WYREBOX_DAEMON_FLAG_KEYWORD_UPDATE_MODE_SET, system_flags, keywords,
        NULL, &error));
    g_assert_no_error (error);

    config_path = write_wyrebox_config (fixture, fixture->socket_path);
    run_imap (config_path, "s STATUS INBOX (UNSEEN)\r\n"
        "a SELECT INBOX\r\n"
        "b UID FETCH 1 (FLAGS)\r\n"
        "c SEARCH UNSEEN\r\nz LOGOUT\r\n", &session);

    g_assert_nonnull (strstr (session.output, "* STATUS INBOX (UNSEEN 0)"));
    g_assert_null (strstr (session.output, "[UNSEEN "));
    flags_start = strstr (session.output, "* FLAGS (");
    g_assert_nonnull (flags_start);
    flags_line = g_strndup (flags_start, strcspn (flags_start, "\r\n"));
    g_assert_nonnull (strstr (flags_line, "Project-X"));
    assert_fetch_flags (session.output, 1, expected);
    g_assert_nonnull (strstr (session.output, "* SEARCH\r\n"));
    g_assert_nonnull (strstr (session.output, "\r\nc OK"));
}

/* Reads UID 1's flags straight from the daemon, bypassing Dovecot. */
static void
assert_daemon_flags (ImapRealDaemonFixture *fixture,
    const char *const *expected)
{
    g_auto (WyreboxDaemonMailboxSelectResult) daemon_state = { 0 };
    g_auto (WyreboxDovecotMailboxUidMapSnapshot) snapshot = { 0 };
    g_autoptr (GError) error = NULL;
    const WyreboxDovecotMailboxUidMapRow *row;

    g_assert_true (wyrebox_dovecot_daemon_client_select_mailbox
            (fixture->socket_path, TEST_ACCOUNT, "INBOX", &daemon_state,
        &error));
    g_assert_no_error (error);
    g_assert_true (wyrebox_dovecot_daemon_client_load_uid_map
            (fixture->socket_path, TEST_ACCOUNT, daemon_state.mailbox_id,
        daemon_state.kind, daemon_state.uid_validity, &snapshot, &error));
    g_assert_no_error (error);
    g_assert_cmpuint (snapshot.rows->len, ==, 1);
    row = g_ptr_array_index (snapshot.rows, 0);
    g_assert_cmpuint (row->uid, ==, 1);
    assert_flag_set ((const char *const *)row->flags, expected);
}

static void
test_store_updates_daemon_flags (ImapRealDaemonFixture *fixture,
    gconstpointer user_data)
{
    const char *const expected[] = { "\\Flagged", "Project-X", "Todo", NULL };
    g_autofree char *config_path = NULL;
    g_auto (ImapSession) session = { 0 };

    (void)user_data;

    config_path = write_wyrebox_config (fixture, fixture->socket_path);
    run_imap (config_path, "a SELECT INBOX\r\n"
        "b STORE 1 +FLAGS (\\Seen)\r\n"
        "c STORE 1 -FLAGS (\\Seen)\r\n"
        "d STORE 1 FLAGS (\\Seen \\Flagged Project-X)\r\n"
        "e UID STORE 1 +FLAGS (Todo)\r\n"
        "f STORE 1 -FLAGS (\\Seen)\r\n"
        "g FETCH 1 (UID FLAGS)\r\nz LOGOUT\r\n", &session);

    g_assert_nonnull (strstr (session.output, "\r\nb OK"));
    g_assert_nonnull (strstr (session.output, "\r\nc OK"));
    g_assert_nonnull (strstr (session.output, "\r\nd OK"));
    g_assert_nonnull (strstr (session.output, "\r\ne OK"));
    g_assert_nonnull (strstr (session.output, "\r\nf OK"));
    g_assert_nonnull (strstr (session.output, "\r\ng OK"));
    assert_fetch_flags (session.output, 1, expected);
    assert_daemon_flags (fixture, expected);
}

static void
test_store_rejected_keyword_reverts (ImapRealDaemonFixture *fixture,
    gconstpointer user_data)
{
    const char *const expected[] = { NULL };
    g_autofree char *config_path = NULL;
    g_auto (ImapSession) session = { 0 };

    (void)user_data;

    config_path = write_wyrebox_config (fixture, fixture->socket_path);
    run_imap (config_path, "a SELECT INBOX\r\n"
        "b STORE 1 +FLAGS (bad+kw)\r\n"
        "c NOOP\r\n"
        "d FETCH 1 (UID FLAGS)\r\nz LOGOUT\r\n", &session);

    g_assert_nonnull (strstr (session.output, "\r\nb NO [CANNOT]"));
    g_assert_nonnull (strstr (session.output, "\r\nc OK"));
    assert_fetch_flags (session.output, 1, expected);
    assert_daemon_flags (fixture, expected);
}

static void
test_fresh_delivery_is_unseen (ImapRealDaemonFixture *fixture,
    gconstpointer user_data)
{
    const char *const expected[] = { NULL };
    g_autofree char *config_path = NULL;
    g_auto (ImapSession) session = { 0 };

    (void)user_data;

    config_path = write_wyrebox_config (fixture, fixture->socket_path);
    run_imap (config_path, "s STATUS INBOX (UNSEEN)\r\n"
        "a SELECT INBOX\r\n"
        "b UID FETCH 1 (FLAGS)\r\n"
        "c SEARCH UNSEEN\r\nz LOGOUT\r\n", &session);

    g_assert_nonnull (strstr (session.output, "* STATUS INBOX (UNSEEN 1)"));
    assert_fetch_flags (session.output, 1, expected);
    g_assert_nonnull (strstr (session.output, "* SEARCH 1\r\n"));
    g_assert_nonnull (strstr (session.output, "\r\nc OK"));
}

static void
test_status_matches_daemon_state (ImapRealDaemonFixture *fixture,
    gconstpointer user_data)
{
    g_autofree char *config_path = NULL;
    g_autofree char *expected = NULL;
    g_auto (WyreboxDaemonMailboxSelectResult) daemon_state = { 0 };
    g_auto (ImapSession) session = { 0 };
    g_autoptr (GError) error = NULL;

    (void)user_data;

    g_assert_true (wyrebox_dovecot_daemon_client_get_mailbox_status
            (fixture->socket_path, TEST_ACCOUNT, "INBOX", &daemon_state,
        &error));
    g_assert_no_error (error);

    config_path = write_wyrebox_config (fixture, fixture->socket_path);
    run_imap (config_path,
        "a STATUS INBOX (MESSAGES UIDNEXT UIDVALIDITY)\r\nz LOGOUT\r\n",
        &session);

    expected = g_strdup_printf ("* STATUS INBOX (MESSAGES %u UIDNEXT %u "
            "UIDVALIDITY %u)\r\n", daemon_state.message_count,
            daemon_state.uid_next, daemon_state.uid_validity);
    g_assert_nonnull (strstr (session.output, expected));
    g_assert_nonnull (strstr (session.output, "\r\na OK"));
}

static void
test_fetch_returns_original_bytes (ImapRealDaemonFixture *fixture,
    gconstpointer user_data)
{
    g_autofree char *config_path = NULL;
    g_autofree char *eml_path = NULL;
    g_autofree char *expected = NULL;
    g_autofree char *literal_prefix = NULL;
    g_auto (ImapSession) session = { 0 };
    g_autoptr (GError) error = NULL;
    const char *literal;
    gsize expected_size = 0;

    (void)user_data;

    eml_path = g_build_filename (required_env ("WYREBOX_EML_FIXTURE_DIR"),
            "simple-crlf.eml", NULL);
    g_assert_true (g_file_get_contents (eml_path, &expected, &expected_size,
        &error));
    g_assert_no_error (error);

    config_path = write_wyrebox_config (fixture, fixture->socket_path);
    run_imap (config_path, "a SELECT INBOX\r\n"
        "b UID FETCH 1 (BODY.PEEK[])\r\nz LOGOUT\r\n", &session);

    literal_prefix = g_strdup_printf ("* 1 FETCH (UID 1 BODY[] {%"
            G_GSIZE_FORMAT "}\r\n", expected_size);
    literal = strstr (session.output, literal_prefix);
    g_assert_nonnull (literal);
    literal += strlen (literal_prefix);
    g_assert_cmpuint (strlen (literal), >=, expected_size);
    g_assert_cmpmem (literal, expected_size, expected, expected_size);
    g_assert_nonnull (strstr (session.output, "\r\nb OK"));
}

static void
test_select_missing_mailbox_is_nonexistent (ImapRealDaemonFixture *fixture,
    gconstpointer user_data)
{
    g_autofree char *config_path = NULL;
    g_auto (ImapSession) session = { 0 };

    (void)user_data;

    config_path = write_wyrebox_config (fixture, fixture->socket_path);
    run_imap (config_path, "a SELECT Missing\r\nz LOGOUT\r\n", &session);

    g_assert_nonnull (strstr (session.output, "\r\na NO [NONEXISTENT]"));
}

static void
test_select_reports_unavailable_daemon (ImapRealDaemonFixture *fixture,
    gconstpointer user_data)
{
    g_autofree char *config_path = NULL;
    g_auto (ImapSession) session = { 0 };

    (void)user_data;

    stop_daemon (fixture);
    config_path = write_wyrebox_config (fixture, fixture->socket_path);
    run_imap (config_path, "a SELECT INBOX\r\nz LOGOUT\r\n", &session);

    g_assert_nonnull (strstr (session.output, "\r\na NO [UNAVAILABLE]"));
    g_assert_nonnull (strstr (session.output, "\r\nz OK"));
}

static gboolean
should_skip (void)
{
    const char *plugin = g_getenv ("WYREBOX_DOVECOT_PLUGIN");
    const char *imap = g_getenv ("DOVECOT_IMAP");

    if (plugin == NULL || plugin[0] == '\0'
        || !g_file_test (plugin, G_FILE_TEST_IS_REGULAR)) {
        g_print ("skipping: WyreBox Dovecot plugin was not built\n");
        return TRUE;
    }
    if (imap == NULL || imap[0] == '\0'
        || !g_file_test (imap, G_FILE_TEST_IS_EXECUTABLE)) {
        g_print ("skipping: Dovecot imap binary is not installed\n");
        return TRUE;
    }
    if (geteuid () == 0) {
        g_print ("skipping: Dovecot imap refuses to run as root without "
            "mail_uid\n");
        return TRUE;
    }

    return FALSE;
}

int
main (int argc, char **argv)
{
    if (should_skip ())
        return MESON_TEST_SKIP;

    g_test_init (&argc, &argv, NULL);

    g_test_add ("/dovecot/imap/list-shows-daemon-inbox",
        ImapRealDaemonFixture, NULL, fixture_set_up,
        test_list_shows_daemon_inbox, fixture_tear_down);
    g_test_add ("/dovecot/imap/list-uses-configured-socket",
        ImapRealDaemonFixture, NULL, fixture_set_up,
        test_list_uses_configured_socket, fixture_tear_down);
    g_test_add ("/dovecot/imap/storage-requires-wyrebox-layout",
        ImapRealDaemonFixture, NULL, fixture_set_up,
        test_storage_requires_wyrebox_layout, fixture_tear_down);
    g_test_add ("/dovecot/imap/select-matches-daemon-state",
        ImapRealDaemonFixture, NULL, fixture_set_up,
        test_select_matches_daemon_state, fixture_tear_down);
    g_test_add ("/dovecot/imap/select-reflects-daemon-flags",
        ImapRealDaemonFixture, NULL, fixture_set_up,
        test_select_reflects_daemon_flags, fixture_tear_down);
    g_test_add ("/dovecot/imap/fresh-delivery-is-unseen",
        ImapRealDaemonFixture, NULL, fixture_set_up,
        test_fresh_delivery_is_unseen, fixture_tear_down);
    g_test_add ("/dovecot/imap/store-updates-daemon-flags",
        ImapRealDaemonFixture, NULL, fixture_set_up,
        test_store_updates_daemon_flags, fixture_tear_down);
    g_test_add ("/dovecot/imap/store-rejected-keyword-reverts",
        ImapRealDaemonFixture, NULL, fixture_set_up,
        test_store_rejected_keyword_reverts, fixture_tear_down);
    g_test_add ("/dovecot/imap/status-matches-daemon-state",
        ImapRealDaemonFixture, NULL, fixture_set_up,
        test_status_matches_daemon_state, fixture_tear_down);
    g_test_add ("/dovecot/imap/fetch-returns-original-bytes",
        ImapRealDaemonFixture, NULL, fixture_set_up,
        test_fetch_returns_original_bytes, fixture_tear_down);
    g_test_add ("/dovecot/imap/select-missing-mailbox-is-nonexistent",
        ImapRealDaemonFixture, NULL, fixture_set_up,
        test_select_missing_mailbox_is_nonexistent, fixture_tear_down);
    g_test_add ("/dovecot/imap/select-reports-unavailable-daemon",
        ImapRealDaemonFixture, NULL, fixture_set_up,
        test_select_reports_unavailable_daemon, fixture_tear_down);

    return g_test_run ();
}
