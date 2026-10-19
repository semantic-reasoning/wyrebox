#include "wyrebox-daemon-memberships-expunge-request.h"

#include <gio/gio.h>

static void
test_request_init_copies_fields (void)
{
    guint64 uids[] = { 1, 3, 5 };
    g_autoptr (GError) error = NULL;
    g_auto (WyreboxDaemonMembershipsExpungeRequest) request = { 0 };

    g_assert_true (wyrebox_daemon_memberships_expunge_request_init (&request,
        "account-1", "mailbox-inbox", 77, uids, G_N_ELEMENTS (uids), &error));
    g_assert_no_error (error);
    g_assert_cmpstr (request.account_identity, ==, "account-1");
    g_assert_cmpstr (request.mailbox_id, ==, "mailbox-inbox");
    g_assert_cmpuint (request.uid_validity, ==, 77);
    g_assert_cmpuint (request.n_uids, ==, 3);
    g_assert_cmpuint (request.uids[0], ==, 1);
    g_assert_cmpuint (request.uids[2], ==, 5);
}

static void
test_request_init_rejects_empty_uid_list (void)
{
    g_autoptr (GError) error = NULL;
    g_auto (WyreboxDaemonMembershipsExpungeRequest) request = { 0 };

    g_assert_false (wyrebox_daemon_memberships_expunge_request_init (&request,
        "account-1", "mailbox-inbox", 77, NULL, 0, &error));
    g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
}

static void
test_request_init_rejects_zero_uid (void)
{
    guint64 uids[] = { 0, 2 };
    g_autoptr (GError) error = NULL;
    g_auto (WyreboxDaemonMembershipsExpungeRequest) request = { 0 };

    g_assert_false (wyrebox_daemon_memberships_expunge_request_init (&request,
        "account-1", "mailbox-inbox", 77, uids, G_N_ELEMENTS (uids), &error));
    g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
}

static void
test_request_init_rejects_non_ascending_uids (void)
{
    guint64 uids[] = { 2, 2 };
    g_autoptr (GError) error = NULL;
    g_auto (WyreboxDaemonMembershipsExpungeRequest) request = { 0 };

    g_assert_false (wyrebox_daemon_memberships_expunge_request_init (&request,
        "account-1", "mailbox-inbox", 77, uids, G_N_ELEMENTS (uids), &error));
    g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);

    g_test_add_func ("/daemon-api/memberships-expunge-request/copies-fields",
        test_request_init_copies_fields);
    g_test_add_func ("/daemon-api/memberships-expunge-request/"
        "rejects-empty-uid-list", test_request_init_rejects_empty_uid_list);
    g_test_add_func ("/daemon-api/memberships-expunge-request/rejects-zero-uid",
        test_request_init_rejects_zero_uid);
    g_test_add_func ("/daemon-api/memberships-expunge-request/"
        "rejects-non-ascending-uids",
        test_request_init_rejects_non_ascending_uids);

    return g_test_run ();
}
