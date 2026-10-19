#include "wyrebox-daemon-memberships-expunge-dispatcher.h"

#include <gio/gio.h>

static gboolean
expunge_fixture (const WyreboxDaemonRequestIdentity *identity,
    const WyreboxDaemonMembershipsExpungeRequest *request,
    WyreboxDaemonSuccessReceipt *out_receipt, gpointer user_data,
    GError **error)
{
    gboolean *was_called = user_data;
    guint64 uids[] = { 1, 2 };

    (void)error;

    g_assert_cmpstr (identity->request_id, ==, "request-expunge");
    g_assert_cmpstr (identity->caller_identity, ==, "dovecot");
    g_assert_cmpuint (request->n_uids, ==, G_N_ELEMENTS (uids));
    g_assert_cmpuint (request->uids[0], ==, 1);

    if (was_called != NULL)
        *was_called = TRUE;

    out_receipt->request_id = g_strdup (identity->request_id);
    out_receipt->durable_marker = g_strdup ("journal:9:10");
    out_receipt->journal_offset = 9;
    out_receipt->journal_sequence = 10;
    out_receipt->summary = g_strdup ("memberships_expunge mailbox_id=mbox");
    return TRUE;
}

static void
test_dispatcher_handles_valid_envelope (void)
{
    guint64 uids[] = { 1, 2 };
    gboolean was_called = FALSE;
    g_autoptr (GError) error = NULL;
    g_autoptr (WyreboxDaemonMembershipsExpungeService) service = NULL;
    g_auto (WyreboxDaemonMembershipsExpungeRequest) request = { 0 };
    g_auto (WyreboxDaemonResponseFrame) frame = { 0 };

    g_assert_true (wyrebox_daemon_memberships_expunge_request_init (&request,
        "account-1", "mbox", 1, uids, G_N_ELEMENTS (uids), &error));
    g_assert_no_error (error);

    service = wyrebox_daemon_memberships_expunge_service_new (expunge_fixture,
            &was_called, NULL);

    g_assert_true (wyrebox_daemon_memberships_expunge_dispatch (service,
        "request-expunge", "dovecot", "account-1", "dovecot-storage",
        "imap-1", &request, &frame, &error));
    g_assert_no_error (error);
    g_assert_true (was_called);
    g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_SUCCESS);
}

static void
test_dispatcher_rejects_unauthorized_caller (void)
{
    guint64 uids[] = { 1 };
    gboolean was_called = FALSE;
    g_autoptr (GError) error = NULL;
    g_autoptr (WyreboxDaemonMembershipsExpungeService) service = NULL;
    g_auto (WyreboxDaemonMembershipsExpungeRequest) request = { 0 };
    g_auto (WyreboxDaemonResponseFrame) frame = { 0 };

    g_assert_true (wyrebox_daemon_memberships_expunge_request_init (&request,
        "account-1", "mbox", 1, uids, G_N_ELEMENTS (uids), &error));
    service = wyrebox_daemon_memberships_expunge_service_new (expunge_fixture,
            &was_called, NULL);

    g_assert_true (wyrebox_daemon_memberships_expunge_dispatch (service,
        "request-expunge", "skill", "account-1", "tool", "imap-1", &request,
        &frame, &error));
    g_assert_false (was_called);
    g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_ERROR);
    g_assert_cmpint (frame.error.error_class, ==,
        WYREBOX_DAEMON_ERROR_PERMISSION_DENIED);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);

    g_test_add_func ("/daemon-api/memberships-expunge-dispatcher/"
        "handles-valid-envelope", test_dispatcher_handles_valid_envelope);
    g_test_add_func ("/daemon-api/memberships-expunge-dispatcher/"
        "rejects-unauthorized-caller",
        test_dispatcher_rejects_unauthorized_caller);

    return g_test_run ();
}
