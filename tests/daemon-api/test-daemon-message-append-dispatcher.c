#include "wyrebox-daemon-message-append-dispatcher.h"

#include <gio/gio.h>

static gboolean
append_fixture (const WyreboxDaemonRequestIdentity *identity,
    const WyreboxDaemonMessageAppendRequest *request,
    WyreboxDaemonSuccessReceipt *out_receipt, gpointer user_data,
    GError **error)
{
    gboolean *was_called = user_data;

    (void)request;
    (void)error;

    if (was_called != NULL)
        *was_called = TRUE;

    out_receipt->request_id = g_strdup (identity->request_id);
    out_receipt->durable_marker = g_strdup ("journal:1:2");
    out_receipt->journal_offset = 1;
    out_receipt->journal_sequence = 2;
    out_receipt->summary = g_strdup ("message_appended pairs=1:3");
    return TRUE;
}

static void
test_dispatcher_handles_valid_envelope (void)
{
    gboolean was_called = FALSE;
    g_autoptr (GBytes) message = g_bytes_new_static ("a", 1);
    g_autoptr (GError) error = NULL;
    g_autoptr (WyreboxDaemonMessageAppendService) service = NULL;
    g_auto (WyreboxDaemonMessageAppendRequest) request = { 0 };
    g_auto (WyreboxDaemonResponseFrame) frame = { 0 };

    g_assert_true (wyrebox_daemon_message_append_request_init (&request,
        "account-1", "inbox:account-1", 1, message, 0, NULL, NULL, &error));
    service = wyrebox_daemon_message_append_service_new (append_fixture,
            &was_called, NULL);

    g_assert_true (wyrebox_daemon_message_append_dispatch (service,
        "request-append", "dovecot", "account-1", "dovecot-storage", "imap-1",
        &request, &frame, &error));
    g_assert_no_error (error);
    g_assert_true (was_called);
    g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_SUCCESS);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);

    g_test_add_func ("/daemon-api/message-append/dispatcher/"
        "handles-valid-envelope", test_dispatcher_handles_valid_envelope);

    return g_test_run ();
}
