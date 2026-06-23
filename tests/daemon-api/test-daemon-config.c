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
}

static void
test_daemon_config_loads_canonical_config (void)
{
  g_autofree char *dir = create_config_fixture_dir ();
  g_autofree char *config_path = write_config_fixture (dir,
      "[daemon]\n"
      "socket_path=/run/wyrebox/wyrebox.sock\n"
      "journal_root_dir=/var/lib/wyrebox/journal\n"
      "object_root_dir=/var/lib/wyrebox/object-store\n",
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
      "object_root_dir=/tmp/wyrebox-object-store\n",
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
      "object_root_dir=/tmp/wyrebox-object-store\n",
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
      "[daemon]\n" "socket_path=/run/wyrebox/wyrebox.sock\n" "unexpected=1\n",
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
  g_test_add_func ("/daemon-api/config/rejects-insecure-permissions",
      test_daemon_config_rejects_insecure_permissions);
  g_test_add_func ("/daemon-api/config/rejects-malformed-assignment",
      test_daemon_config_rejects_malformed_assignment);

  return g_test_run ();
}
