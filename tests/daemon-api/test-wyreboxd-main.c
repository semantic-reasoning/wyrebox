#include <gio/gio.h>
#include <glib.h>
#include <glib/gstdio.h>

#include "wyrebox-daemon-frame-io.h"
#if defined(WYREBOX_HAVE_CAPNP_SERIALIZATION) && \
    WYREBOX_HAVE_CAPNP_SERIALIZATION
#include "wyrebox-daemon-capnp-codec.h"
#endif

#include <signal.h>
#include <sys/stat.h>

static char *
make_temp_root (void)
{
  g_autofree char *root = g_dir_make_tmp ("wyreboxd-main-test-XXXXXX", NULL);

  g_assert_nonnull (root);
  return g_steal_pointer (&root);
}

static char *
write_config (const char *dir, const char *contents)
{
  g_autofree char *path = g_build_filename (dir, "wyrebox.conf", NULL);
  g_autoptr (GError) error = NULL;

  g_assert_true (g_file_set_contents (path, contents, -1, &error));
  g_assert_no_error (error);
  g_assert_cmpint (chmod (path, 0600), ==, 0);
  return g_steal_pointer (&path);
}

static const char *
wyreboxd_executable (void)
{
  const char *path = g_getenv ("WYREBOXD_EXECUTABLE");

  g_assert_nonnull (path);
  g_assert_cmpstr (path, !=, "");
  return path;
}

static gboolean
wait_for_socket (const char *socket_path)
{
  for (guint i = 0; i < 200; i++) {
    if (g_file_test (socket_path, G_FILE_TEST_EXISTS))
      return TRUE;
    g_usleep (10 * 1000);
  }

  return FALSE;
}

#if defined(WYREBOX_HAVE_CAPNP_SERIALIZATION) && \
    WYREBOX_HAVE_CAPNP_SERIALIZATION
static GBytes *
build_delivery_request (void)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) encoded = NULL;
  g_auto (WyreboxDaemonRequestIdentity) identity = { 0 };
  g_auto (WyreboxDaemonDeliveryIngestionRequest) request = { 0 };
  const gchar *const recipients[] = { "recipient@example.com", NULL };
  const guint8 payload[] =
      "From: sender@example.com\r\n"
      "To: recipient@example.com\r\n"
      "Subject: wyreboxd roundtrip\r\n" "\r\n" "body\r\n";

  g_assert_true (wyrebox_daemon_request_identity_init (&identity,
          "request-wyreboxd-1",
          "postfix",
          "account-1", "wyrebox-postfix-pipe", "corr-wyreboxd-1", &error));
  g_assert_no_error (error);

  g_assert_true (wyrebox_daemon_delivery_ingestion_request_init (&request,
          "delivery-wyreboxd-1",
          "queue-wyreboxd-1",
          "sender@example.com",
          recipients,
          g_bytes_new_static (payload, sizeof (payload) - 1), &error));
  g_assert_no_error (error);

  encoded =
      wyrebox_daemon_capnp_codec_encode_delivery_ingestion_request (&identity,
      &request, NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (encoded);

  return g_steal_pointer (&encoded);
}

static void
assert_error_response_roundtrip (GBytes *response)
{
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonResponseFrame) frame = { 0 };

  g_assert_true (wyrebox_daemon_capnp_codec_decode_response_frame (response,
          &frame, &error));
  g_assert_no_error (error);
  g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_ERROR);
  g_assert_nonnull (frame.request_id);
}

static GBytes *
roundtrip_request (const char *socket_path, GBytes *request)
{
  g_autoptr (GBytes) response = NULL;
  g_autoptr (GError) error = NULL;
  g_autoptr (GSocketConnection) connection = NULL;
  GInputStream *input = NULL;
  GOutputStream *output = NULL;
  GSocket *socket = NULL;
  g_autoptr (GSocketClient) client = NULL;
  g_autoptr (GSocketAddress) address = NULL;

  client = g_socket_client_new ();
  address = g_unix_socket_address_new (socket_path);
  connection = g_socket_client_connect (client, G_SOCKET_CONNECTABLE (address),
      NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (connection);

  input = g_io_stream_get_input_stream (G_IO_STREAM (connection));
  output = g_io_stream_get_output_stream (G_IO_STREAM (connection));
  socket = g_socket_connection_get_socket (connection);
  g_assert_nonnull (socket);

  g_assert_true (wyrebox_daemon_frame_io_write_payload (output,
          (const guint8 *) g_bytes_get_data (request, NULL),
          g_bytes_get_size (request), &error));
  g_assert_no_error (error);

  g_assert_true (g_socket_shutdown (socket, FALSE, TRUE, &error));
  g_assert_no_error (error);

  response = wyrebox_daemon_frame_io_read_payload (input, &error);
  g_assert_no_error (error);
  g_assert_nonnull (response);

  return g_steal_pointer (&response);
}
#endif

static void
test_wyreboxd_accepts_config_and_starts_socket (void)
{
  g_autofree char *root = make_temp_root ();
  g_autofree char *run_dir = g_build_filename (root, "run", "wyrebox", NULL);
  g_autofree char *config_path = NULL;
  g_autofree char *socket_path = NULL;
  g_autofree char *config_contents = NULL;
  g_autoptr (GSubprocess) subprocess = NULL;
  g_autoptr (GError) error = NULL;

  g_assert_cmpint (g_mkdir_with_parents (run_dir, 0750), ==, 0);
  socket_path = g_build_filename (run_dir, "wyrebox.sock", NULL);
  config_contents = g_strdup_printf ("[daemon]\n"
      "socket_path=%s\n", socket_path);
  config_path = write_config (root, config_contents);

  const char *argv[] = {
    wyreboxd_executable (),
    "--config",
    config_path,
    NULL
  };

  subprocess = g_subprocess_newv (argv,
      (GSubprocessFlags) (G_SUBPROCESS_FLAGS_STDOUT_SILENCE |
          G_SUBPROCESS_FLAGS_STDERR_PIPE), &error);
  g_assert_no_error (error);
  g_assert_nonnull (subprocess);

  g_assert_true (wait_for_socket (socket_path));
  g_subprocess_send_signal (subprocess, SIGTERM);
  g_assert_true (g_subprocess_wait (subprocess, NULL, &error));
  g_assert_no_error (error);
  g_assert_cmpint (g_subprocess_get_exit_status (subprocess), ==, 0);
}

static void
test_wyreboxd_rejects_invalid_config_path (void)
{
  g_autoptr (GSubprocess) subprocess = NULL;
  g_autoptr (GError) error = NULL;
  const char *argv[] = {
    wyreboxd_executable (),
    "--config",
    "/tmp/wyreboxd-does-not-exist.conf",
    NULL
  };

  subprocess = g_subprocess_newv (argv,
      (GSubprocessFlags) (G_SUBPROCESS_FLAGS_STDOUT_SILENCE |
          G_SUBPROCESS_FLAGS_STDERR_PIPE), &error);
  g_assert_no_error (error);
  g_assert_nonnull (subprocess);

  g_assert_true (g_subprocess_wait (subprocess, NULL, &error));
  g_assert_no_error (error);
  g_assert_cmpint (g_subprocess_get_exit_status (subprocess), ==, 78);
}

static void
test_wyreboxd_roundtrips_delivery_request_over_socket (void)
{
#if defined(WYREBOX_HAVE_CAPNP_SERIALIZATION) && \
    WYREBOX_HAVE_CAPNP_SERIALIZATION
  g_autofree char *root = make_temp_root ();
  g_autofree char *run_dir = g_build_filename (root, "run", "wyrebox", NULL);
  g_autofree char *config_path = NULL;
  g_autofree char *socket_path = NULL;
  g_autofree char *config_contents = NULL;
  g_autoptr (GSubprocess) subprocess = NULL;
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) request = NULL;
  g_autoptr (GBytes) response = NULL;

  g_assert_cmpint (g_mkdir_with_parents (run_dir, 0750), ==, 0);
  socket_path = g_build_filename (run_dir, "wyrebox.sock", NULL);
  config_contents = g_strdup_printf ("[daemon]\n"
      "socket_path=%s\n", socket_path);
  config_path = write_config (root, config_contents);

  const char *argv[] = {
    wyreboxd_executable (),
    "--config",
    config_path,
    NULL
  };

  subprocess = g_subprocess_newv (argv,
      (GSubprocessFlags) (G_SUBPROCESS_FLAGS_STDOUT_SILENCE |
          G_SUBPROCESS_FLAGS_STDERR_PIPE), &error);
  g_assert_no_error (error);
  g_assert_nonnull (subprocess);

  g_assert_true (wait_for_socket (socket_path));
  request = build_delivery_request ();
  response = roundtrip_request (socket_path, request);
  assert_error_response_roundtrip (response);

  g_subprocess_send_signal (subprocess, SIGTERM);
  g_assert_true (g_subprocess_wait (subprocess, NULL, &error));
  g_assert_no_error (error);
  g_assert_cmpint (g_subprocess_get_exit_status (subprocess), ==, 0);
#else
  g_test_skip ("Cap'n Proto serialization is not available in this build");
#endif
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/daemon-api/wyreboxd/starts-with-valid-config",
      test_wyreboxd_accepts_config_and_starts_socket);
  g_test_add_func ("/daemon-api/wyreboxd/rejects-invalid-config-path",
      test_wyreboxd_rejects_invalid_config_path);
  g_test_add_func
      ("/daemon-api/wyreboxd/roundtrips-delivery-request-over-socket",
      test_wyreboxd_roundtrips_delivery_request_over_socket);

  return g_test_run ();
}
