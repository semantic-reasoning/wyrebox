#include "wyrebox-daemon-message-append-request.h"

#include <gio/gio.h>

static GBytes *
one_byte_message (void)
{
    return g_bytes_new_static ("a", 1);
}

static void
test_request_init_copies_fields (void)
{
    const char *system_flags[] = { "\\Seen", NULL };
    const char *user_keywords[] = { "project", NULL };
    g_autoptr (GBytes) message = one_byte_message ();
    g_autoptr (GError) error = NULL;
    g_auto (WyreboxDaemonMessageAppendRequest) request = { 0 };

    g_assert_true (wyrebox_daemon_message_append_request_init (&request,
        "account-1", "inbox:account-1", 1, message, 42, system_flags,
        user_keywords, &error));
    g_assert_no_error (error);
    g_assert_cmpstr (request.account_identity, ==, "account-1");
    g_assert_cmpstr (request.mailbox_id, ==, "inbox:account-1");
    g_assert_cmpuint (request.uid_validity, ==, 1);
    g_assert_cmpuint (request.internal_date_unix_us, ==, 42);
    g_assert_cmpuint (g_bytes_get_size (request.message_bytes), ==, 1);
    g_assert_cmpstr (request.system_flags[0], ==, "\\Seen");
    g_assert_cmpstr (request.user_keywords[0], ==, "project");
}

static void
test_request_init_rejects_empty_body (void)
{
    g_autoptr (GBytes) message = g_bytes_new_static ("", 0);
    g_autoptr (GError) error = NULL;
    g_auto (WyreboxDaemonMessageAppendRequest) request = { 0 };

    g_assert_false (wyrebox_daemon_message_append_request_init (&request,
        "account-1", "inbox:account-1", 1, message, 0, NULL, NULL, &error));
    g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
}

static void
test_request_init_rejects_oversized_body (void)
{
    g_autoptr (GBytes) message = NULL;
    g_autoptr (GError) error = NULL;
    g_auto (WyreboxDaemonMessageAppendRequest) request = { 0 };
    gsize size = WYREBOX_DAEMON_MESSAGE_APPEND_MAX_MESSAGE_BYTES + 1;
    guint8 *data = g_malloc0 (size);

    message = g_bytes_new_take (data, size);
    g_assert_false (wyrebox_daemon_message_append_request_init (&request,
        "account-1", "inbox:account-1", 1, message, 0, NULL, NULL, &error));
    g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);

    g_test_add_func ("/daemon-api/message-append-request/copies-fields",
        test_request_init_copies_fields);
    g_test_add_func ("/daemon-api/message-append-request/rejects-empty-body",
        test_request_init_rejects_empty_body);
    g_test_add_func (
        "/daemon-api/message-append-request/rejects-oversized-body",
        test_request_init_rejects_oversized_body);

    return g_test_run ();
}
