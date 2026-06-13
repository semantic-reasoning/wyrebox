#include "wyrebox-daemon-frame-io.h"
#include "wyrebox-daemon-uds-client.h"

#include <gio/gio.h>
#include <gio/gunixsocketaddress.h>
#include <glib.h>
#include <glib/gstdio.h>

#include <string.h>

typedef enum
{
  FAKE_SERVER_ROUND_TRIP,
  FAKE_SERVER_CLOSE_WITHOUT_RESPONSE,
  FAKE_SERVER_TRUNCATED_RESPONSE,
} FakeServerBehavior;

typedef struct
{
  GSocketListener *listener;
  GThread *thread;
  FakeServerBehavior behavior;
  const guint8 *expected_request;
  gsize expected_request_size;
  const guint8 *response;
  gsize response_size;
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
      g_dir_make_tmp ("wyrebox-daemon-uds-client-XXXXXX", NULL);

  g_assert_nonnull (root);
  *out_root = g_strdup (root);

  return g_build_filename (root, "wyrebox.sock", NULL);
}

static void
assert_bytes_equal (GBytes *actual, const guint8 *expected, gsize expected_size)
{
  const guint8 *actual_data = NULL;
  gsize actual_size = 0;

  g_assert_nonnull (actual);
  actual_data = g_bytes_get_data (actual, &actual_size);
  g_assert_cmpuint (actual_size, ==, expected_size);
  g_assert_cmpint (memcmp (actual_data, expected, expected_size), ==, 0);
}

static void
write_truncated_response_frame (GOutputStream *output)
{
  const guint8 truncated_frame[] = {
    0x00, 0x00, 0x00, 0x05, 'a', 'b', 'c',
  };
  gsize bytes_written = 0;
  g_autoptr (GError) error = NULL;

  g_assert_true (g_output_stream_write_all (output, truncated_frame,
          sizeof (truncated_frame), &bytes_written, NULL, &error));
  g_assert_no_error (error);
  g_assert_cmpuint (bytes_written, ==, sizeof (truncated_frame));
}

static gpointer
fake_server_thread_main (gpointer user_data)
{
  FakeServer *server = user_data;
  g_autoptr (GError) error = NULL;
  g_autoptr (GSocketConnection) connection = NULL;
  GInputStream *input = NULL;
  GOutputStream *output = NULL;
  g_autoptr (GBytes) request = NULL;

  connection = g_socket_listener_accept (server->listener, NULL, NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (connection);

  input = g_io_stream_get_input_stream (G_IO_STREAM (connection));
  output = g_io_stream_get_output_stream (G_IO_STREAM (connection));
  g_assert_nonnull (input);
  g_assert_nonnull (output);

  request = wyrebox_daemon_frame_io_read_payload (input, &error);
  g_assert_no_error (error);
  assert_bytes_equal (request, server->expected_request,
      server->expected_request_size);

  switch (server->behavior) {
    case FAKE_SERVER_ROUND_TRIP:
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
    FakeServerBehavior behavior,
    const guint8 *expected_request,
    gsize expected_request_size, const guint8 *response, gsize response_size)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (GSocketAddress) address = NULL;

  server->listener = g_socket_listener_new ();
  server->behavior = behavior;
  server->expected_request = expected_request;
  server->expected_request_size = expected_request_size;
  server->response = response;
  server->response_size = response_size;

  address = g_unix_socket_address_new (socket_path);
  g_assert_true (g_socket_listener_add_address (server->listener,
          address, G_SOCKET_TYPE_STREAM, G_SOCKET_PROTOCOL_DEFAULT,
          NULL, NULL, &error));
  g_assert_no_error (error);

  server->thread =
      g_thread_new ("fake-uds-client-server", fake_server_thread_main, server);
}

static void
fake_server_join (FakeServer *server)
{
  if (server->thread != NULL)
    g_thread_join (server->thread);

  g_clear_object (&server->listener);
}

static void
test_uds_client_roundtrip_preserves_raw_bytes (void)
{
  const guint8 request[] = {
    0x00, 'r', 'e', 'q', 0x00, 0xFF, 0x7F, 'z',
  };
  const guint8 response[] = {
    0x01, 'r', 'e', 's', 0x00, 0xFE, 0x80, 0x00,
  };
  g_autofree char *root = NULL;
  g_autofree char *socket_path = make_socket_path (&root);
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) request_payload = NULL;
  g_autoptr (GBytes) response_payload = NULL;
  FakeServer server = { 0 };

  fake_server_start (&server, socket_path, FAKE_SERVER_ROUND_TRIP,
      request, sizeof (request), response, sizeof (response));
  request_payload = g_bytes_new_static (request, sizeof (request));

  response_payload = wyrebox_daemon_uds_client_send_request (socket_path,
      request_payload, &error);

  g_assert_no_error (error);
  assert_bytes_equal (response_payload, response, sizeof (response));
  fake_server_join (&server);
  remove_tree (root);
}

static void
test_uds_client_rejects_null_socket_path (void)
{
  const guint8 request[] = "request";
  g_autoptr (GBytes) request_payload = NULL;
  g_autoptr (GBytes) response = NULL;
  g_autoptr (GError) error = NULL;

  request_payload = g_bytes_new_static (request, sizeof (request));
  response = wyrebox_daemon_uds_client_send_request (NULL, request_payload,
      &error);

  g_assert_null (response);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
}

static void
test_uds_client_rejects_empty_socket_path (void)
{
  const guint8 request[] = "request";
  g_autoptr (GBytes) request_payload = NULL;
  g_autoptr (GBytes) response = NULL;
  g_autoptr (GError) error = NULL;

  request_payload = g_bytes_new_static (request, sizeof (request));
  response = wyrebox_daemon_uds_client_send_request ("", request_payload,
      &error);

  g_assert_null (response);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
}

static void
test_uds_client_rejects_null_request_payload (void)
{
  g_autoptr (GBytes) response = NULL;
  g_autoptr (GError) error = NULL;

  response = wyrebox_daemon_uds_client_send_request ("/tmp/missing.sock", NULL,
      &error);

  g_assert_null (response);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
}

static void
test_uds_client_rejects_empty_request_payload (void)
{
  g_autoptr (GBytes) request_payload = NULL;
  g_autoptr (GBytes) response = NULL;
  g_autoptr (GError) error = NULL;

  request_payload = g_bytes_new_static ("", 0);
  response = wyrebox_daemon_uds_client_send_request ("/tmp/missing.sock",
      request_payload, &error);

  g_assert_null (response);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
}

static void
test_uds_client_missing_socket_returns_error (void)
{
  const guint8 request[] = "request";
  g_autofree char *root = NULL;
  g_autofree char *socket_path = make_socket_path (&root);
  g_autoptr (GBytes) request_payload = NULL;
  g_autoptr (GBytes) response = NULL;
  g_autoptr (GError) error = NULL;

  request_payload = g_bytes_new_static (request, sizeof (request));
  response = wyrebox_daemon_uds_client_send_request (socket_path,
      request_payload, &error);

  g_assert_null (response);
  g_assert_nonnull (error);
  g_assert_cmpuint (error->domain, ==, G_IO_ERROR);
  remove_tree (root);
}

static void
test_uds_client_server_close_without_response_returns_error (void)
{
  const guint8 request[] = "request";
  g_autofree char *root = NULL;
  g_autofree char *socket_path = make_socket_path (&root);
  g_autoptr (GBytes) request_payload = NULL;
  g_autoptr (GBytes) response = NULL;
  g_autoptr (GError) error = NULL;
  FakeServer server = { 0 };

  fake_server_start (&server, socket_path, FAKE_SERVER_CLOSE_WITHOUT_RESPONSE,
      request, sizeof (request), NULL, 0);
  request_payload = g_bytes_new_static (request, sizeof (request));

  response = wyrebox_daemon_uds_client_send_request (socket_path,
      request_payload, &error);

  g_assert_null (response);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
  fake_server_join (&server);
  remove_tree (root);
}

static void
test_uds_client_truncated_response_returns_error (void)
{
  const guint8 request[] = "request";
  g_autofree char *root = NULL;
  g_autofree char *socket_path = make_socket_path (&root);
  g_autoptr (GBytes) request_payload = NULL;
  g_autoptr (GBytes) response = NULL;
  g_autoptr (GError) error = NULL;
  FakeServer server = { 0 };

  fake_server_start (&server, socket_path, FAKE_SERVER_TRUNCATED_RESPONSE,
      request, sizeof (request), NULL, 0);
  request_payload = g_bytes_new_static (request, sizeof (request));

  response = wyrebox_daemon_uds_client_send_request (socket_path,
      request_payload, &error);

  g_assert_null (response);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
  fake_server_join (&server);
  remove_tree (root);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/daemon-api/uds-client/roundtrip-preserves-raw-bytes",
      test_uds_client_roundtrip_preserves_raw_bytes);
  g_test_add_func ("/daemon-api/uds-client/rejects-null-socket-path",
      test_uds_client_rejects_null_socket_path);
  g_test_add_func ("/daemon-api/uds-client/rejects-empty-socket-path",
      test_uds_client_rejects_empty_socket_path);
  g_test_add_func ("/daemon-api/uds-client/rejects-null-request-payload",
      test_uds_client_rejects_null_request_payload);
  g_test_add_func ("/daemon-api/uds-client/rejects-empty-request-payload",
      test_uds_client_rejects_empty_request_payload);
  g_test_add_func ("/daemon-api/uds-client/missing-socket-returns-error",
      test_uds_client_missing_socket_returns_error);
  g_test_add_func ("/daemon-api/uds-client/server-close-without-response",
      test_uds_client_server_close_without_response_returns_error);
  g_test_add_func ("/daemon-api/uds-client/truncated-response",
      test_uds_client_truncated_response_returns_error);

  return g_test_run ();
}
