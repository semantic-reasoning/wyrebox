#include "wyrebox-daemon-capnp-codec.h"
#include "wyrebox-daemon-connection-server.h"
#include "wyrebox-daemon-frame-io.h"
#include "wyrebox-daemon-mailbox-list-service.h"
#include "wyrebox-daemon-request-adapter.h"

#include <capnp/message.h>
#include <capnp/serialize.h>

#include <gio/gio.h>
#include <gio/gunixsocketaddress.h>
#include <glib.h>
#include <glib/gstdio.h>

#include "wyrebox-daemon-api.capnp.h"

typedef struct
{
  gboolean was_called;
} MailboxListFixtureState;

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
      g_dir_make_tmp ("wyrebox-daemon-capnp-uds-XXXXXX", NULL);

  g_assert_nonnull (root);
  *out_root = g_strdup (root);

  return g_build_filename (root, "run", "wyrebox.sock", NULL);
}

static GBytes *
build_mailbox_list_request_bytes (void)
{
  capnp::MallocMessageBuilder request_builder;
  auto request_frame = request_builder.initRoot<RequestFrame> ();

  auto identity = request_frame.initIdentity ();
  identity.setRequestId ("request-uds-1");
  identity.setCallerIdentity ("dovecot");
  identity.setAccountIdentity ("account-uds");
  identity.setToolIdentity ("dovecot-storage");
  identity.setCorrelationId ("corr-uds-1");

  auto mailbox_list = request_frame.initMailboxList ();
  mailbox_list.setAccountIdentity ("account-uds");
  mailbox_list.setNamespacePrefix ("");

  auto words = capnp::messageToFlatArray (request_builder);
  auto bytes = words.asBytes ();

  return g_bytes_new (bytes.begin (), bytes.size ());
}

static gboolean
list_mailboxes_fixture (const WyreboxDaemonRequestIdentity *identity,
    const WyreboxDaemonMailboxListRequest *request,
    WyreboxDaemonMailboxListResult *out_result,
    gpointer user_data,
    GError **error)
{
  MailboxListFixtureState *state =
      static_cast<MailboxListFixtureState *> (user_data);

  g_assert_nonnull (identity);
  g_assert_nonnull (request);
  g_assert_nonnull (state);

  g_assert_cmpstr (identity->request_id, ==, "request-uds-1");
  g_assert_cmpstr (identity->caller_identity, ==, "dovecot");
  g_assert_cmpstr (identity->account_identity, ==, "account-uds");
  g_assert_cmpstr (identity->tool_identity, ==, "dovecot-storage");
  g_assert_cmpstr (identity->correlation_id, ==, "corr-uds-1");
  g_assert_cmpstr (request->account_identity, ==, "account-uds");
  g_assert_cmpstr (request->namespace_prefix, ==, "");

  state->was_called = TRUE;

  wyrebox_daemon_mailbox_list_result_init_empty (out_result);
  return wyrebox_daemon_mailbox_list_result_append_entry (out_result,
      WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_ORDINARY,
      "mailbox-inbox",
      "INBOX",
      "/",
      "\\Inbox",
      TRUE,
      WYREBOX_DAEMON_MAILBOX_LIST_CHILD_STATE_HAS_NO_CHILDREN,
      error);
}

static GSocketConnection *
connect_with_socket_client (const char *socket_path)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (GSocketAddress) address = NULL;
  g_autoptr (GSocketClient) client = NULL;
  g_autoptr (GSocketConnection) connection = NULL;

  client = g_socket_client_new ();
  address = g_unix_socket_address_new (socket_path);
  connection = g_socket_client_connect (client, G_SOCKET_CONNECTABLE (address),
      NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (connection);

  return g_steal_pointer (&connection);
}

static void
write_framed_bytes (GOutputStream *output, GBytes *payload)
{
  g_autoptr (GError) error = NULL;
  const guint8 *data = NULL;
  gsize size = 0;

  data = static_cast<const guint8 *> (g_bytes_get_data (payload, &size));
  g_assert_true (wyrebox_daemon_frame_io_write_payload (output, data, size,
          &error));
  g_assert_no_error (error);
}

static void
shutdown_output (GSocketConnection *connection)
{
  g_autoptr (GError) error = NULL;
  GSocket *socket = NULL;

  socket = g_socket_connection_get_socket (connection);
  g_assert_nonnull (socket);
  g_assert_true (g_socket_shutdown (socket, FALSE, TRUE, &error));
  g_assert_no_error (error);
}

static void
await_framed_response_data (GSocket *socket, guint timeout_ms)
{
  gint64 deadline = 0;
  const gint64 poll_slice_us = 10 * G_TIME_SPAN_MILLISECOND;

  deadline = g_get_monotonic_time ()
      + (gint64) timeout_ms * G_TIME_SPAN_MILLISECOND;

  while (g_get_monotonic_time () < deadline) {
    g_autoptr (GError) error = NULL;
    gint64 now = 0;
    gint64 remaining_us = 0;
    gint64 wait_us = 0;

    while (g_main_context_pending (NULL))
      g_assert_true (g_main_context_iteration (NULL, FALSE));

    if (g_socket_get_available_bytes (socket) >= (gssize) sizeof (guint32))
      return;

    now = g_get_monotonic_time ();
    remaining_us = deadline - now;
    if (remaining_us <= 0)
      break;

    wait_us = MIN (remaining_us, poll_slice_us);
    (void) g_socket_condition_timed_wait (socket,
        (GIOCondition) (G_IO_IN | G_IO_HUP | G_IO_ERR),
        wait_us,
        NULL,
        &error);
    g_assert_no_error (error);
  }

  g_assert_not_reached ();
}

static GBytes *
send_request_and_read_response (const char *socket_path, GBytes *request)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (GSocketConnection) connection = NULL;
  g_autoptr (GBytes) response = NULL;
  GInputStream *input = NULL;
  GOutputStream *output = NULL;
  GSocket *socket = NULL;

  connection = connect_with_socket_client (socket_path);
  input = g_io_stream_get_input_stream (G_IO_STREAM (connection));
  output = g_io_stream_get_output_stream (G_IO_STREAM (connection));
  socket = g_socket_connection_get_socket (connection);
  g_assert_nonnull (socket);

  write_framed_bytes (output, request);
  shutdown_output (connection);
  await_framed_response_data (socket, 2000);

  response = wyrebox_daemon_frame_io_read_payload (input, &error);
  g_assert_no_error (error);
  g_assert_nonnull (response);

  g_assert_true (g_io_stream_close (G_IO_STREAM (connection), NULL, &error));
  g_assert_no_error (error);

  return g_steal_pointer (&response);
}

static void
assert_mailbox_list_response (GBytes *response)
{
  gsize size = 0;
  const guint8 *data = static_cast<const guint8 *> (
      g_bytes_get_data (response, &size));

  g_assert_cmpuint (size % sizeof (capnp::word), ==, 0);

  auto words = kj::arrayPtr (reinterpret_cast<const capnp::word *> (data),
      size / sizeof (capnp::word));
  capnp::FlatArrayMessageReader reader (words);
  auto response_frame = reader.getRoot<ResponseFrame> ();

  g_assert_true (response_frame.which () == ResponseFrame::MAILBOX_LIST);
  g_assert_cmpstr (response_frame.getRequestId ().cStr (), ==,
      "request-uds-1");
  g_assert_cmpstr (response_frame.getCorrelationId ().cStr (), ==,
      "corr-uds-1");

  auto mailbox_list = response_frame.getMailboxList ();
  g_assert_cmpstr (mailbox_list.getRequestId ().cStr (), ==,
      "request-uds-1");
  g_assert_cmpuint (mailbox_list.getEntries ().size (), ==, 1);

  auto entry = mailbox_list.getEntries ()[0];
  g_assert_true (entry.getKind () == MailboxListEntryKind::ORDINARY);
  g_assert_cmpstr (entry.getMailboxId ().cStr (), ==, "mailbox-inbox");
  g_assert_cmpstr (entry.getMailboxName ().cStr (), ==, "INBOX");
  g_assert_cmpstr (entry.getHierarchyDelimiter ().cStr (), ==, "/");
  g_assert_cmpstr (entry.getSpecialUse ().cStr (), ==, "\\Inbox");
  g_assert_true (entry.getSelectable ());
  g_assert_true (entry.getChildState () ==
      MailboxListChildState::HAS_NO_CHILDREN);
}

static void
test_capnp_uds_mailbox_list_round_trip (void)
{
  g_autofree char *root = NULL;
  g_autofree char *socket_path = NULL;
  g_autoptr (GBytes) request = NULL;
  g_autoptr (GBytes) response = NULL;
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxDaemonConnectionServer) server = NULL;
  g_autoptr (WyreboxDaemonMailboxListService) mailbox_list_service = NULL;
  g_autoptr (WyreboxDaemonRequestAdapter) adapter = NULL;
  MailboxListFixtureState state = { 0 };

  socket_path = make_socket_path (&root);
  mailbox_list_service = wyrebox_daemon_mailbox_list_service_new (
      list_mailboxes_fixture, &state, NULL);
  adapter = wyrebox_daemon_request_adapter_new (NULL,
      NULL,
      mailbox_list_service,
      NULL,
      NULL,
      NULL,
      NULL,
      NULL,
      wyrebox_daemon_capnp_codec_decode_request_frame,
      NULL,
      NULL,
      wyrebox_daemon_capnp_codec_encode_response_frame,
      NULL,
      NULL);
  server = wyrebox_daemon_connection_server_new (socket_path, adapter);

  g_assert_nonnull (mailbox_list_service);
  g_assert_nonnull (adapter);
  g_assert_nonnull (server);
  g_assert_true (wyrebox_daemon_connection_server_start (server, &error));
  g_assert_no_error (error);

  request = build_mailbox_list_request_bytes ();
  response = send_request_and_read_response (socket_path, request);

  g_assert_true (state.was_called);
  assert_mailbox_list_response (response);

  g_assert_true (wyrebox_daemon_connection_server_stop (server, &error));
  g_assert_no_error (error);

  remove_tree (root);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/daemon-api/capnp-uds/mailbox-list-round-trip",
      test_capnp_uds_mailbox_list_round_trip);

  return g_test_run ();
}
