#include "wyrebox-daemon-memberships-copy-move-dispatcher.h"

#include <gio/gio.h>

static gboolean
copy_move_fixture (const WyreboxDaemonRequestIdentity *identity,
    gboolean is_move, const WyreboxDaemonMembershipsCopyMoveRequest *request,
    WyreboxDaemonSuccessReceipt *out_receipt, gpointer user_data,
    GError **error)
{
    gboolean *was_called = user_data;

    (void)identity;
    (void)is_move;
    (void)request;
    (void)error;

    *was_called = TRUE;
    wyrebox_daemon_success_receipt_clear (out_receipt);
    out_receipt->request_id = g_strdup ("request-copy");
    out_receipt->durable_marker = g_strdup ("journal:1:1");
    out_receipt->journal_offset = 1;
    out_receipt->journal_sequence = 1;
    out_receipt->summary = g_strdup ("memberships_copied pairs=1:3");
    return TRUE;
}

static void
test_dispatcher_routes_copy (void)
{
    guint64 uids[] = { 1 };
    gboolean was_called = FALSE;
    g_autoptr (GError) error = NULL;
    g_autoptr (WyreboxDaemonMembershipsCopyMoveService) service = NULL;
    g_auto (WyreboxDaemonMembershipsCopyMoveRequest) request = { 0 };
    g_auto (WyreboxDaemonResponseFrame) frame = { 0 };

    g_assert_true (wyrebox_daemon_memberships_copy_move_request_init (&request,
        "account-1", "inbox:account-1",
        WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_ORDINARY, 1, "inbox:account-1", 1,
        uids, G_N_ELEMENTS (uids), &error));
    service =
        wyrebox_daemon_memberships_copy_move_service_new (copy_move_fixture,
            &was_called, NULL);
    g_assert_true (wyrebox_daemon_memberships_copy_dispatch (service,
        "request-copy", "dovecot", "account-1", "dovecot-storage", "imap-1",
        &request, &frame, &error));
    g_assert_true (was_called);
    g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_SUCCESS);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);

    g_test_add_func ("/daemon-api/memberships-copy-move-dispatcher/routes-copy",
        test_dispatcher_routes_copy);

    return g_test_run ();
}
