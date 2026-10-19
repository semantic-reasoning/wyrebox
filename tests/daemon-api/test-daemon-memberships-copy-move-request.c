#include "wyrebox-daemon-memberships-copy-move-request.h"

#include <gio/gio.h>

static void
test_request_init_copies_fields (void)
{
    guint64 uids[] = { 1, 3 };
    g_autoptr (GError) error = NULL;
    g_auto (WyreboxDaemonMembershipsCopyMoveRequest) request = { 0 };

    g_assert_true (wyrebox_daemon_memberships_copy_move_request_init (&request,
        "account-1", "inbox:account-1",
        WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_ORDINARY, 77, "archive:account-1",
        88, uids, G_N_ELEMENTS (uids), &error));
    g_assert_no_error (error);
    g_assert_cmpstr (request.source_mailbox_id, ==, "inbox:account-1");
    g_assert_cmpstr (request.dest_mailbox_id, ==, "archive:account-1");
    g_assert_cmpuint (request.source_uid_validity, ==, 77);
    g_assert_cmpuint (request.dest_uid_validity, ==, 88);
    g_assert_cmpuint (request.n_source_uids, ==, 2);
}

static void
test_request_init_rejects_oversized_uid_list (void)
{
    g_autofree guint64 *uids = g_new (guint64,
            WYREBOX_DAEMON_MEMBERSHIPS_COPY_MOVE_MAX_UIDS + 1);
    g_autoptr (GError) error = NULL;
    g_auto (WyreboxDaemonMembershipsCopyMoveRequest) request = { 0 };

    for (gsize i = 0; i < WYREBOX_DAEMON_MEMBERSHIPS_COPY_MOVE_MAX_UIDS + 1;
        i++)
        uids[i] = (guint64)(i + 1);

    g_assert_false (wyrebox_daemon_memberships_copy_move_request_init (&request,
        "account-1", "inbox:account-1",
        WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_ORDINARY, 1, "inbox:account-1", 1,
        uids, WYREBOX_DAEMON_MEMBERSHIPS_COPY_MOVE_MAX_UIDS + 1, &error));
    g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);

    g_test_add_func ("/daemon-api/memberships-copy-move-request/copies-fields",
        test_request_init_copies_fields);
    g_test_add_func ("/daemon-api/memberships-copy-move-request/"
        "rejects-oversized-uid-list",
        test_request_init_rejects_oversized_uid_list);

    return g_test_run ();
}
