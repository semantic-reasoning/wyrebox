#include "config.h"

#include "lib.h"
#include "mail-namespace.h"
#include "mail-storage-private.h"
#include "mail-storage.h"
#include "mail-user.h"
#include "module-dir.h"
#include "wyrebox-daemon-capnp-codec.h"
#include "wyrebox-daemon-error-frame.h"
#include "wyrebox-daemon-frame-io.h"
#include "wyrebox-daemon-mailbox-select-result.h"
#include "wyrebox-daemon-response-frame.h"

#include <gio/gio.h>
#include <gio/gunixsocketaddress.h>
#include <glib/gstdio.h>
#include <stdio.h>

extern struct mail_storage *wyrebox_dovecot_loader_shim_mail_storage_class;
extern int wyrebox_dovecot_loader_shim_mail_storage_class_register_calls;
extern int wyrebox_dovecot_loader_shim_mail_storage_class_unregister_calls;
extern void wyrebox_plugin_init (struct module *module);
extern void wyrebox_plugin_deinit (void);

const char *wyrebox_dovecot_test_daemon_socket_path;

typedef enum
{
  FAKE_SERVER_MAILBOX_SELECT_RESPONSE,
  FAKE_SERVER_DAEMON_ERROR_RESPONSE,
  FAKE_SERVER_MAILBOX_SELECT_THEN_UID_MAP_RESPONSE,
} FakeServerBehavior;

typedef struct
{
  GSocketListener *listener;
  GThread *thread;
  FakeServerBehavior behavior;
  const char *uid_map_csv;
  const char *expected_uid_map_mailbox_id;
  guint request_count;
  guint expected_request_count;
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
  g_autofree char *root = NULL;
  char *socket_path = NULL;

  root = g_dir_make_tmp ("wyrebox-dovecot-plugin-mailbox-smoke-XXXXXX", NULL);
  g_assert_nonnull (root);

  *out_root = g_steal_pointer (&root);
  socket_path = g_build_filename (*out_root, "wyrebox.sock", NULL);
  g_assert_nonnull (socket_path);
  return socket_path;
}

#if defined(WYREBOX_HAVE_CAPNP_SERIALIZATION) && WYREBOX_HAVE_CAPNP_SERIALIZATION
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
  g_assert_cmpstr (decoded.request_id, !=, "");
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

static char *
assert_decoded_uid_map_request (GBytes *request,
    const char *expected_mailbox_id)
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
      WYREBOX_DAEMON_REQUEST_FRAME_OPERATION_DUCKDB_QUERY_TEMPLATE);
  g_assert_nonnull (decoded.duckdb_query_template);
  g_assert_cmpstr (decoded.caller_identity, ==, "dovecot");
  g_assert_cmpstr (decoded.account_identity, ==, "account-1");
  g_assert_cmpstr (decoded.tool_identity, ==, "dovecot-storage");
  g_assert_cmpstr (decoded.correlation_id, ==, "");
  g_assert_cmpstr (decoded.duckdb_query_template->template_id, ==,
      "mailbox.uid_map.v1");
  g_assert_cmpstr (decoded.duckdb_query_template->scope_id, ==, "account-1");
  g_assert_nonnull (decoded.duckdb_query_template->parameters);
  g_assert_cmpstr (decoded.duckdb_query_template->parameters[0], ==,
      expected_mailbox_id);
  g_assert_null (decoded.duckdb_query_template->parameters[1]);

  g_assert_nonnull (decoded_state_clear);
  g_assert_nonnull (decoded_state);
  request_id = g_strdup (decoded.request_id);
  decoded_state_clear (decoded_state);

  return request_id;
}

#if defined(WYREBOX_HAVE_CAPNP_SERIALIZATION) && WYREBOX_HAVE_CAPNP_SERIALIZATION
static GBytes *
encode_mailbox_select_response (const char *request_id)
{
  g_auto (WyreboxDaemonMailboxSelectResult) select = { 0 };
  g_auto (WyreboxDaemonResponseFrame) frame = { 0 };
  g_autoptr (GError) error = NULL;

  g_assert_true (wyrebox_daemon_mailbox_select_result_init (&select,
          WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_VIRTUAL,
          "view-projects", "Projects", 77, 42, 7, &error));
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
encode_uid_map_response (const char *request_id, const char *uid_map_csv)
{
  g_auto (WyreboxDaemonStreamChunkFrame) chunk = { 0 };
  g_auto (WyreboxDaemonResponseFrame) frame = { 0 };
  g_autofree char *query_id = g_uuid_string_random ();
  g_autoptr (GBytes) csv_bytes = NULL;
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) response_payload = NULL;

  csv_bytes = g_bytes_new (uid_map_csv, strlen (uid_map_csv));
  g_assert_nonnull (csv_bytes);
  g_assert_true (wyrebox_daemon_stream_chunk_frame_init (&chunk,
          request_id, NULL, query_id, NULL, 0, csv_bytes, TRUE, &error));
  g_assert_no_error (error);

  g_assert_true (wyrebox_daemon_response_frame_init_stream_chunk (&frame,
          &chunk, &error));
  g_assert_no_error (error);

  response_payload = wyrebox_daemon_capnp_codec_encode_response_frame (&frame,
      NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (response_payload);

  return g_steal_pointer (&response_payload);
}
#endif

static gpointer
fake_server_thread_main (gpointer user_data)
{
  FakeServer *server = user_data;
  g_autoptr (GError) error = NULL;
  while (server->request_count < server->expected_request_count) {
    g_autoptr (GSocketConnection) connection = NULL;
    g_autoptr (GBytes) request = NULL;
    g_autoptr (GBytes) response = NULL;
    g_autofree char *request_id = NULL;
    gsize response_size = 0;
    GInputStream *input = NULL;
    GOutputStream *output = NULL;
    const guint8 *response_payload = NULL;

    connection =
        g_socket_listener_accept (server->listener, NULL, NULL, &error);
    g_assert_no_error (error);
    g_assert_nonnull (connection);

    input = g_io_stream_get_input_stream (G_IO_STREAM (connection));
    output = g_io_stream_get_output_stream (G_IO_STREAM (connection));
    g_assert_nonnull (input);
    g_assert_nonnull (output);

    request = wyrebox_daemon_frame_io_read_payload (input, &error);
    g_assert_no_error (error);

    switch (server->behavior) {
      case FAKE_SERVER_MAILBOX_SELECT_RESPONSE:
#if defined(WYREBOX_HAVE_CAPNP_SERIALIZATION) && WYREBOX_HAVE_CAPNP_SERIALIZATION
        request_id = assert_decoded_select_request (request);
        response = encode_mailbox_select_response (request_id);
#else
        g_assert_not_reached ();
#endif
        break;
      case FAKE_SERVER_DAEMON_ERROR_RESPONSE:
#if defined(WYREBOX_HAVE_CAPNP_SERIALIZATION) && WYREBOX_HAVE_CAPNP_SERIALIZATION
        request_id = assert_decoded_select_request (request);
        response = encode_daemon_error_response (request_id);
#else
        g_assert_not_reached ();
#endif
        break;
      case FAKE_SERVER_MAILBOX_SELECT_THEN_UID_MAP_RESPONSE:
        if (server->request_count == 0) {
#if defined(WYREBOX_HAVE_CAPNP_SERIALIZATION) && WYREBOX_HAVE_CAPNP_SERIALIZATION
          request_id = assert_decoded_select_request (request);
          response = encode_mailbox_select_response (request_id);
#else
          g_assert_not_reached ();
#endif
          break;
        }

        if (server->request_count == 1) {
          g_autofree gchar *uid_map_request_id = NULL;

          uid_map_request_id = assert_decoded_uid_map_request (request,
              server->expected_uid_map_mailbox_id != NULL
              ? server->expected_uid_map_mailbox_id : "view-projects");
          g_clear_pointer (&request_id, g_free);
          request_id = g_steal_pointer (&uid_map_request_id);
          g_assert_nonnull (server->uid_map_csv);
#if defined(WYREBOX_HAVE_CAPNP_SERIALIZATION) && WYREBOX_HAVE_CAPNP_SERIALIZATION
          response = encode_uid_map_response (request_id, server->uid_map_csv);
#else
          g_assert_not_reached ();
#endif
          break;
        }

        g_assert_not_reached ();
      default:
        g_assert_not_reached ();
    }

    response_payload = g_bytes_get_data (response, &response_size);
    g_assert_nonnull (response_payload);
    g_assert_true (wyrebox_daemon_frame_io_write_payload (output,
            response_payload, response_size, &error));
    g_assert_no_error (error);

    g_assert_true (g_io_stream_close (G_IO_STREAM (connection), NULL, &error));
    g_assert_no_error (error);
    server->request_count++;
  }

  return NULL;
}

static void
fake_server_start (FakeServer *server, const char *socket_path,
    FakeServerBehavior behavior,
    const char *uid_map_csv, const char *expected_uid_map_mailbox_id)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (GSocketAddress) address = NULL;

  server->listener = g_socket_listener_new ();
  server->behavior = behavior;
  server->uid_map_csv = uid_map_csv;
  server->expected_uid_map_mailbox_id = expected_uid_map_mailbox_id;
  server->expected_request_count =
      behavior == FAKE_SERVER_MAILBOX_SELECT_THEN_UID_MAP_RESPONSE ? 2 : 1;

  address = g_unix_socket_address_new (socket_path);
  g_assert_true (g_socket_listener_add_address (server->listener, address,
          G_SOCKET_TYPE_STREAM, G_SOCKET_PROTOCOL_DEFAULT, NULL, NULL, &error));
  g_assert_no_error (error);
  server->thread = g_thread_new ("wyrebox-plugin-mailbox-smoke-server",
      fake_server_thread_main, server);
}

static void
fake_server_join (FakeServer *server)
{
  if (server->thread != NULL) {
    g_thread_join (server->thread);
  }

  g_clear_object (&server->listener);
}
#endif

static struct mail_storage *
init_plugin_and_get_storage_class (void)
{
  wyrebox_dovecot_loader_shim_mail_storage_class = NULL;
  wyrebox_dovecot_loader_shim_mail_storage_class_register_calls = 0;
  wyrebox_dovecot_loader_shim_mail_storage_class_unregister_calls = 0;

  wyrebox_plugin_init (NULL);

  g_assert_true (wyrebox_dovecot_loader_shim_mail_storage_class_register_calls >
      0);
  g_assert_nonnull (wyrebox_dovecot_loader_shim_mail_storage_class);
  return wyrebox_dovecot_loader_shim_mail_storage_class;
}

static void
load_box (struct mail_storage *storage_class,
    struct mail_storage **storage_r, struct mailbox **box_r)
{
  struct mail_storage *storage;
  struct mail_user user = { "account-1", };
  struct mail_namespace ns = {
    .user = &user,
    .owner = &user,
  };
  struct mailbox *box = NULL;
  const char *error = NULL;

  storage = storage_class->v.alloc ();
  g_assert_nonnull (storage);
  g_assert_cmpint (storage->v.create (storage, &ns, &error), ==, 0);
  g_assert_null (error);
  box = storage->v.mailbox_alloc (storage, NULL, "Projects", 0);
  g_assert_nonnull (box);

  *storage_r = storage;
  *box_r = box;
}

static void
close_unload_box_and_plugin (struct mail_storage *storage, struct mailbox *box)
{
  if (box != NULL) {
    box->v.close (box);
    box->v.free (box);
  }

  if (storage != NULL) {
    storage->v.destroy (storage);
  }

  wyrebox_plugin_deinit ();
  g_assert_true (wyrebox_dovecot_loader_shim_mail_storage_class_unregister_calls
      > 0);
}

static void
test_open_and_get_status_after_open (void)
{
  g_autofree char *socket_root = NULL;
  g_autofree char *socket_path = NULL;
  FakeServer server = { 0 };
  struct mailbox *box = NULL;
  struct mail_storage *storage = NULL;
  struct mailbox_status status = {
    .uidvalidity = 1,
    .uidnext = 1,
    .messages = 1,
  };
  const char *uid_map_csv =
      "account_id,mailbox_id,uidvalidity,uid,message_id,object_id\n"
      "account-1,view-projects,77,42,message-1,object-1\n";
  struct mail_storage *storage_class = NULL;

#if defined(WYREBOX_HAVE_CAPNP_SERIALIZATION) && WYREBOX_HAVE_CAPNP_SERIALIZATION
  g_autofree char *socket_path_local = make_socket_path (&socket_root);
  fake_server_start (&server, socket_path_local,
      FAKE_SERVER_MAILBOX_SELECT_THEN_UID_MAP_RESPONSE, uid_map_csv,
      "view-projects");
  socket_path = g_steal_pointer (&socket_path_local);

  wyrebox_dovecot_test_daemon_socket_path = socket_path;
  storage_class = init_plugin_and_get_storage_class ();
  load_box (storage_class, &storage, &box);

  g_assert_false (box->opened);
  g_assert_cmpint (box->v.open (box), ==, 0);
  g_assert_true (box->opened);

  g_assert_cmpint (box->v.get_status (box,
          STATUS_UIDVALIDITY | STATUS_UIDNEXT | STATUS_MESSAGES, &status), ==,
      0);
  g_assert_cmpuint (status.uidvalidity, ==, 77);
  g_assert_cmpuint (status.uidnext, ==, 42);
  g_assert_cmpuint (status.messages, ==, 7);
  g_assert_cmpuint (server.request_count, ==, 2);

  fake_server_join (&server);
  remove_tree (socket_root);

  wyrebox_dovecot_test_daemon_socket_path = NULL;
  close_unload_box_and_plugin (storage, box);
#else
  g_test_skip ("CAPNP serialization is disabled");
#endif
}

static void
test_lazy_status_before_open (void)
{
  g_autofree char *socket_root = NULL;
  g_autofree char *socket_path = NULL;
  FakeServer server = { 0 };
  struct mailbox *box = NULL;
  struct mail_storage *storage = NULL;
  struct mailbox_status status = {
    .uidvalidity = 1,
    .uidnext = 1,
    .messages = 1,
  };
  const char *uid_map_csv =
      "account_id,mailbox_id,uidvalidity,uid,message_id,object_id\n"
      "account-1,view-projects,77,42,message-1,object-1\n";
  struct mail_storage *storage_class = NULL;

#if defined(WYREBOX_HAVE_CAPNP_SERIALIZATION) && WYREBOX_HAVE_CAPNP_SERIALIZATION
  g_autofree char *socket_path_local = make_socket_path (&socket_root);
  fake_server_start (&server, socket_path_local,
      FAKE_SERVER_MAILBOX_SELECT_THEN_UID_MAP_RESPONSE, uid_map_csv,
      "view-projects");
  socket_path = g_steal_pointer (&socket_path_local);

  wyrebox_dovecot_test_daemon_socket_path = socket_path;
  storage_class = init_plugin_and_get_storage_class ();
  load_box (storage_class, &storage, &box);

  g_assert_false (box->opened);
  g_assert_cmpint (box->v.get_status (box,
          STATUS_UIDVALIDITY | STATUS_UIDNEXT | STATUS_MESSAGES, &status), ==,
      0);
  g_assert_cmpuint (status.uidvalidity, ==, 77);
  g_assert_cmpuint (status.uidnext, ==, 42);
  g_assert_cmpuint (status.messages, ==, 7);
  g_assert_false (box->opened);
  g_assert_cmpuint (server.request_count, ==, 2);

  fake_server_join (&server);
  remove_tree (socket_root);

  wyrebox_dovecot_test_daemon_socket_path = NULL;
  close_unload_box_and_plugin (storage, box);
#else
  g_test_skip ("CAPNP serialization is disabled");
#endif
}

static void
test_open_fails_with_missing_socket_clears_state (void)
{
  g_autofree char *socket_root = NULL;
  g_autofree char *missing_socket_path = NULL;
  struct mailbox *box = NULL;
  struct mail_storage *storage = NULL;
  struct mail_storage *storage_class = NULL;

#if defined(WYREBOX_HAVE_CAPNP_SERIALIZATION) && WYREBOX_HAVE_CAPNP_SERIALIZATION
  missing_socket_path = make_socket_path (&socket_root);
  wyrebox_dovecot_test_daemon_socket_path = missing_socket_path;
  storage_class = init_plugin_and_get_storage_class ();
  load_box (storage_class, &storage, &box);

  g_assert_cmpint (box->v.open (box), ==, -1);
  g_assert_false (box->opened);

  wyrebox_dovecot_test_daemon_socket_path = NULL;
  close_unload_box_and_plugin (storage, box);
  remove_tree (socket_root);
#else
  g_test_skip ("CAPNP serialization is disabled");
#endif
}

static void
test_open_fails_with_daemon_error (void)
{
  g_autofree char *socket_root = NULL;
  g_autofree char *socket_path = NULL;
  FakeServer server = { 0 };
  struct mailbox *box = NULL;
  struct mail_storage *storage = NULL;
  struct mail_storage *storage_class = NULL;

#if defined(WYREBOX_HAVE_CAPNP_SERIALIZATION) && WYREBOX_HAVE_CAPNP_SERIALIZATION
  g_autofree char *socket_path_local = make_socket_path (&socket_root);
  fake_server_start (&server, socket_path_local,
      FAKE_SERVER_DAEMON_ERROR_RESPONSE, NULL, NULL);
  socket_path = g_steal_pointer (&socket_path_local);

  wyrebox_dovecot_test_daemon_socket_path = socket_path;
  storage_class = init_plugin_and_get_storage_class ();
  load_box (storage_class, &storage, &box);

  g_assert_cmpint (box->v.open (box), ==, -1);
  g_assert_false (box->opened);

  fake_server_join (&server);
  remove_tree (socket_root);

  wyrebox_dovecot_test_daemon_socket_path = NULL;
  close_unload_box_and_plugin (storage, box);
#else
  g_test_skip ("CAPNP serialization is disabled");
#endif
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/dovecot/plugin-mailbox-smoke/open-get-status-after-open",
      test_open_and_get_status_after_open);
  g_test_add_func ("/dovecot/plugin-mailbox-smoke/lazy-status-before-open",
      test_lazy_status_before_open);
  g_test_add_func
      ("/dovecot/plugin-mailbox-smoke/open-fails-with-missing-socket",
      test_open_fails_with_missing_socket_clears_state);
  g_test_add_func ("/dovecot/plugin-mailbox-smoke/open-fails-with-daemon-error",
      test_open_fails_with_daemon_error);

  return g_test_run ();
}
