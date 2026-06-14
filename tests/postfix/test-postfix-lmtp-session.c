#include "wyrebox-postfix-lmtp-session.h"

#include <gio/gio.h>
#include <string.h>

static GInputStream *
stream_from_text (const char *text)
{
  g_autoptr (GBytes) bytes = g_bytes_new_static (text, strlen (text));

  return g_memory_input_stream_new_from_bytes (bytes);
}

static gboolean
build_with_conversation (const char *const *argv,
    const char *conversation,
    WyreboxPostfixLmtpOptions *options,
    WyreboxDaemonRequestIdentity *identity,
    WyreboxDaemonDeliveryIngestionRequest *request, GError **error)
{
  int argc = 0;
  g_autoptr (GInputStream) input = NULL;

  while (argv[argc] != NULL)
    argc++;

  input = stream_from_text (conversation);
  return wyrebox_postfix_lmtp_session_build (argc,
      argv, input, G_MAXSIZE, options, identity, request, error);
}

static gboolean
build_with_conversation_and_limit (const char *const *argv,
    const char *conversation,
    gsize max_message_bytes,
    WyreboxPostfixLmtpOptions *options,
    WyreboxDaemonRequestIdentity *identity,
    WyreboxDaemonDeliveryIngestionRequest *request, GError **error)
{
  int argc = 0;
  g_autoptr (GInputStream) input = NULL;

  while (argv[argc] != NULL)
    argc++;

  input = stream_from_text (conversation);
  return wyrebox_postfix_lmtp_session_build (argc,
      argv, input, max_message_bytes, options, identity, request, error);
}

static void
assert_message_bytes (GBytes *bytes, const guint8 *expected, gsize size)
{
  const guint8 *actual = NULL;
  gsize actual_size = 0;

  g_assert_nonnull (bytes);
  actual = g_bytes_get_data (bytes, &actual_size);
  g_assert_cmpuint (actual_size, ==, size);
  g_assert_cmpmem (actual, actual_size, expected, size);
}

static void
test_single_recipient_builds_delivery_request (void)
{
  const char *argv[] = {
    "wyrebox-postfix-lmtp",
    "--account-id", "account-1",
    "--delivery-id", "stable-delivery-1",
    NULL
  };
  const char *conversation =
      "LHLO postfix.example\r\n"
      "MAIL FROM:<sender@example.com>\r\n"
      "RCPT TO:<alice@example.com>\r\n"
      "DATA\r\n"
      "From: sender@example.com\r\n" "\r\n" "hello\r\n" ".\r\n" "QUIT\r\n";
  const guint8 expected_message[] = "From: sender@example.com\r\n\r\nhello\r\n";
  g_auto (WyreboxPostfixLmtpOptions) options = { 0 };
  g_auto (WyreboxDaemonRequestIdentity) identity = { 0 };
  g_auto (WyreboxDaemonDeliveryIngestionRequest) request = { 0 };
  g_autoptr (GError) error = NULL;

  g_assert_true (build_with_conversation (argv,
          conversation, &options, &identity, &request, &error));
  g_assert_no_error (error);

  g_assert_cmpstr (options.socket_path, ==,
      WYREBOX_POSTFIX_LMTP_DEFAULT_SOCKET_PATH);
  g_assert_cmpstr (identity.request_id, ==, "postfix-lmtp:stable-delivery-1");
  g_assert_cmpstr (identity.caller_identity, ==, "postfix");
  g_assert_cmpstr (identity.account_identity, ==, "account-1");
  g_assert_cmpstr (identity.tool_identity, ==, "wyrebox-postfix-lmtp");
  g_assert_cmpstr (identity.correlation_id, ==,
      "postfix-lmtp:stable-delivery-1");
  g_assert_cmpstr (request.delivery_id, ==, "stable-delivery-1");
  g_assert_null (request.queue_id);
  g_assert_cmpstr (request.envelope_sender, ==, "sender@example.com");
  g_assert_cmpstr (request.recipients[0], ==, "alice@example.com");
  g_assert_null (request.recipients[1]);
  assert_message_bytes (request.message_bytes,
      expected_message, sizeof (expected_message) - 1);
}

static void
test_dot_stuffed_data_is_unstuffed_without_lf_normalization (void)
{
  const char *argv[] = {
    "wyrebox-postfix-lmtp",
    "--account-id", "account-1",
    "--delivery-id", "stable-delivery-1",
    NULL
  };
  const char *conversation =
      "LHLO postfix.example\r\n"
      "MAIL FROM:<>\r\n"
      "RCPT TO:<alice@example.com>\r\n"
      "DATA\r\n" ".leading dot\r\n" "..two leading dots\r\n" "\r\n" ".\r\n";
  const guint8 expected_message[] = "leading dot\r\n.two leading dots\r\n\r\n";
  g_auto (WyreboxPostfixLmtpOptions) options = { 0 };
  g_auto (WyreboxDaemonRequestIdentity) identity = { 0 };
  g_auto (WyreboxDaemonDeliveryIngestionRequest) request = { 0 };
  g_autoptr (GError) error = NULL;

  g_assert_true (build_with_conversation (argv,
          conversation, &options, &identity, &request, &error));
  g_assert_no_error (error);
  g_assert_cmpstr (request.envelope_sender, ==, "");
  assert_message_bytes (request.message_bytes,
      expected_message, sizeof (expected_message) - 1);
}

static void
test_multiple_recipients_fail_closed_for_first_slice (void)
{
  const char *argv[] = {
    "wyrebox-postfix-lmtp",
    "--account-id", "account-1",
    "--delivery-id", "stable-delivery-1",
    NULL
  };
  const char *conversation =
      "LHLO postfix.example\r\n"
      "MAIL FROM:<sender@example.com>\r\n"
      "RCPT TO:<alice@example.com>\r\n"
      "RCPT TO:<bob@example.com>\r\n"
      "DATA\r\n" "Subject: hi\r\n" "\r\n" "body\r\n" ".\r\n";
  g_auto (WyreboxPostfixLmtpOptions) options = { 0 };
  g_auto (WyreboxDaemonRequestIdentity) identity = { 0 };
  g_auto (WyreboxDaemonDeliveryIngestionRequest) request = { 0 };
  g_autoptr (GError) error = NULL;

  g_assert_false (build_with_conversation (argv,
          conversation, &options, &identity, &request, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_null (identity.request_id);
  g_assert_null (request.message_bytes);
}

static void
test_missing_stable_delivery_id_fails_closed (void)
{
  const char *argv[] = {
    "wyrebox-postfix-lmtp",
    "--account-id", "account-1",
    NULL
  };
  const char *conversation =
      "LHLO postfix.example\r\n"
      "MAIL FROM:<sender@example.com>\r\n"
      "RCPT TO:<alice@example.com>\r\n"
      "DATA\r\n" "Subject: hi\r\n" "\r\n" "body\r\n" ".\r\n";
  g_auto (WyreboxPostfixLmtpOptions) options = { 0 };
  g_auto (WyreboxDaemonRequestIdentity) identity = { 0 };
  g_auto (WyreboxDaemonDeliveryIngestionRequest) request = { 0 };
  g_autoptr (GError) error = NULL;

  g_assert_false (build_with_conversation (argv,
          conversation, &options, &identity, &request, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_null (identity.request_id);
  g_assert_null (request.message_bytes);
}

static void
test_bad_command_order_fails (void)
{
  const char *argv[] = {
    "wyrebox-postfix-lmtp",
    "--account-id", "account-1",
    "--delivery-id", "stable-delivery-1",
    NULL
  };
  const char *conversation =
      "LHLO postfix.example\r\n" "DATA\r\n" "Subject: hi\r\n" ".\r\n";
  g_auto (WyreboxPostfixLmtpOptions) options = { 0 };
  g_auto (WyreboxDaemonRequestIdentity) identity = { 0 };
  g_auto (WyreboxDaemonDeliveryIngestionRequest) request = { 0 };
  g_autoptr (GError) error = NULL;

  g_assert_false (build_with_conversation (argv,
          conversation, &options, &identity, &request, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
}

static void
test_truncated_data_fails (void)
{
  const char *argv[] = {
    "wyrebox-postfix-lmtp",
    "--account-id", "account-1",
    "--delivery-id", "stable-delivery-1",
    NULL
  };
  const char *conversation =
      "LHLO postfix.example\r\n"
      "MAIL FROM:<sender@example.com>\r\n"
      "RCPT TO:<alice@example.com>\r\n" "DATA\r\n" "Subject: hi\r\n";
  g_auto (WyreboxPostfixLmtpOptions) options = { 0 };
  g_auto (WyreboxDaemonRequestIdentity) identity = { 0 };
  g_auto (WyreboxDaemonDeliveryIngestionRequest) request = { 0 };
  g_autoptr (GError) error = NULL;

  g_assert_false (build_with_conversation (argv,
          conversation, &options, &identity, &request, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
}

static void
test_oversize_data_fails (void)
{
  const char *argv[] = {
    "wyrebox-postfix-lmtp",
    "--account-id", "account-1",
    "--delivery-id", "stable-delivery-1",
    NULL
  };
  const char *conversation =
      "LHLO postfix.example\r\n"
      "MAIL FROM:<sender@example.com>\r\n"
      "RCPT TO:<alice@example.com>\r\n" "DATA\r\n" "abcd\r\n" ".\r\n";
  g_auto (WyreboxPostfixLmtpOptions) options = { 0 };
  g_auto (WyreboxDaemonRequestIdentity) identity = { 0 };
  g_auto (WyreboxDaemonDeliveryIngestionRequest) request = { 0 };
  g_autoptr (GError) error = NULL;

  g_assert_false (build_with_conversation_and_limit (argv,
          conversation, 5, &options, &identity, &request, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_null (identity.request_id);
  g_assert_null (request.message_bytes);
}

static void
test_exact_message_limit_is_accepted (void)
{
  const char *argv[] = {
    "wyrebox-postfix-lmtp",
    "--account-id", "account-1",
    "--delivery-id", "stable-delivery-1",
    NULL
  };
  const char *conversation =
      "LHLO postfix.example\r\n"
      "MAIL FROM:<sender@example.com>\r\n"
      "RCPT TO:<alice@example.com>\r\n" "DATA\r\n" "abcd\r\n" ".\r\n";
  const guint8 expected_message[] = "abcd\r\n";
  g_auto (WyreboxPostfixLmtpOptions) options = { 0 };
  g_auto (WyreboxDaemonRequestIdentity) identity = { 0 };
  g_auto (WyreboxDaemonDeliveryIngestionRequest) request = { 0 };
  g_autoptr (GError) error = NULL;

  g_assert_cmpuint (strlen (conversation), >, sizeof (expected_message) - 1);
  g_assert_true (build_with_conversation_and_limit (argv,
          conversation, sizeof (expected_message) - 1,
          &options, &identity, &request, &error));
  g_assert_no_error (error);
  assert_message_bytes (request.message_bytes,
      expected_message, sizeof (expected_message) - 1);
}

static void
test_unterminated_oversize_data_line_fails_before_full_line_buffering (void)
{
  const char *argv[] = {
    "wyrebox-postfix-lmtp",
    "--account-id", "account-1",
    "--delivery-id", "stable-delivery-1",
    NULL
  };
  const char *conversation =
      "LHLO postfix.example\r\n"
      "MAIL FROM:<sender@example.com>\r\n"
      "RCPT TO:<alice@example.com>\r\n" "DATA\r\n" "abcdef";
  g_auto (WyreboxPostfixLmtpOptions) options = { 0 };
  g_auto (WyreboxDaemonRequestIdentity) identity = { 0 };
  g_auto (WyreboxDaemonDeliveryIngestionRequest) request = { 0 };
  g_autoptr (GError) error = NULL;

  g_assert_false (build_with_conversation_and_limit (argv,
          conversation, 5, &options, &identity, &request, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_nonnull (strstr (error->message, "exceeds maximum size"));
  g_assert_null (identity.request_id);
  g_assert_null (request.message_bytes);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/postfix/lmtp-session/single-recipient-builds-request",
      test_single_recipient_builds_delivery_request);
  g_test_add_func ("/postfix/lmtp-session/dot-stuffed-data",
      test_dot_stuffed_data_is_unstuffed_without_lf_normalization);
  g_test_add_func ("/postfix/lmtp-session/multiple-recipients-fail-closed",
      test_multiple_recipients_fail_closed_for_first_slice);
  g_test_add_func ("/postfix/lmtp-session/missing-delivery-id-fails",
      test_missing_stable_delivery_id_fails_closed);
  g_test_add_func ("/postfix/lmtp-session/bad-command-order-fails",
      test_bad_command_order_fails);
  g_test_add_func ("/postfix/lmtp-session/truncated-data-fails",
      test_truncated_data_fails);
  g_test_add_func ("/postfix/lmtp-session/oversize-data-fails",
      test_oversize_data_fails);
  g_test_add_func ("/postfix/lmtp-session/exact-message-limit",
      test_exact_message_limit_is_accepted);
  g_test_add_func ("/postfix/lmtp-session/unterminated-oversize-data-line",
      test_unterminated_oversize_data_line_fails_before_full_line_buffering);

  return g_test_run ();
}
