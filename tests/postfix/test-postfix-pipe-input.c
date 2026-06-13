#include "wyrebox-postfix-pipe-input.h"

#include <gio/gio.h>
#include <string.h>

static GInputStream *
stream_from_data (const guint8 *data, gsize size)
{
  g_autoptr (GBytes) bytes = g_bytes_new_static (data, size);

  return g_memory_input_stream_new_from_bytes (bytes);
}

static void
assert_bytes_equal (GBytes *bytes, const guint8 *expected, gsize size)
{
  gsize actual_size = 0;
  const guint8 *actual = g_bytes_get_data (bytes, &actual_size);

  g_assert_cmpuint (actual_size, ==, size);
  g_assert_cmpmem (actual, actual_size, expected, size);
}

static gboolean
build_with_message (const char *const *argv,
    const guint8 *message,
    gsize message_size,
    gsize max_message_bytes,
    WyreboxPostfixPipeOptions *options,
    WyreboxDaemonRequestIdentity *identity,
    WyreboxDaemonDeliveryIngestionRequest *request, GError **error)
{
  int argc = 0;
  g_autoptr (GInputStream) input = NULL;

  while (argv[argc] != NULL)
    argc++;

  input = stream_from_data (message, message_size);
  return wyrebox_postfix_pipe_input_build (argc,
      argv, input, max_message_bytes, options, identity, request, error);
}

static void
test_valid_input_preserves_exact_bytes_and_metadata (void)
{
  const char *argv[] = {
    "wyrebox-postfix-pipe",
    "--account-id", "account-1",
    "--delivery-id", "delivery-1",
    "--queue-id", "queue-1",
    "--sender", "sender@example.com",
    "--recipient", "alice@example.com",
    NULL
  };
  const guint8 message[] = {
    'F', 'r', 'o', 'm', ':', ' ', 'a', '\r', '\n',
    'X', '-', 'N', 'u', 'l', ':', ' ', 'y', '\r', '\n',
    '\r', '\n',
    'a', '\0', 'b', '\r', '\n'
  };
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxPostfixPipeOptions) options = { 0 };
  g_auto (WyreboxDaemonRequestIdentity) identity = { 0 };
  g_auto (WyreboxDaemonDeliveryIngestionRequest) request = { 0 };

  g_assert_true (build_with_message (argv,
          message,
          sizeof (message), 4096, &options, &identity, &request, &error));
  g_assert_no_error (error);

  g_assert_cmpstr (options.socket_path, ==,
      WYREBOX_POSTFIX_PIPE_DEFAULT_SOCKET_PATH);
  g_assert_cmpstr (identity.request_id, ==, "postfix:queue-1:delivery-1");
  g_assert_cmpstr (identity.caller_identity, ==, "postfix");
  g_assert_cmpstr (identity.account_identity, ==, "account-1");
  g_assert_cmpstr (identity.tool_identity, ==, "wyrebox-postfix-pipe");
  g_assert_cmpstr (identity.correlation_id, ==, "postfix:queue-1:delivery-1");
  g_assert_cmpstr (request.delivery_id, ==, "delivery-1");
  g_assert_cmpstr (request.queue_id, ==, "queue-1");
  g_assert_cmpstr (request.envelope_sender, ==, "sender@example.com");
  g_assert_cmpstr (request.recipients[0], ==, "alice@example.com");
  g_assert_null (request.recipients[1]);
  assert_bytes_equal (request.message_bytes, message, sizeof (message));
}

static void
test_repeated_recipients_preserve_order (void)
{
  const char *argv[] = {
    "wyrebox-postfix-pipe",
    "--account-id", "account-1",
    "--delivery-id", "delivery-1",
    "--queue-id", "queue-1",
    "--recipient", "alice@example.com",
    "--recipient", "bob@example.com",
    "--recipient", "carol@example.com",
    NULL
  };
  const guint8 message[] = "Subject: hi\r\n\r\nbody\r\n";
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxPostfixPipeOptions) options = { 0 };
  g_auto (WyreboxDaemonRequestIdentity) identity = { 0 };
  g_auto (WyreboxDaemonDeliveryIngestionRequest) request = { 0 };

  g_assert_true (build_with_message (argv,
          message,
          sizeof (message) - 1, 4096, &options, &identity, &request, &error));
  g_assert_no_error (error);

  g_assert_cmpstr (request.envelope_sender, ==, "");
  g_assert_cmpstr (request.recipients[0], ==, "alice@example.com");
  g_assert_cmpstr (request.recipients[1], ==, "bob@example.com");
  g_assert_cmpstr (request.recipients[2], ==, "carol@example.com");
  g_assert_null (request.recipients[3]);
}

static void
test_valid_input_without_queue_id_uses_delivery_fallback_id (void)
{
  const char *argv[] = {
    "wyrebox-postfix-pipe",
    "--account-id", "account-1",
    "--delivery-id", "delivery-1",
    "--recipient", "alice@example.com",
    NULL
  };
  const guint8 message[] = "Subject: hi\r\n\r\nbody\r\n";
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxPostfixPipeOptions) options = { 0 };
  g_auto (WyreboxDaemonRequestIdentity) identity = { 0 };
  g_auto (WyreboxDaemonDeliveryIngestionRequest) request = { 0 };

  g_assert_true (build_with_message (argv,
          message,
          sizeof (message) - 1, 4096, &options, &identity, &request, &error));
  g_assert_no_error (error);

  g_assert_cmpstr (identity.request_id, ==, "postfix:delivery-1");
  g_assert_cmpstr (identity.correlation_id, ==, "postfix:delivery-1");
  g_assert_null (request.queue_id);
}

static void
test_socket_default_and_override (void)
{
  const char *default_argv[] = {
    "wyrebox-postfix-pipe",
    "--account-id", "account-1",
    "--delivery-id", "delivery-1",
    "--queue-id", "queue-1",
    "--recipient", "alice@example.com",
    NULL
  };
  const char *override_argv[] = {
    "wyrebox-postfix-pipe",
    "--account-id", "account-1",
    "--delivery-id", "delivery-1",
    "--queue-id", "queue-1",
    "--recipient", "alice@example.com",
    "--socket", "/tmp/wyrebox.sock",
    NULL
  };
  const guint8 message[] = "Subject: hi\r\n\r\nbody\r\n";
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxPostfixPipeOptions) options = { 0 };
  g_auto (WyreboxDaemonRequestIdentity) identity = { 0 };
  g_auto (WyreboxDaemonDeliveryIngestionRequest) request = { 0 };

  g_assert_true (build_with_message (default_argv,
          message,
          sizeof (message) - 1, 4096, &options, &identity, &request, &error));
  g_assert_no_error (error);
  g_assert_cmpstr (options.socket_path, ==,
      WYREBOX_POSTFIX_PIPE_DEFAULT_SOCKET_PATH);

  g_assert_true (build_with_message (override_argv,
          message,
          sizeof (message) - 1, 4096, &options, &identity, &request, &error));
  g_assert_no_error (error);
  g_assert_cmpstr (options.socket_path, ==, "/tmp/wyrebox.sock");
}

static void
assert_failure (const char *const *argv,
    const guint8 *message,
    gsize message_size,
    gsize max_message_bytes,
    WyreboxPostfixPipeOptions *options,
    WyreboxDaemonRequestIdentity *identity,
    WyreboxDaemonDeliveryIngestionRequest *request)
{
  g_autoptr (GError) error = NULL;

  g_assert_false (build_with_message (argv,
          message,
          message_size, max_message_bytes, options, identity, request, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_null (options->socket_path);
  g_assert_null (identity->request_id);
  g_assert_null (request->delivery_id);
  g_assert_null (request->message_bytes);
}

static void
test_missing_required_options_fail (void)
{
  const guint8 message[] = "Subject: hi\r\n\r\nbody\r\n";
  const char *missing_account[] = {
    "wyrebox-postfix-pipe",
    "--delivery-id", "delivery-1",
    "--queue-id", "queue-1",
    "--recipient", "alice@example.com",
    NULL
  };
  const char *missing_delivery[] = {
    "wyrebox-postfix-pipe",
    "--account-id", "account-1",
    "--queue-id", "queue-1",
    "--recipient", "alice@example.com",
    NULL
  };
  const char *missing_recipient[] = {
    "wyrebox-postfix-pipe",
    "--account-id", "account-1",
    "--delivery-id", "delivery-1",
    "--queue-id", "queue-1",
    NULL
  };
  g_auto (WyreboxPostfixPipeOptions) options = { 0 };
  g_auto (WyreboxDaemonRequestIdentity) identity = { 0 };
  g_auto (WyreboxDaemonDeliveryIngestionRequest) request = { 0 };

  assert_failure (missing_account,
      message, sizeof (message) - 1, 4096, &options, &identity, &request);
  assert_failure (missing_delivery,
      message, sizeof (message) - 1, 4096, &options, &identity, &request);
  assert_failure (missing_recipient,
      message, sizeof (message) - 1, 4096, &options, &identity, &request);
}

static void
test_empty_stdin_fails (void)
{
  const char *argv[] = {
    "wyrebox-postfix-pipe",
    "--account-id", "account-1",
    "--delivery-id", "delivery-1",
    "--queue-id", "queue-1",
    "--recipient", "alice@example.com",
    NULL
  };
  const guint8 message[] = "";
  g_auto (WyreboxPostfixPipeOptions) options = { 0 };
  g_auto (WyreboxDaemonRequestIdentity) identity = { 0 };
  g_auto (WyreboxDaemonDeliveryIngestionRequest) request = { 0 };

  assert_failure (argv, message, 0, 4096, &options, &identity, &request);
}

static void
test_oversize_stdin_fails (void)
{
  const char *argv[] = {
    "wyrebox-postfix-pipe",
    "--account-id", "account-1",
    "--delivery-id", "delivery-1",
    "--queue-id", "queue-1",
    "--recipient", "alice@example.com",
    NULL
  };
  const guint8 message[] = "123456";
  g_auto (WyreboxPostfixPipeOptions) options = { 0 };
  g_auto (WyreboxDaemonRequestIdentity) identity = { 0 };
  g_auto (WyreboxDaemonDeliveryIngestionRequest) request = { 0 };

  assert_failure (argv,
      message, sizeof (message) - 1, 5, &options, &identity, &request);
}

static void
test_max_message_bytes_gmaxsize_succeeds (void)
{
  const char *argv[] = {
    "wyrebox-postfix-pipe",
    "--account-id", "account-1",
    "--delivery-id", "delivery-1",
    "--queue-id", "queue-1",
    "--recipient", "alice@example.com",
    NULL
  };
  const guint8 message[] = "Subject: hi\r\n\r\nbody\r\n";
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxPostfixPipeOptions) options = { 0 };
  g_auto (WyreboxDaemonRequestIdentity) identity = { 0 };
  g_auto (WyreboxDaemonDeliveryIngestionRequest) request = { 0 };

  g_assert_true (build_with_message (argv,
          message,
          sizeof (message) - 1,
          G_MAXSIZE, &options, &identity, &request, &error));
  g_assert_no_error (error);
  assert_bytes_equal (request.message_bytes, message, sizeof (message) - 1);
}

static void
test_control_characters_are_rejected_by_request_validation (void)
{
  const char *bad_request_argv[] = {
    "wyrebox-postfix-pipe",
    "--account-id", "account-1",
    "--delivery-id", "delivery\n1",
    "--queue-id", "queue-1",
    "--recipient", "alice@example.com",
    NULL
  };
  const guint8 message[] = "Subject: hi\r\n\r\nbody\r\n";
  g_auto (WyreboxPostfixPipeOptions) options = { 0 };
  g_auto (WyreboxDaemonRequestIdentity) identity = { 0 };
  g_auto (WyreboxDaemonDeliveryIngestionRequest) request = { 0 };

  assert_failure (bad_request_argv,
      message, sizeof (message) - 1, 4096, &options, &identity, &request);
}

static void
test_unexpected_positional_argument_fails (void)
{
  const char *argv[] = {
    "wyrebox-postfix-pipe",
    "--account-id", "account-1",
    "--delivery-id", "delivery-1",
    "--recipient", "alice@example.com",
    "unexpected",
    NULL
  };
  const guint8 message[] = "Subject: hi\r\n\r\nbody\r\n";
  g_auto (WyreboxPostfixPipeOptions) options = { 0 };
  g_auto (WyreboxDaemonRequestIdentity) identity = { 0 };
  g_auto (WyreboxDaemonDeliveryIngestionRequest) request = { 0 };

  assert_failure (argv,
      message, sizeof (message) - 1, 4096, &options, &identity, &request);
}

static void
test_stale_outputs_are_cleared_on_failure (void)
{
  const char *good_argv[] = {
    "wyrebox-postfix-pipe",
    "--account-id", "account-1",
    "--delivery-id", "delivery-1",
    "--queue-id", "queue-1",
    "--recipient", "alice@example.com",
    NULL
  };
  const char *bad_argv[] = {
    "wyrebox-postfix-pipe",
    "--account-id", "account-1",
    "--delivery-id", "delivery-1",
    "--queue-id", "queue-1",
    NULL
  };
  const guint8 message[] = "Subject: hi\r\n\r\nbody\r\n";
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxPostfixPipeOptions) options = { 0 };
  g_auto (WyreboxDaemonRequestIdentity) identity = { 0 };
  g_auto (WyreboxDaemonDeliveryIngestionRequest) request = { 0 };

  g_assert_true (build_with_message (good_argv,
          message,
          sizeof (message) - 1, 4096, &options, &identity, &request, &error));
  g_assert_no_error (error);
  g_assert_nonnull (options.socket_path);
  g_assert_nonnull (identity.request_id);
  g_assert_nonnull (request.message_bytes);

  assert_failure (bad_argv,
      message, sizeof (message) - 1, 4096, &options, &identity, &request);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/postfix/pipe-input/valid-input-preserves-exact-bytes",
      test_valid_input_preserves_exact_bytes_and_metadata);
  g_test_add_func ("/postfix/pipe-input/repeated-recipients-preserve-order",
      test_repeated_recipients_preserve_order);
  g_test_add_func ("/postfix/pipe-input/without-queue-id-uses-fallback-id",
      test_valid_input_without_queue_id_uses_delivery_fallback_id);
  g_test_add_func ("/postfix/pipe-input/socket-default-and-override",
      test_socket_default_and_override);
  g_test_add_func ("/postfix/pipe-input/missing-required-options-fail",
      test_missing_required_options_fail);
  g_test_add_func ("/postfix/pipe-input/empty-stdin-fails",
      test_empty_stdin_fails);
  g_test_add_func ("/postfix/pipe-input/oversize-stdin-fails",
      test_oversize_stdin_fails);
  g_test_add_func ("/postfix/pipe-input/gmaxsize-limit-succeeds",
      test_max_message_bytes_gmaxsize_succeeds);
  g_test_add_func ("/postfix/pipe-input/control-characters-rejected",
      test_control_characters_are_rejected_by_request_validation);
  g_test_add_func ("/postfix/pipe-input/unexpected-positional-argument-fails",
      test_unexpected_positional_argument_fails);
  g_test_add_func ("/postfix/pipe-input/stale-outputs-cleared-on-failure",
      test_stale_outputs_are_cleared_on_failure);

  return g_test_run ();
}
