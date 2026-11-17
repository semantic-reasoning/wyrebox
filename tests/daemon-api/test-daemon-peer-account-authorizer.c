#include "wyrebox-daemon-config.h"
#include "wyrebox-daemon-peer-account-authorizer.h"

#include <gio/gio.h>
#include <glib.h>
#include <glib/gstdio.h>

#include <unistd.h>

static char *
write_config (const char *contents)
{
    g_autoptr (GError) error = NULL;
    g_autofree char *dir = g_dir_make_tmp (
        "wyrebox-peer-auth-XXXXXX", &error);
    g_autofree char *path = NULL;

    g_assert_no_error (error);
    path = g_build_filename (dir, "wyrebox.conf", NULL);
    g_assert_true (g_file_set_contents (path, contents, -1, &error));
    g_assert_no_error (error);
    g_assert_cmpint (g_chmod (path, 0600), ==, 0);
    return g_steal_pointer (&path);
}

static WyreboxDaemonConfig *
load_config (const char *accounts_line)
{
    g_autoptr (GError) error = NULL;
    g_autofree char *contents = g_strdup_printf (
        "[daemon]\n"
        "socket_path=/run/wyrebox/wyrebox.sock\n"
        "[peer-account-binding:%u]\n"
        "%s\n",
        (unsigned int)getuid (), accounts_line);
    g_autofree char *path = write_config (contents);
    WyreboxDaemonConfig *config =
        wyrebox_daemon_config_new_from_file (path, &error);

    g_assert_no_error (error);
    g_assert_nonnull (config);
    return config;
}

static void
test_allows_bound_account (void)
{
    g_autoptr (WyreboxDaemonConfig) config = load_config (
        "accounts=account-1,account-2");
    g_autoptr (WyreboxDaemonPeerAccountAuthorizer) authorizer =
        wyrebox_daemon_peer_account_authorizer_new (config);
    g_autoptr (GError) error = NULL;

    g_assert_true (wyrebox_daemon_peer_account_authorizer_allows (authorizer,
        getuid (), "account-1", &error));
    g_assert_no_error (error);
}

static void
test_denies_unbound_account (void)
{
    g_autoptr (WyreboxDaemonConfig) config = load_config (
        "accounts=account-1");
    g_autoptr (WyreboxDaemonPeerAccountAuthorizer) authorizer =
        wyrebox_daemon_peer_account_authorizer_new (config);
    g_autoptr (GError) error = NULL;

    g_assert_false (wyrebox_daemon_peer_account_authorizer_allows (authorizer,
        getuid (), "account-2", &error));
    g_assert_error (error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
}

static void
test_denies_unknown_uid (void)
{
    g_autoptr (WyreboxDaemonConfig) config = load_config (
        "accounts=account-1");
    g_autoptr (WyreboxDaemonPeerAccountAuthorizer) authorizer =
        wyrebox_daemon_peer_account_authorizer_new (config);
    g_autoptr (GError) error = NULL;
    uid_t other = getuid () == 0 ? 1 : 0;

    g_assert_false (wyrebox_daemon_peer_account_authorizer_allows (authorizer,
        other, "account-1", &error));
    g_assert_error (error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
}

static void
test_star_allows_any_account (void)
{
    g_autoptr (WyreboxDaemonConfig) config = load_config ("accounts=*");
    g_autoptr (WyreboxDaemonPeerAccountAuthorizer) authorizer =
        wyrebox_daemon_peer_account_authorizer_new (config);
    g_autoptr (GError) error = NULL;

    g_assert_true (wyrebox_daemon_peer_account_authorizer_allows (authorizer,
        getuid (), "any-account", &error));
    g_assert_no_error (error);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);
    g_test_add_func ("/daemon-api/peer-account-authorizer/allows-bound",
        test_allows_bound_account);
    g_test_add_func ("/daemon-api/peer-account-authorizer/denies-unbound",
        test_denies_unbound_account);
    g_test_add_func ("/daemon-api/peer-account-authorizer/denies-unknown-uid",
        test_denies_unknown_uid);
    g_test_add_func ("/daemon-api/peer-account-authorizer/star-allows-any",
        test_star_allows_any_account);
    return g_test_run ();
}
