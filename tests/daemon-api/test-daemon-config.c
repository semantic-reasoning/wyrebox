#include "wyrebox-daemon-config.h"
#include "wyrebox-daemon-runtime.h"

#include <gio/gio.h>
#include <glib.h>
#include <glib/gstdio.h>

#include <errno.h>
#include <string.h>
#include <sys/stat.h>

static char *
create_config_fixture_dir (void)
{
    g_autoptr (GError) error = NULL;
    g_autofree char *dir = g_dir_make_tmp ("wyrebox-daemon-config-XXXXXX",
            &error);

    g_assert_no_error (error);
    g_assert_nonnull (dir);
    return g_steal_pointer (&dir);
}

static char *
write_config_fixture (const char *dir, const char *contents, mode_t mode)
{
    g_autofree char *path = g_build_filename (dir, "wyrebox.conf", NULL);
    g_autoptr (GError) error = NULL;

    g_assert_true (g_file_set_contents (path, contents, -1, &error));
    g_assert_no_error (error);
    g_assert_cmpint (chmod (path, mode), ==, 0);

    return g_steal_pointer (&path);
}

static void
assert_config_loads (const char *config_path)
{
    g_autoptr (GError) error = NULL;
    g_autoptr (WyreboxDaemonConfig) config = NULL;

    config = wyrebox_daemon_config_new_from_file (config_path, &error);
    g_assert_no_error (error);
    g_assert_nonnull (config);
    g_assert_cmpstr (wyrebox_daemon_config_get_config_path (config), ==,
        config_path);
    g_assert_cmpstr (wyrebox_daemon_config_get_socket_path (config), ==,
        WYREBOX_DAEMON_DEFAULT_SOCKET_PATH);
    g_assert_cmpstr (wyrebox_daemon_config_get_journal_root_dir (config), ==,
        WYREBOX_DAEMON_DEFAULT_JOURNAL_ROOT_DIR);
    g_assert_cmpstr (wyrebox_daemon_config_get_object_root_dir (config), ==,
        WYREBOX_DAEMON_DEFAULT_OBJECT_ROOT_DIR);
    g_assert_cmpstr (wyrebox_daemon_config_get_catalog_path (config), ==,
        WYREBOX_DAEMON_DEFAULT_CATALOG_PATH);
}

static void
test_daemon_config_loads_canonical_config (void)
{
    g_autofree char *dir = create_config_fixture_dir ();
    g_autofree char *config_path = write_config_fixture (dir,
            "[daemon]\n"
            "socket_path=/run/wyrebox/wyrebox.sock\n"
            "journal_root_dir=/var/lib/wyrebox/journal\n"
            "object_root_dir=/var/lib/wyrebox/object-store\n"
            "catalog_path=/var/lib/wyrebox/catalog.duckdb\n",
            0600);

    assert_config_loads (config_path);
}

static void
test_daemon_config_validate_for_startup_accepts_absolute_socket_path (void)
{
    g_autofree char *dir = create_config_fixture_dir ();
    g_autofree char *config_path = write_config_fixture (dir,
            "[daemon]\n"
            "socket_path=/tmp/wyrebox.sock\n"
            "journal_root_dir=/tmp/wyrebox-journal\n"
            "object_root_dir=/tmp/wyrebox-object-store\n"
            "catalog_path=/tmp/wyrebox-catalog.duckdb\n",
            0600);
    g_autoptr (GError) error = NULL;
    g_autoptr (WyreboxDaemonConfig) config = NULL;

    config = wyrebox_daemon_config_new_from_file (config_path, &error);
    g_assert_no_error (error);
    g_assert_nonnull (config);
    g_assert_true (wyrebox_daemon_config_validate_for_startup (config, &error));
    g_assert_no_error (error);
}

static void
test_daemon_config_validate_for_startup_rejects_null_config (void)
{
    g_autoptr (GError) error = NULL;

    g_assert_false (wyrebox_daemon_config_validate_for_startup (NULL, &error));
    g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
    g_assert_nonnull (strstr (error->message, "daemon config is required"));
}

static void
test_daemon_config_rejects_empty_socket_path (void)
{
    g_autofree char *dir = create_config_fixture_dir ();
    g_autofree char *config_path = write_config_fixture (dir,
            "[daemon]\n" "socket_path=\n",
            0600);
    g_autoptr (GError) error = NULL;

    g_assert_null (wyrebox_daemon_config_new_from_file (config_path, &error));
    g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_assert_nonnull (strstr (error->message, "socket_path is required"));
}

static void
test_daemon_config_rejects_relative_socket_path (void)
{
    g_autofree char *dir = create_config_fixture_dir ();
    g_autofree char *config_path = write_config_fixture (dir,
            "[daemon]\n" "socket_path=run/wyrebox/wyrebox.sock\n",
            0600);
    g_autoptr (GError) error = NULL;

    g_assert_null (wyrebox_daemon_config_new_from_file (config_path, &error));
    g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_assert_nonnull (strstr (error->message, "must be absolute"));
}

static void
test_daemon_config_accepts_non_canonical_absolute_socket_path (void)
{
    g_autofree char *dir = create_config_fixture_dir ();
    g_autofree char *config_path = write_config_fixture (dir,
            "[daemon]\n"
            "socket_path=/tmp/wyrebox.sock\n"
            "journal_root_dir=/tmp/wyrebox-journal\n"
            "object_root_dir=/tmp/wyrebox-object-store\n"
            "catalog_path=/tmp/wyrebox-catalog.duckdb\n",
            0600);
    g_autoptr (GError) error = NULL;
    g_autoptr (WyreboxDaemonConfig) config = NULL;

    config = wyrebox_daemon_config_new_from_file (config_path, &error);
    g_assert_no_error (error);
    g_assert_nonnull (config);
    g_assert_true (wyrebox_daemon_config_validate_for_startup (config, &error));
    g_assert_no_error (error);
}

static void
test_daemon_config_rejects_unknown_keys (void)
{
    g_autofree char *dir = create_config_fixture_dir ();
    g_autofree char *config_path = write_config_fixture (dir,
            "[daemon]\n" "socket_path=/run/wyrebox/wyrebox.sock\n"
            "unexpected=1\n",
            0600);
    g_autoptr (GError) error = NULL;

    g_assert_null (wyrebox_daemon_config_new_from_file (config_path, &error));
    g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_assert_nonnull (strstr (error->message, "unknown key"));
}

static void
test_daemon_config_rejects_missing_socket_path (void)
{
    g_autofree char *dir = create_config_fixture_dir ();
    g_autofree char *config_path = write_config_fixture (dir,
            "[daemon]\n",
            0600);
    g_autoptr (GError) error = NULL;

    g_assert_null (wyrebox_daemon_config_new_from_file (config_path, &error));
    g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_assert_nonnull (strstr (error->message, "missing socket_path"));
}

static void
test_daemon_config_rejects_relative_catalog_path (void)
{
    g_autofree char *dir = create_config_fixture_dir ();
    g_autofree char *config_path = write_config_fixture (dir,
            "[daemon]\n"
            "socket_path=/run/wyrebox/wyrebox.sock\n"
            "catalog_path=var/lib/wyrebox/catalog.duckdb\n",
            0600);
    g_autoptr (GError) error = NULL;

    g_assert_null (wyrebox_daemon_config_new_from_file (config_path, &error));
    g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_assert_nonnull (strstr (error->message, "catalog_path must be absolute"));
}

static void
test_daemon_config_rejects_insecure_permissions (void)
{
    g_autofree char *dir = create_config_fixture_dir ();
    g_autofree char *config_path = write_config_fixture (dir,
            "[daemon]\n"
            "socket_path=/run/wyrebox/wyrebox.sock\n"
            "journal_root_dir=/var/lib/wyrebox/journal\n"
            "object_root_dir=/var/lib/wyrebox/object-store\n",
            0664);
    g_autoptr (GError) error = NULL;

    g_assert_null (wyrebox_daemon_config_new_from_file (config_path, &error));
    g_assert_error (error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
    g_assert_nonnull (strstr (error->message, "group- or world-writable"));
}

static void
test_daemon_config_rejects_malformed_assignment (void)
{
    g_autofree char *dir = create_config_fixture_dir ();
    g_autofree char *config_path = write_config_fixture (dir,
            "[daemon]\n" "socket_path /run/wyrebox/wyrebox.sock\n",
            0600);
    g_autoptr (GError) error = NULL;

    g_assert_null (wyrebox_daemon_config_new_from_file (config_path, &error));
    g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_assert_nonnull (strstr (error->message, "malformed assignment"));
}

static void
test_daemon_config_loads_wirelog_views (void)
{
    g_autofree char *dir = create_config_fixture_dir ();
    g_autofree char *config_path = write_config_fixture (dir,
            "[daemon]\n"
            "socket_path=/run/wyrebox/wyrebox.sock\n"
            "\n"
            "[wirelog]\n"
            "rules_path=/etc/wyrebox/views.dl\n"
            "extraction_rules_path=/etc/wyrebox/extraction.rules\n"
            "\n"
            "[view:projects]\n"
            "imap_name=Projects\n"
            "scope=thread\n"
            "\n"
            "[view:ops.alerts]\n"
            "imap_name=Ops/Alerts\n"
            "scope=message\n"
            "\n"
            "[view:vips]\n"
            "imap_name=VIPs\n"
            "scope=account\n",
            0600);
    g_autoptr (GError) error = NULL;
    g_autoptr (WyreboxDaemonConfig) config = NULL;

    config = wyrebox_daemon_config_new_from_file (config_path, &error);
    g_assert_no_error (error);
    g_assert_nonnull (config);
    g_assert_cmpstr (wyrebox_daemon_config_get_wirelog_rules_path (config), ==,
        "/etc/wyrebox/views.dl");
    g_assert_cmpstr (wyrebox_daemon_config_get_extraction_rules_path (config),
        ==, "/etc/wyrebox/extraction.rules");
    g_assert_cmpuint (wyrebox_daemon_config_get_n_views (config), ==, 3);
    g_assert_cmpstr (wyrebox_daemon_config_get_view_id (config, 0), ==,
        "projects");
    g_assert_cmpstr (wyrebox_daemon_config_get_view_imap_name (config, 0), ==,
        "Projects");
    g_assert_cmpstr (wyrebox_daemon_config_get_view_id (config, 1), ==,
        "ops.alerts");
    g_assert_cmpstr (wyrebox_daemon_config_get_view_imap_name (config, 1), ==,
        "Ops/Alerts");
    g_assert_cmpint (wyrebox_daemon_config_get_view_scope (config, 0), ==,
        WYREBOX_DAEMON_VIEW_SCOPE_THREAD);
    g_assert_cmpint (wyrebox_daemon_config_get_view_scope (config, 1), ==,
        WYREBOX_DAEMON_VIEW_SCOPE_MESSAGE);
    g_assert_cmpint (wyrebox_daemon_config_get_view_scope (config, 2), ==,
        WYREBOX_DAEMON_VIEW_SCOPE_ACCOUNT);
}

static void
test_daemon_config_without_wirelog_has_no_views (void)
{
    g_autofree char *dir = create_config_fixture_dir ();
    g_autofree char *config_path = write_config_fixture (dir,
            "[daemon]\n" "socket_path=/run/wyrebox/wyrebox.sock\n", 0600);
    g_autoptr (GError) error = NULL;
    g_autoptr (WyreboxDaemonConfig) config = NULL;

    config = wyrebox_daemon_config_new_from_file (config_path, &error);
    g_assert_no_error (error);
    g_assert_null (wyrebox_daemon_config_get_wirelog_rules_path (config));
    g_assert_null (wyrebox_daemon_config_get_extraction_rules_path (config));
    g_assert_cmpuint (wyrebox_daemon_config_get_n_views (config), ==, 0);
}

static void
assert_wirelog_config_rejected (const char *wirelog_sections,
    const char *expected_message)
{
    g_autofree char *dir = create_config_fixture_dir ();
    g_autofree char *contents = g_strconcat ("[daemon]\n"
            "socket_path=/run/wyrebox/wyrebox.sock\n", wirelog_sections, NULL);
    g_autofree char *config_path = write_config_fixture (dir, contents, 0600);
    g_autoptr (GError) error = NULL;

    g_assert_null (wyrebox_daemon_config_new_from_file (config_path, &error));
    g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    if (strstr (error->message, expected_message) == NULL)
        g_error ("expected '%s' in '%s'", expected_message, error->message);
}

static void
test_daemon_config_rejects_invalid_wirelog_views (void)
{
    assert_wirelog_config_rejected ("[wirelog]\nrules_path=views.dl\n",
        "[wirelog] rules_path must be absolute");
    assert_wirelog_config_rejected ("[view:projects]\nimap_name=Projects\n",
        "view sections require [wirelog] rules_path");
    assert_wirelog_config_rejected ("[wirelog]\nrules_path=/etc/v.dl\n"
        "[view:projects]\n", "view 'projects' is missing imap_name");
    assert_wirelog_config_rejected ("[wirelog]\nrules_path=/etc/v.dl\n"
        "[view:bad id]\nimap_name=Projects\n", "invalid view id 'bad id'");
    assert_wirelog_config_rejected ("[wirelog]\nrules_path=/etc/v.dl\n"
        "[view:projects]\nimap_name=Projects\nscope=message\n"
        "[view:projects]\nimap_name=Other\nscope=message\n",
        "defines view 'projects' more than once");
    assert_wirelog_config_rejected ("[wirelog]\nrules_path=/etc/v.dl\n"
        "[view:a]\nimap_name=Projects\nscope=message\n"
        "[view:b]\nimap_name=Projects\nscope=message\n",
        "share imap_name 'Projects'");
    assert_wirelog_config_rejected ("[wirelog]\nrules_path=/etc/v.dl\n"
        "[view:a]\nimap_name=INBOX\nscope=message\n",
        "view 'a' has invalid imap_name");
    assert_wirelog_config_rejected ("[wirelog]\nrules_path=/etc/v.dl\n"
        "[view:a]\nimap_name=Projects\n", "view 'a' is missing scope");
    assert_wirelog_config_rejected ("[wirelog]\nrules_path=/etc/v.dl\n"
        "[view:a]\nimap_name=Projects\nscope=mailbox\n",
        "view 'a' has invalid scope 'mailbox': expected message, thread, or "
        "account");
    assert_wirelog_config_rejected ("[wirelog]\nrules_path=/etc/v.dl\n"
        "[view:a]\nimap_name=Projects\nscope=message\nscope=thread\n",
        "more than once");
    assert_wirelog_config_rejected ("[wirelog]\nrules_path=/etc/v.dl\n"
        "relation=show\n", "unknown key 'relation'");
    assert_wirelog_config_rejected ("[wirelog]\nrules_path=/etc/v.dl\n"
        "extraction_rules_path=extraction.rules\n",
        "[wirelog] extraction_rules_path must be absolute");
    assert_wirelog_config_rejected ("[wirelog]\n"
        "extraction_rules_path=/etc/extraction.rules\n",
        "[wirelog] extraction_rules_path requires rules_path");
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);

    g_test_add_func ("/daemon-api/config/loads-canonical-config",
        test_daemon_config_loads_canonical_config);
    g_test_add_func
        ("/daemon-api/config/validate-for-startup-accepts-absolute-socket-path",
        test_daemon_config_validate_for_startup_accepts_absolute_socket_path);
    g_test_add_func
        ("/daemon-api/config/validate-for-startup-rejects-null-config",
        test_daemon_config_validate_for_startup_rejects_null_config);
    g_test_add_func ("/daemon-api/config/rejects-empty-socket-path",
        test_daemon_config_rejects_empty_socket_path);
    g_test_add_func ("/daemon-api/config/rejects-relative-socket-path",
        test_daemon_config_rejects_relative_socket_path);
    g_test_add_func
        ("/daemon-api/config/accepts-non-canonical-absolute-socket-path",
        test_daemon_config_accepts_non_canonical_absolute_socket_path);
    g_test_add_func ("/daemon-api/config/rejects-unknown-keys",
        test_daemon_config_rejects_unknown_keys);
    g_test_add_func ("/daemon-api/config/rejects-missing-socket-path",
        test_daemon_config_rejects_missing_socket_path);
    g_test_add_func ("/daemon-api/config/rejects-relative-catalog-path",
        test_daemon_config_rejects_relative_catalog_path);
    g_test_add_func ("/daemon-api/config/rejects-insecure-permissions",
        test_daemon_config_rejects_insecure_permissions);
    g_test_add_func ("/daemon-api/config/loads-wirelog-views",
        test_daemon_config_loads_wirelog_views);
    g_test_add_func ("/daemon-api/config/without-wirelog-has-no-views",
        test_daemon_config_without_wirelog_has_no_views);
    g_test_add_func ("/daemon-api/config/rejects-invalid-wirelog-views",
        test_daemon_config_rejects_invalid_wirelog_views);
    g_test_add_func ("/daemon-api/config/rejects-malformed-assignment",
        test_daemon_config_rejects_malformed_assignment);

    return g_test_run ();
}
