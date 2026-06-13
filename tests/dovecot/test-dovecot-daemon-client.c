#include "wyrebox-dovecot-daemon-client.h"

#include "wyrebox-build-config.h"
#include "wyrebox-daemon-capnp-codec.h"
#include "wyrebox-daemon-error-frame.h"
#include "wyrebox-daemon-fact-mutation-request.h"
#include "wyrebox-daemon-frame-io.h"
#include "wyrebox-daemon-mailbox-list-result.h"
#include "wyrebox-daemon-request-router.h"
#include "wyrebox-daemon-response-frame.h"

#include <gio/gio.h>
#include <gio/gunixsocketaddress.h>
#include <glib/gstdio.h>

#if defined(WYREBOX_HAVE_CAPNP_SERIALIZATION) && WYREBOX_HAVE_CAPNP_SERIALIZATION

typedef enum
{
  FAKE_SERVER_RESPONSE_BYTES,
  FAKE_SERVER_MAILBOX_SELECT_RESPONSE,
  FAKE_SERVER_DAEMON_ERROR_RESPONSE,
  FAKE_SERVER_UNEXPECTED_SUCCESS_RESPONSE,
  FAKE_SERVER_CLOSE_WITHOUT_RESPONSE,
  FAKE_SERVER_TRUNCATED_RESPONSE,
} FakeServerBehavior;

typedef struct
{
  GSocketListener *listener;
  GThread *thread;
  FakeServerBehavior behavior;
  const guint8 *response;
  gsize response_size;
  const char *response_request_id_override;
} FakeServer;

static void
remove_tree (const char *path)
{
  g_autoptr (GDir) dir = NULL;
  const char *entry = NULL;

  dir = g_dir_open (path, 0, NULL);
  if (dir == NULL) {
    (void) g_remove (path);
    return;
  }

  while ((entry = g_dir_read_name (dir)) != NULL) {
    g_autofree char *child = g_build_filename (path, entry, NULL);

    remove_tree (child);
  }

  (void) g_rmdir (path);
}

static char *
make_socket_path (char **out_root)
{
  g_autofree char *root =
      g_dir_make_tmp ("wyrebox-dovecot-daemon-client-XXXXXX", NULL);

  g_assert_nonnull (root);
  *out_root = g_strdup (root);

  return g_build_filename (root, "wyrebox.sock", NULL);
}

#endif

static void
assert_result_is_cleared (const WyreboxDaemonMailboxSelectResult *result)
{
  g_assert_cmpint (result->kind, ==, 0);
  g_assert_null (result->mailbox_id);
  g_assert_null (result->mailbox_name);
  g_assert_cmpuint (result->uid_validity, ==, 0);
  g_assert_cmpuint (result->uid_next, ==, 0);
}

static void
init_stale_result (WyreboxDaemonMailboxSelectResult *result)
{
  g_autoptr (GError) error = NULL;

  g_assert_true (wyrebox_daemon_mailbox_select_result_init (result,
          WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_VIRTUAL,
          "stale-id", "stale-name", 1, 2, &error));
  g_assert_no_error (error);
}

#if defined(WYREBOX_HAVE_CAPNP_SERIALIZATION) && WYREBOX_HAVE_CAPNP_SERIALIZATION

static void
write_truncated_response_frame (GOutputStream *output)
{
  const guint8 truncated_frame[] = {
    0x00, 0x00, 0x00, 0x05, 'a', 'b',
  };
  gsize bytes_written = 0;
  g_autoptr (GError) error = NULL;

  g_assert_true (g_output_stream_write_all (output, truncated_frame,
          sizeof (truncated_frame), &bytes_written, NULL, &error));
  g_assert_no_error (error);
  g_assert_cmpuint (bytes_written, ==, sizeof (truncated_frame));
}

static GBytes *encode_mailbox_select_response (const char *request_id);
static GBytes *encode_daemon_error_response (const char *request_id);
static GBytes *encode_unexpected_success_response (const char *request_id);

static char *
assert_decoded_select_request (GBytes *request)
{
  g_autoptr (GError) error = NULL;
  WyreboxDaemonDecodedRequestFrame decoded = { 0 };
  gpointer decoded_state = NULL;
  GDestroyNotify decoded_state_clear = NULL;
  char *request_id = NULL;

  g_assert_true (wyrebox_daemon_capnp_codec_decode_request_frame (NULL,
          request, &decoded, &decoded_state, &decoded_state_clear, NULL,
          &error));
  g_assert_no_error (error);
  g_assert_cmpint (decoded.operation, ==,
      WYREBOX_DAEMON_REQUEST_FRAME_OPERATION_MAILBOX_SELECT);
  g_assert_nonnull (decoded.mailbox_select);
  g_assert_cmpstr (decoded.caller_identity, ==, "dovecot");
  g_assert_cmpstr (decoded.account_identity, ==, "account-1");
  g_assert_cmpstr (decoded.tool_identity, ==, "dovecot-storage");
  g_assert_nonnull (decoded.request_id);
  g_assert_cmpstr (decoded.correlation_id, ==, "");
  g_assert_cmpstr (decoded.mailbox_select->account_identity, ==, "account-1");
  g_assert_cmpstr (decoded.mailbox_select->mailbox_id, ==, NULL);
  g_assert_cmpstr (decoded.mailbox_select->mailbox_name, ==, "Projects");

  g_assert_nonnull (decoded_state_clear);
  g_assert_nonnull (decoded_state);
  request_id = g_strdup (decoded.request_id);
  decoded_state_clear (decoded_state);

  return request_id;
}

static gpointer
fake_server_thread_main (gpointer user_data)
{
  FakeServer *server = user_data;
  g_autoptr (GError) error = NULL;
  g_autoptr (GSocketConnection) connection = NULL;
  g_autoptr (GBytes) request = NULL;
  g_autoptr (GBytes) response = NULL;
  g_autofree char *request_id = NULL;
  GInputStream *input = NULL;
  GOutputStream *output = NULL;

  connection = g_socket_listener_accept (server->listener, NULL, NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (connection);

  input = g_io_stream_get_input_stream (G_IO_STREAM (connection));
  output = g_io_stream_get_output_stream (G_IO_STREAM (connection));
  g_assert_nonnull (input);
  g_assert_nonnull (output);

  request = wyrebox_daemon_frame_io_read_payload (input, &error);
  g_assert_no_error (error);
  request_id = assert_decoded_select_request (request);

  switch (server->behavior) {
    case FAKE_SERVER_RESPONSE_BYTES:
      g_assert_true (wyrebox_daemon_frame_io_write_payload (output,
              server->response, server->response_size, &error));
      g_assert_no_error (error);
      break;
    case FAKE_SERVER_MAILBOX_SELECT_RESPONSE:
      response =
          encode_mailbox_select_response (server->response_request_id_override
          != NULL ? server->response_request_id_override : request_id);
      g_assert_nonnull (response);
      server->response = g_bytes_get_data (response, &server->response_size);
      g_assert_true (wyrebox_daemon_frame_io_write_payload (output,
              server->response, server->response_size, &error));
      g_assert_no_error (error);
      break;
    case FAKE_SERVER_DAEMON_ERROR_RESPONSE:
      response =
          encode_daemon_error_response (server->response_request_id_override !=
          NULL ? server->response_request_id_override : request_id);
      g_assert_nonnull (response);
      server->response = g_bytes_get_data (response, &server->response_size);
      g_assert_true (wyrebox_daemon_frame_io_write_payload (output,
              server->response, server->response_size, &error));
      g_assert_no_error (error);
      break;
    case FAKE_SERVER_UNEXPECTED_SUCCESS_RESPONSE:
      response =
          encode_unexpected_success_response
          (server->response_request_id_override !=
          NULL ? server->response_request_id_override : request_id);
      g_assert_nonnull (response);
      server->response = g_bytes_get_data (response, &server->response_size);
      g_assert_true (wyrebox_daemon_frame_io_write_payload (output,
              server->response, server->response_size, &error));
      g_assert_no_error (error);
      break;
    case FAKE_SERVER_CLOSE_WITHOUT_RESPONSE:
      break;
    case FAKE_SERVER_TRUNCATED_RESPONSE:
      write_truncated_response_frame (output);
      break;
  }

  g_assert_true (g_io_stream_close (G_IO_STREAM (connection), NULL, &error));
  g_assert_no_error (error);

  return NULL;
}

static void
fake_server_start (FakeServer *server,
    const char *socket_path,
    FakeServerBehavior behavior, GBytes *response_payload,
    const char *response_request_id_override)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (GSocketAddress) address = NULL;

  server->listener = g_socket_listener_new ();
  server->behavior = behavior;
  server->response_request_id_override = response_request_id_override;
  if (response_payload != NULL)
    server->response = g_bytes_get_data (response_payload,
        &server->response_size);

  address = g_unix_socket_address_new (socket_path);
  g_assert_true (g_socket_listener_add_address (server->listener,
          address, G_SOCKET_TYPE_STREAM, G_SOCKET_PROTOCOL_DEFAULT, NULL, NULL,
          &error));
  g_assert_no_error (error);

  server->thread =
      g_thread_new ("fake-dovecot-daemon-client", fake_server_thread_main,
      server);
}

static void
fake_server_join (FakeServer *server)
{
  if (server->thread != NULL)
    g_thread_join (server->thread);

  g_clear_object (&server->listener);
}

static GBytes *
encode_mailbox_select_response (const char *request_id)
{
  g_auto (WyreboxDaemonMailboxSelectResult) select = { 0 };
  g_auto (WyreboxDaemonResponseFrame) frame = { 0 };
  g_autoptr (GError) error = NULL;

  g_assert_true (wyrebox_daemon_mailbox_select_result_init (&select,
          WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_VIRTUAL,
          "view-projects", "Projects", 77, 42, &error));
  g_assert_no_error (error);
  g_assert_true (wyrebox_daemon_response_frame_init_mailbox_select (&frame,
          request_id, NULL, &select, &error));
  g_assert_no_error (error);

  return wyrebox_daemon_capnp_codec_encode_response_frame (&frame, NULL,
      &error);
}

static GBytes *
encode_daemon_error_response (const char *request_id)
{
  g_auto (WyreboxDaemonErrorFrame) daemon_error = { 0 };
  g_auto (WyreboxDaemonResponseFrame) frame = { 0 };
  g_autoptr (GError) error = NULL;

  g_assert_true (wyrebox_daemon_error_frame_init (&daemon_error,
          request_id, WYREBOX_DAEMON_ERROR_NOT_FOUND,
          "mailbox not found", NULL, &error));
  g_assert_no_error (error);
  g_assert_true (wyrebox_daemon_response_frame_init_error (&frame,
          &daemon_error, NULL, &error));
  g_assert_no_error (error);

  return wyrebox_daemon_capnp_codec_encode_response_frame (&frame, NULL,
      &error);
}

static GBytes *
encode_unexpected_success_response (const char *request_id)
{
  const char *args[] = { "message-1", "project-a", NULL };
  g_auto (WyreboxDaemonFactMutationRequest) mutation = { 0 };
  g_auto (WyreboxDaemonSuccessReceipt) receipt = { 0 };
  g_auto (WyreboxDaemonResponseFrame) frame = { 0 };
  g_autoptr (GError) error = NULL;

  g_assert_true (wyrebox_daemon_fact_mutation_request_init (&mutation,
          WYREBOX_DAEMON_FACT_MUTATION_INSERT,
          "project_mention", "account-1", args, &error));
  g_assert_no_error (error);
  g_assert_true (wyrebox_daemon_success_receipt_init_fact_mutation (&receipt,
          request_id, &mutation, 4096, 7, &error));
  g_assert_no_error (error);
  g_assert_true (wyrebox_daemon_response_frame_init_success (&frame,
          &receipt, NULL, &error));
  g_assert_no_error (error);

  return wyrebox_daemon_capnp_codec_encode_response_frame (&frame, NULL,
      &error);
}

static void
assert_select_call_fails_with_response (GBytes *response_payload,
    GIOErrorEnum expected_code)
{
  g_autofree char *root = NULL;
  g_autofree char *socket_path = make_socket_path (&root);
  g_auto (WyreboxDaemonMailboxSelectResult) result = { 0 };
  g_autoptr (GError) error = NULL;
  FakeServer server = { 0 };

  init_stale_result (&result);
  fake_server_start (&server, socket_path, FAKE_SERVER_RESPONSE_BYTES,
      response_payload, NULL);

  g_assert_false (wyrebox_dovecot_daemon_client_select_mailbox (socket_path,
          "account-1", "Projects", &result, &error));

  g_assert_error (error, G_IO_ERROR, expected_code);
  assert_result_is_cleared (&result);
  fake_server_join (&server);
  remove_tree (root);
}

static void
test_dovecot_daemon_client_sends_select_and_returns_copy (void)
{
  g_autofree char *root = NULL;
  g_autofree char *socket_path = make_socket_path (&root);
  g_auto (WyreboxDaemonMailboxSelectResult) result = { 0 };
  g_autoptr (GError) error = NULL;
  FakeServer server = { 0 };

  fake_server_start (&server, socket_path, FAKE_SERVER_MAILBOX_SELECT_RESPONSE,
      NULL, NULL);

  g_assert_true (wyrebox_dovecot_daemon_client_select_mailbox (socket_path,
          "account-1", "Projects", &result, &error));

  g_assert_no_error (error);
  g_assert_cmpint (result.kind, ==, WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_VIRTUAL);
  g_assert_cmpstr (result.mailbox_id, ==, "view-projects");
  g_assert_cmpstr (result.mailbox_name, ==, "Projects");
  g_assert_cmpuint (result.uid_validity, ==, 77);
  g_assert_cmpuint (result.uid_next, ==, 42);

  fake_server_join (&server);
  remove_tree (root);
}

static void
test_dovecot_daemon_client_daemon_error_fails_cleanly (void)
{
  g_autofree char *root = NULL;
  g_autofree char *socket_path = make_socket_path (&root);
  g_auto (WyreboxDaemonMailboxSelectResult) result = { 0 };
  g_autoptr (GError) error = NULL;
  FakeServer server = { 0 };

  init_stale_result (&result);
  fake_server_start (&server, socket_path, FAKE_SERVER_DAEMON_ERROR_RESPONSE,
      NULL, NULL);

  g_assert_false (wyrebox_dovecot_daemon_client_select_mailbox (socket_path,
          "account-1", "Projects", &result, &error));

  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_FAILED);
  assert_result_is_cleared (&result);
  fake_server_join (&server);
  remove_tree (root);
}

static void
assert_select_call_rejects_mismatched_request_id (FakeServerBehavior
    response_behavior)
{
  g_autofree char *root = NULL;
  g_autofree char *socket_path = make_socket_path (&root);
  g_auto (WyreboxDaemonMailboxSelectResult) result = { 0 };
  g_autoptr (GError) error = NULL;
  FakeServer server = { 0 };

  init_stale_result (&result);
  fake_server_start (&server, socket_path, response_behavior,
      NULL, "mismatched-request-id");

  g_assert_false (wyrebox_dovecot_daemon_client_select_mailbox (socket_path,
          "account-1", "Projects", &result, &error));

  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
  assert_result_is_cleared (&result);
  fake_server_join (&server);
  remove_tree (root);
}

static void
test_dovecot_daemon_client_mismatched_select_request_id_fails_cleanly (void)
{
  assert_select_call_rejects_mismatched_request_id
      (FAKE_SERVER_MAILBOX_SELECT_RESPONSE);
}

static void
test_dovecot_daemon_client_mismatched_error_request_id_fails_cleanly (void)
{
  assert_select_call_rejects_mismatched_request_id
      (FAKE_SERVER_DAEMON_ERROR_RESPONSE);
}

static void
test_dovecot_daemon_client_unexpected_response_fails_cleanly (void)
{
  g_autofree char *root = NULL;
  g_autofree char *socket_path = make_socket_path (&root);
  g_auto (WyreboxDaemonMailboxSelectResult) result = { 0 };
  g_autoptr (GError) error = NULL;
  FakeServer server = { 0 };

  init_stale_result (&result);
  fake_server_start (&server, socket_path,
      FAKE_SERVER_UNEXPECTED_SUCCESS_RESPONSE, NULL, NULL);

  g_assert_false (wyrebox_dovecot_daemon_client_select_mailbox (socket_path,
          "account-1", "Projects", &result, &error));

  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
  assert_result_is_cleared (&result);
  fake_server_join (&server);
  remove_tree (root);
}

static void
test_dovecot_daemon_client_malformed_response_fails_cleanly (void)
{
  const guint8 invalid_response[] = {
    'n', 'o', 't', '-', 'c', 'a', 'p', 'n', 'p',
  };
  g_autoptr (GBytes) response_payload =
      g_bytes_new_static (invalid_response, sizeof (invalid_response));

  assert_select_call_fails_with_response (response_payload,
      G_IO_ERROR_INVALID_ARGUMENT);
}

static void
test_dovecot_daemon_client_no_response_fails_cleanly (void)
{
  g_autofree char *root = NULL;
  g_autofree char *socket_path = make_socket_path (&root);
  g_auto (WyreboxDaemonMailboxSelectResult) result = { 0 };
  g_autoptr (GError) error = NULL;
  FakeServer server = { 0 };

  init_stale_result (&result);
  fake_server_start (&server, socket_path, FAKE_SERVER_CLOSE_WITHOUT_RESPONSE,
      NULL, NULL);

  g_assert_false (wyrebox_dovecot_daemon_client_select_mailbox (socket_path,
          "account-1", "Projects", &result, &error));

  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
  assert_result_is_cleared (&result);
  fake_server_join (&server);
  remove_tree (root);
}

static void
test_dovecot_daemon_client_truncated_response_fails_cleanly (void)
{
  g_autofree char *root = NULL;
  g_autofree char *socket_path = make_socket_path (&root);
  g_auto (WyreboxDaemonMailboxSelectResult) result = { 0 };
  g_autoptr (GError) error = NULL;
  FakeServer server = { 0 };

  init_stale_result (&result);
  fake_server_start (&server, socket_path, FAKE_SERVER_TRUNCATED_RESPONSE,
      NULL, NULL);

  g_assert_false (wyrebox_dovecot_daemon_client_select_mailbox (socket_path,
          "account-1", "Projects", &result, &error));

  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
  assert_result_is_cleared (&result);
  fake_server_join (&server);
  remove_tree (root);
}

#else

static void
test_dovecot_daemon_client_capnp_disabled_is_not_supported (void)
{
  g_auto (WyreboxDaemonMailboxSelectResult) result = { 0 };
  g_autoptr (GError) error = NULL;

  init_stale_result (&result);

  g_assert_false (wyrebox_dovecot_daemon_client_select_mailbox
      ("/tmp/wyrebox.sock", "account-1", "Projects", &result, &error));

  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED);
  assert_result_is_cleared (&result);
}

#endif

static void
test_dovecot_daemon_client_invalid_inputs_fail_cleanly (void)
{
  g_auto (WyreboxDaemonMailboxSelectResult) result = { 0 };
  g_autoptr (GError) error = NULL;

  init_stale_result (&result);
  g_assert_false (wyrebox_dovecot_daemon_client_select_mailbox (NULL,
          "account-1", "Projects", &result, &error));
#if defined(WYREBOX_HAVE_CAPNP_SERIALIZATION) && WYREBOX_HAVE_CAPNP_SERIALIZATION
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
#else
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED);
#endif
  assert_result_is_cleared (&result);

#if defined(WYREBOX_HAVE_CAPNP_SERIALIZATION) && WYREBOX_HAVE_CAPNP_SERIALIZATION
  g_clear_error (&error);
  init_stale_result (&result);
  g_assert_false (wyrebox_dovecot_daemon_client_select_mailbox
      ("/tmp/wyrebox.sock", "", "Projects", &result, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  assert_result_is_cleared (&result);

  g_clear_error (&error);
  init_stale_result (&result);
  g_assert_false (wyrebox_dovecot_daemon_client_select_mailbox
      ("/tmp/wyrebox.sock", "account-1", "", &result, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  assert_result_is_cleared (&result);

  g_clear_error (&error);
  g_assert_false (wyrebox_dovecot_daemon_client_select_mailbox
      ("/tmp/wyrebox.sock", "account-1", "Projects", NULL, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
#endif
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

#if defined(WYREBOX_HAVE_CAPNP_SERIALIZATION) && WYREBOX_HAVE_CAPNP_SERIALIZATION
  g_test_add_func ("/dovecot/daemon-client/select-sends-request-and-copies",
      test_dovecot_daemon_client_sends_select_and_returns_copy);
  g_test_add_func ("/dovecot/daemon-client/daemon-error",
      test_dovecot_daemon_client_daemon_error_fails_cleanly);
  g_test_add_func ("/dovecot/daemon-client/mismatched-select-request-id",
      test_dovecot_daemon_client_mismatched_select_request_id_fails_cleanly);
  g_test_add_func ("/dovecot/daemon-client/mismatched-error-request-id",
      test_dovecot_daemon_client_mismatched_error_request_id_fails_cleanly);
  g_test_add_func ("/dovecot/daemon-client/unexpected-response",
      test_dovecot_daemon_client_unexpected_response_fails_cleanly);
  g_test_add_func ("/dovecot/daemon-client/malformed-response",
      test_dovecot_daemon_client_malformed_response_fails_cleanly);
  g_test_add_func ("/dovecot/daemon-client/no-response",
      test_dovecot_daemon_client_no_response_fails_cleanly);
  g_test_add_func ("/dovecot/daemon-client/truncated-response",
      test_dovecot_daemon_client_truncated_response_fails_cleanly);
#else
  g_test_add_func ("/dovecot/daemon-client/capnp-disabled-not-supported",
      test_dovecot_daemon_client_capnp_disabled_is_not_supported);
#endif
  g_test_add_func ("/dovecot/daemon-client/invalid-inputs",
      test_dovecot_daemon_client_invalid_inputs_fail_cleanly);

  return g_test_run ();
}
