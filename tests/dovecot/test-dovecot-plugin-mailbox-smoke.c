#include "config.h"

#include "lib.h"
#include "mail-namespace.h"
#include "mail-storage-private.h"
#include "mail-storage.h"
#include "mail-user.h"
#include "mailbox-list-private.h"
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

typedef gboolean (*WyreboxDovecotMailboxListPublishFunc) (struct mailbox_list
    * list, const char *name, char hierarchy_delimiter, gboolean selectable,
    enum mailbox_list_child_state child_state, const char *special_use,
    gpointer user_data);

extern gboolean wyrebox_dovecot_publish_mailbox_list_result (struct mailbox_list
    *list, const WyreboxDaemonMailboxListResult * result,
    WyreboxDovecotMailboxListPublishFunc publisher, gpointer publisher_data,
    GError ** error);

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
  WyreboxDaemonMailboxListEntryKind expected_uid_map_kind;
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

static gboolean
publish_mailbox_list_entry_to_sink (struct mailbox_list *list,
    const char *name, char hierarchy_delimiter, gboolean selectable,
    enum mailbox_list_child_state child_state, const char *special_use,
    gpointer user_data)
{
  g_assert_null (user_data);
  return mailbox_list_sink_publish_entry (list, name, hierarchy_delimiter,
      selectable, child_state, special_use);
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
    const char *expected_mailbox_id,
    WyreboxDaemonMailboxListEntryKind expected_kind, char **out_query_id)
{
  g_autoptr (GError) error = NULL;
  WyreboxDaemonDecodedRequestFrame decoded = { 0 };
  gpointer decoded_state = NULL;
  GDestroyNotify decoded_state_clear = NULL;
  char *request_id = NULL;
  const char *expected_template_id = NULL;

  expected_template_id = expected_kind ==
      WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_VIRTUAL
      ? "derived_view.uid_map.v1" : "mailbox.uid_map.v1";

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
      expected_template_id);
  g_assert_cmpstr (decoded.duckdb_query_template->scope_id, ==, "account-1");
  g_assert_nonnull (decoded.duckdb_query_template->parameters);
  g_assert_cmpstr (decoded.duckdb_query_template->parameters[0], ==,
      expected_mailbox_id);
  g_assert_null (decoded.duckdb_query_template->parameters[1]);
  g_assert_nonnull (decoded.duckdb_query_template->query_id);

  g_assert_nonnull (decoded_state_clear);
  g_assert_nonnull (decoded_state);
  if (out_query_id != NULL)
    *out_query_id = g_strdup (decoded.duckdb_query_template->query_id);
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
encode_uid_map_response (const char *request_id, const char *query_id,
    const char *uid_map_csv)
{
  g_auto (WyreboxDaemonStreamChunkFrame) chunk = { 0 };
  g_auto (WyreboxDaemonResponseFrame) frame = { 0 };
  g_autofree char *allocated_query_id = g_uuid_string_random ();
  const char *response_query_id =
      query_id != NULL ? query_id : allocated_query_id;
  g_autoptr (GBytes) csv_bytes = NULL;
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) response_payload = NULL;

  csv_bytes = g_bytes_new (uid_map_csv, strlen (uid_map_csv));
  g_assert_nonnull (csv_bytes);
  g_assert_true (wyrebox_daemon_stream_chunk_frame_init (&chunk,
          request_id, NULL, response_query_id, NULL, 0, csv_bytes, TRUE,
          &error));
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
    g_autofree char *query_id = NULL;
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
              ? server->expected_uid_map_mailbox_id : "view-projects",
              server->expected_uid_map_kind, &query_id);
          g_clear_pointer (&request_id, g_free);
          request_id = g_steal_pointer (&uid_map_request_id);
          g_assert_nonnull (server->uid_map_csv);
#if defined(WYREBOX_HAVE_CAPNP_SERIALIZATION) && WYREBOX_HAVE_CAPNP_SERIALIZATION
          response = encode_uid_map_response (request_id, query_id,
              server->uid_map_csv);
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
    const char *uid_map_csv, const char *expected_uid_map_mailbox_id,
    WyreboxDaemonMailboxListEntryKind expected_uid_map_kind)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (GSocketAddress) address = NULL;

  server->listener = g_socket_listener_new ();
  server->behavior = behavior;
  server->uid_map_csv = uid_map_csv;
  server->expected_uid_map_mailbox_id = expected_uid_map_mailbox_id;
  server->expected_uid_map_kind = expected_uid_map_kind;
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
test_registered_storage_installs_add_list_hooks_without_socket_io (void)
{
  const char *patterns[] = { "*", NULL };
  struct mailbox_list *list = NULL;
  struct mailbox_list *other_list = NULL;
  struct mailbox_list_iterate_context *ctx = NULL;
  struct mail_storage *storage_class = NULL;
  struct mailbox_list_vfuncs original_vfuncs;
  struct mailbox_list_vfuncs other_original_vfuncs;
  struct mailbox_list_vfuncs *original_vlast = NULL;
  struct mailbox_list_vfuncs *other_original_vlast = NULL;

  storage_class = init_plugin_and_get_storage_class ();
  g_assert_nonnull (storage_class->v.add_list);

  list = mailbox_list_sink_alloc ();
  other_list = mailbox_list_sink_alloc ();
  g_assert_nonnull (list);
  g_assert_nonnull (other_list);

  original_vfuncs = list->v;
  original_vlast = list->vlast;
  other_original_vfuncs = other_list->v;
  other_original_vlast = other_list->vlast;

  wyrebox_dovecot_test_daemon_socket_path =
      "/tmp/wyrebox-add-list-must-not-connect.sock";
  storage_class->v.add_list (storage_class, list);
  wyrebox_dovecot_test_daemon_socket_path = NULL;

  g_assert_true (list->v.iter_init != original_vfuncs.iter_init);
  g_assert_true (list->v.iter_next != original_vfuncs.iter_next);
  g_assert_true (list->v.iter_deinit != original_vfuncs.iter_deinit);
  g_assert_true (list->v.deinit != original_vfuncs.deinit);
  g_assert_nonnull (list->vlast);
  g_assert_true (list->vlast != original_vlast);
  g_assert_true (list->vlast->iter_init == original_vfuncs.iter_init);
  g_assert_true (list->vlast->iter_next == original_vfuncs.iter_next);
  g_assert_true (list->vlast->iter_deinit == original_vfuncs.iter_deinit);
  g_assert_true (list->vlast->deinit == original_vfuncs.deinit);

  g_assert_true (other_list->v.iter_init == other_original_vfuncs.iter_init);
  g_assert_true (other_list->v.iter_next == other_original_vfuncs.iter_next);
  g_assert_true (other_list->v.iter_deinit ==
      other_original_vfuncs.iter_deinit);
  g_assert_true (other_list->v.deinit == other_original_vfuncs.deinit);
  g_assert_true (other_list->vlast == other_original_vlast);

  ctx = list->v.iter_init (list, patterns, MAILBOX_LIST_ITER_RETURN_CHILDREN);
  g_assert_nonnull (ctx);
  g_assert_cmpuint (mailbox_list_sink_get_original_iter_init_calls (list), ==,
      1);
  g_assert_null (list->v.iter_next (ctx));
  g_assert_cmpuint (mailbox_list_sink_get_original_iter_next_calls (list), ==,
      1);
  g_assert_cmpint (list->v.iter_deinit (ctx), ==, 0);
  g_assert_cmpuint (mailbox_list_sink_get_original_iter_deinit_calls (list),
      ==, 1);
  g_assert_cmpuint (mailbox_list_sink_get_original_iter_init_calls
      (other_list), ==, 0);

  list->v.deinit (list);
  g_assert_cmpuint (mailbox_list_sink_get_original_deinit_calls (list), ==, 1);
  g_assert_true (list->v.iter_init == original_vfuncs.iter_init);
  g_assert_true (list->v.iter_next == original_vfuncs.iter_next);
  g_assert_true (list->v.iter_deinit == original_vfuncs.iter_deinit);
  g_assert_true (list->v.deinit == original_vfuncs.deinit);
  g_assert_true (list->vlast == original_vlast);

  mailbox_list_sink_free (list);
  mailbox_list_sink_free (other_list);

  wyrebox_plugin_deinit ();
  g_assert_true (wyrebox_dovecot_loader_shim_mail_storage_class_unregister_calls
      > 0);
}

static void
assert_mailbox_list_vfuncs_restored (struct mailbox_list *list,
    const struct mailbox_list_vfuncs *original_vfuncs,
    struct mailbox_list_vfuncs *original_vlast)
{
  g_assert_true (list->v.iter_init == original_vfuncs->iter_init);
  g_assert_true (list->v.iter_next == original_vfuncs->iter_next);
  g_assert_true (list->v.iter_deinit == original_vfuncs->iter_deinit);
  g_assert_true (list->v.deinit == original_vfuncs->deinit);
  g_assert_true (list->vlast == original_vlast);
}

static void
assert_mailbox_list_uses_original_vfuncs_as_sink (struct mailbox_list *list,
    const struct mailbox_list_vfuncs *original_vfuncs,
    struct mailbox_list_vfuncs *original_vlast)
{
  g_assert_true (list->v.iter_init != original_vfuncs->iter_init);
  g_assert_true (list->v.iter_next != original_vfuncs->iter_next);
  g_assert_true (list->v.iter_deinit != original_vfuncs->iter_deinit);
  g_assert_true (list->v.deinit != original_vfuncs->deinit);
  g_assert_nonnull (list->vlast);
  g_assert_true (list->vlast != original_vlast);
  g_assert_true (list->vlast->iter_init == original_vfuncs->iter_init);
  g_assert_true (list->vlast->iter_next == original_vfuncs->iter_next);
  g_assert_true (list->vlast->iter_deinit == original_vfuncs->iter_deinit);
  g_assert_true (list->vlast->deinit == original_vfuncs->deinit);
}

static void
test_plugin_deinit_restores_list_hooks_before_list_deinit (void)
{
  struct mailbox_list *list = NULL;
  struct mail_storage *storage_class = NULL;
  struct mailbox_list_vfuncs original_vfuncs;
  struct mailbox_list_vfuncs *original_vlast = NULL;

  storage_class = init_plugin_and_get_storage_class ();
  g_assert_nonnull (storage_class->v.add_list);

  list = mailbox_list_sink_alloc ();
  g_assert_nonnull (list);

  original_vfuncs = list->v;
  original_vlast = list->vlast;

  storage_class->v.add_list (storage_class, list);
  assert_mailbox_list_uses_original_vfuncs_as_sink (list, &original_vfuncs,
      original_vlast);

  wyrebox_plugin_deinit ();
  g_assert_true (wyrebox_dovecot_loader_shim_mail_storage_class_unregister_calls
      > 0);
  g_assert_cmpuint (mailbox_list_sink_get_original_deinit_calls (list), ==, 0);
  assert_mailbox_list_vfuncs_restored (list, &original_vfuncs, original_vlast);

  list->v.deinit (list);
  g_assert_cmpuint (mailbox_list_sink_get_original_deinit_calls (list), ==, 1);

  mailbox_list_sink_free (list);
}

static void
test_plugin_reload_rehooks_same_list_with_original_sink_vfuncs (void)
{
  const char *patterns[] = { "*", NULL };
  struct mailbox_list *list = NULL;
  struct mailbox_list_iterate_context *ctx = NULL;
  struct mail_storage *storage_class = NULL;
  struct mailbox_list_vfuncs original_vfuncs;
  struct mailbox_list_vfuncs *original_vlast = NULL;

  storage_class = init_plugin_and_get_storage_class ();
  g_assert_nonnull (storage_class->v.add_list);

  list = mailbox_list_sink_alloc ();
  g_assert_nonnull (list);

  original_vfuncs = list->v;
  original_vlast = list->vlast;

  storage_class->v.add_list (storage_class, list);
  wyrebox_plugin_deinit ();
  assert_mailbox_list_vfuncs_restored (list, &original_vfuncs, original_vlast);

  storage_class = init_plugin_and_get_storage_class ();
  g_assert_nonnull (storage_class->v.add_list);

  storage_class->v.add_list (storage_class, list);
  assert_mailbox_list_uses_original_vfuncs_as_sink (list, &original_vfuncs,
      original_vlast);

  ctx = list->v.iter_init (list, patterns, MAILBOX_LIST_ITER_RETURN_CHILDREN);
  g_assert_nonnull (ctx);
  g_assert_cmpuint (mailbox_list_sink_get_original_iter_init_calls (list), ==,
      1);
  g_assert_null (list->v.iter_next (ctx));
  g_assert_cmpuint (mailbox_list_sink_get_original_iter_next_calls (list), ==,
      1);
  g_assert_cmpint (list->v.iter_deinit (ctx), ==, 0);
  g_assert_cmpuint (mailbox_list_sink_get_original_iter_deinit_calls (list),
      ==, 1);

  list->v.deinit (list);
  g_assert_cmpuint (mailbox_list_sink_get_original_deinit_calls (list), ==, 1);

  mailbox_list_sink_free (list);

  wyrebox_plugin_deinit ();
  g_assert_true (wyrebox_dovecot_loader_shim_mail_storage_class_unregister_calls
      > 0);
}

static void
test_mailbox_list_sink_captures_published_entries (void)
{
  struct mailbox_list *list = NULL;
  const struct mailbox_list_sink_entry *entry = NULL;

  list = mailbox_list_sink_alloc ();
  g_assert_nonnull (list);

  g_assert_true (mailbox_list_sink_publish_entry (list, "INBOX", '/',
          TRUE, MAILBOX_LIST_CHILD_STATE_HAS_NO_CHILDREN, "\\Inbox"));
  g_assert_true (mailbox_list_sink_publish_entry (list,
          "Virtual/Projects", '/', FALSE,
          MAILBOX_LIST_CHILD_STATE_HAS_CHILDREN, NULL));

  g_assert_cmpuint (mailbox_list_sink_get_count (list), ==, 2);

  entry = mailbox_list_sink_get_entry (list, 0);
  g_assert_nonnull (entry);
  g_assert_cmpstr (entry->name, ==, "INBOX");
  g_assert_cmpint (entry->hierarchy_delimiter, ==, '/');
  g_assert_true (entry->selectable);
  g_assert_cmpint (entry->child_state, ==,
      MAILBOX_LIST_CHILD_STATE_HAS_NO_CHILDREN);
  g_assert_cmpstr (entry->special_use, ==, "\\Inbox");

  entry = mailbox_list_sink_get_entry (list, 1);
  g_assert_nonnull (entry);
  g_assert_cmpstr (entry->name, ==, "Virtual/Projects");
  g_assert_cmpint (entry->hierarchy_delimiter, ==, '/');
  g_assert_false (entry->selectable);
  g_assert_cmpint (entry->child_state, ==,
      MAILBOX_LIST_CHILD_STATE_HAS_CHILDREN);
  g_assert_null (entry->special_use);

  g_assert_null (mailbox_list_sink_get_entry (list, 2));

  mailbox_list_sink_free (list);
}

static void
test_publish_mailbox_list_result_maps_entries (void)
{
  g_auto (WyreboxDaemonMailboxListResult) result = { 0 };
  g_autoptr (GError) error = NULL;
  struct mailbox_list *list = NULL;
  const struct mailbox_list_sink_entry *entry = NULL;

  wyrebox_daemon_mailbox_list_result_init_empty (&result);
  g_assert_true (wyrebox_daemon_mailbox_list_result_append_entry (&result,
          WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_ORDINARY,
          "mailbox-inbox", "INBOX", "/", "\\Inbox", TRUE,
          WYREBOX_DAEMON_MAILBOX_LIST_CHILD_STATE_HAS_NO_CHILDREN, &error));
  g_assert_no_error (error);
  g_assert_true (wyrebox_daemon_mailbox_list_result_append_entry (&result,
          WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_VIRTUAL,
          "view-projects", "Virtual/Projects", "/", NULL, FALSE,
          WYREBOX_DAEMON_MAILBOX_LIST_CHILD_STATE_HAS_CHILDREN, &error));
  g_assert_no_error (error);

  list = mailbox_list_sink_alloc ();
  g_assert_nonnull (list);
  g_assert_true (wyrebox_dovecot_publish_mailbox_list_result (list, &result,
          publish_mailbox_list_entry_to_sink, NULL, &error));
  g_assert_no_error (error);

  g_assert_cmpuint (mailbox_list_sink_get_count (list), ==, 2);

  entry = mailbox_list_sink_get_entry (list, 0);
  g_assert_nonnull (entry);
  g_assert_cmpstr (entry->name, ==, "INBOX");
  g_assert_cmpint (entry->hierarchy_delimiter, ==, '/');
  g_assert_true (entry->selectable);
  g_assert_cmpint (entry->child_state, ==,
      MAILBOX_LIST_CHILD_STATE_HAS_NO_CHILDREN);
  g_assert_cmpstr (entry->special_use, ==, "\\Inbox");

  entry = mailbox_list_sink_get_entry (list, 1);
  g_assert_nonnull (entry);
  g_assert_cmpstr (entry->name, ==, "Virtual/Projects");
  g_assert_cmpint (entry->hierarchy_delimiter, ==, '/');
  g_assert_false (entry->selectable);
  g_assert_cmpint (entry->child_state, ==,
      MAILBOX_LIST_CHILD_STATE_HAS_CHILDREN);
  g_assert_null (entry->special_use);

  mailbox_list_sink_free (list);
}

static void
test_publish_mailbox_list_result_accepts_empty_result (void)
{
  g_auto (WyreboxDaemonMailboxListResult) result = { 0 };
  g_autoptr (GError) error = NULL;
  struct mailbox_list *list = NULL;

  wyrebox_daemon_mailbox_list_result_init_empty (&result);
  list = mailbox_list_sink_alloc ();
  g_assert_nonnull (list);

  g_assert_true (wyrebox_dovecot_publish_mailbox_list_result (list, &result,
          publish_mailbox_list_entry_to_sink, NULL, &error));
  g_assert_no_error (error);
  g_assert_cmpuint (mailbox_list_sink_get_count (list), ==, 0);

  mailbox_list_sink_free (list);
}

static void
test_publish_mailbox_list_result_rejects_null_inputs (void)
{
  g_auto (WyreboxDaemonMailboxListResult) result = { 0 };
  g_autoptr (GError) error = NULL;
  struct mailbox_list *list = NULL;

  wyrebox_daemon_mailbox_list_result_init_empty (&result);
  list = mailbox_list_sink_alloc ();
  g_assert_nonnull (list);

  g_assert_false (wyrebox_dovecot_publish_mailbox_list_result (NULL, &result,
          publish_mailbox_list_entry_to_sink, NULL, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_clear_error (&error);

  g_assert_false (wyrebox_dovecot_publish_mailbox_list_result (list, NULL,
          publish_mailbox_list_entry_to_sink, NULL, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_clear_error (&error);

  g_assert_false (wyrebox_dovecot_publish_mailbox_list_result (list, &result,
          NULL, NULL, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);

  mailbox_list_sink_free (list);
}

static void
test_publish_mailbox_list_result_rejects_invalid_delimiter (void)
{
  g_auto (WyreboxDaemonMailboxListResult) result = { 0 };
  g_autoptr (GError) error = NULL;
  struct mailbox_list *list = NULL;
  WyreboxDaemonMailboxListEntry *entry = NULL;

  wyrebox_daemon_mailbox_list_result_init_empty (&result);
  g_assert_true (wyrebox_daemon_mailbox_list_result_append_entry (&result,
          WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_ORDINARY,
          "mailbox-inbox", "INBOX", "/", "\\Inbox", TRUE,
          WYREBOX_DAEMON_MAILBOX_LIST_CHILD_STATE_HAS_NO_CHILDREN, &error));
  g_assert_no_error (error);

  entry = g_ptr_array_index (result.entries, 0);
  g_assert_nonnull (entry);
  g_free (entry->hierarchy_delimiter);
  entry->hierarchy_delimiter = g_strdup ("//");

  list = mailbox_list_sink_alloc ();
  g_assert_nonnull (list);
  g_assert_false (wyrebox_dovecot_publish_mailbox_list_result (list, &result,
          publish_mailbox_list_entry_to_sink, NULL, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
  g_assert_cmpuint (mailbox_list_sink_get_count (list), ==, 0);

  mailbox_list_sink_free (list);
}

static void
test_publish_mailbox_list_result_rejects_invalid_child_state (void)
{
  g_auto (WyreboxDaemonMailboxListResult) result = { 0 };
  g_autoptr (GError) error = NULL;
  struct mailbox_list *list = NULL;
  WyreboxDaemonMailboxListEntry *entry = NULL;

  wyrebox_daemon_mailbox_list_result_init_empty (&result);
  g_assert_true (wyrebox_daemon_mailbox_list_result_append_entry (&result,
          WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_ORDINARY,
          "mailbox-inbox", "INBOX", "/", "\\Inbox", TRUE,
          WYREBOX_DAEMON_MAILBOX_LIST_CHILD_STATE_HAS_NO_CHILDREN, &error));
  g_assert_no_error (error);

  entry = g_ptr_array_index (result.entries, 0);
  g_assert_nonnull (entry);
  entry->child_state = (WyreboxDaemonMailboxListChildState) 999;

  list = mailbox_list_sink_alloc ();
  g_assert_nonnull (list);
  g_assert_false (wyrebox_dovecot_publish_mailbox_list_result (list, &result,
          publish_mailbox_list_entry_to_sink, NULL, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
  g_assert_cmpuint (mailbox_list_sink_get_count (list), ==, 0);

  mailbox_list_sink_free (list);
}

static void
test_publish_mailbox_list_result_validates_before_publishing (void)
{
  g_auto (WyreboxDaemonMailboxListResult) result = { 0 };
  g_autoptr (GError) error = NULL;
  struct mailbox_list *list = NULL;
  WyreboxDaemonMailboxListEntry *entry = NULL;

  wyrebox_daemon_mailbox_list_result_init_empty (&result);
  g_assert_true (wyrebox_daemon_mailbox_list_result_append_entry (&result,
          WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_ORDINARY,
          "mailbox-inbox", "INBOX", "/", "\\Inbox", TRUE,
          WYREBOX_DAEMON_MAILBOX_LIST_CHILD_STATE_HAS_NO_CHILDREN, &error));
  g_assert_no_error (error);
  g_assert_true (wyrebox_daemon_mailbox_list_result_append_entry (&result,
          WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_VIRTUAL,
          "view-projects", "Virtual/Projects", "/", NULL, FALSE,
          WYREBOX_DAEMON_MAILBOX_LIST_CHILD_STATE_HAS_CHILDREN, &error));
  g_assert_no_error (error);

  entry = g_ptr_array_index (result.entries, 1);
  g_assert_nonnull (entry);
  entry->child_state = (WyreboxDaemonMailboxListChildState) 999;

  list = mailbox_list_sink_alloc ();
  g_assert_nonnull (list);
  g_assert_false (wyrebox_dovecot_publish_mailbox_list_result (list, &result,
          publish_mailbox_list_entry_to_sink, NULL, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
  g_assert_cmpuint (mailbox_list_sink_get_count (list), ==, 0);

  mailbox_list_sink_free (list);
}

static void
test_publish_mailbox_list_result_reports_publish_failure (void)
{
  g_auto (WyreboxDaemonMailboxListResult) result = { 0 };
  g_autoptr (GError) error = NULL;
  struct mailbox_list *list = NULL;

  wyrebox_daemon_mailbox_list_result_init_empty (&result);
  g_assert_true (wyrebox_daemon_mailbox_list_result_append_entry (&result,
          WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_ORDINARY,
          "mailbox-inbox", "INBOX", "/", "\\Inbox", TRUE,
          WYREBOX_DAEMON_MAILBOX_LIST_CHILD_STATE_HAS_NO_CHILDREN, &error));
  g_assert_no_error (error);

  list = mailbox_list_sink_alloc ();
  g_assert_nonnull (list);
  mailbox_list_sink_fail_next_publish (list);

  g_assert_false (wyrebox_dovecot_publish_mailbox_list_result (list, &result,
          publish_mailbox_list_entry_to_sink, NULL, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_FAILED);
  g_assert_cmpuint (mailbox_list_sink_get_count (list), ==, 0);

  mailbox_list_sink_free (list);
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
      "account_id,view_id,uidvalidity,uid,message_id,object_id,rule_version_hash\n"
      "account-1,view-projects,77,42,message-1,object-1,hash-1\n";
  struct mail_storage *storage_class = NULL;

#if defined(WYREBOX_HAVE_CAPNP_SERIALIZATION) && WYREBOX_HAVE_CAPNP_SERIALIZATION
  g_autofree char *socket_path_local = make_socket_path (&socket_root);
  fake_server_start (&server, socket_path_local,
      FAKE_SERVER_MAILBOX_SELECT_THEN_UID_MAP_RESPONSE, uid_map_csv,
      "view-projects", WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_VIRTUAL);
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

  fake_server_join (&server);
  g_assert_cmpuint (server.request_count, ==, 2);
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
      "account_id,view_id,uidvalidity,uid,message_id,object_id,rule_version_hash\n"
      "account-1,view-projects,77,42,message-1,object-1,hash-1\n";
  struct mail_storage *storage_class = NULL;

#if defined(WYREBOX_HAVE_CAPNP_SERIALIZATION) && WYREBOX_HAVE_CAPNP_SERIALIZATION
  g_autofree char *socket_path_local = make_socket_path (&socket_root);
  fake_server_start (&server, socket_path_local,
      FAKE_SERVER_MAILBOX_SELECT_THEN_UID_MAP_RESPONSE, uid_map_csv,
      "view-projects", WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_VIRTUAL);
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

  fake_server_join (&server);
  g_assert_cmpuint (server.request_count, ==, 2);
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
      FAKE_SERVER_DAEMON_ERROR_RESPONSE, NULL, NULL,
      WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_ORDINARY);
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

  g_test_add_func ("/dovecot/plugin-mailbox-smoke/storage-add-list-hooks",
      test_registered_storage_installs_add_list_hooks_without_socket_io);
  g_test_add_func ("/dovecot/plugin-mailbox-smoke/unload-restores-list-hooks",
      test_plugin_deinit_restores_list_hooks_before_list_deinit);
  g_test_add_func ("/dovecot/plugin-mailbox-smoke/reload-rehooks-list",
      test_plugin_reload_rehooks_same_list_with_original_sink_vfuncs);
  g_test_add_func ("/dovecot/plugin-mailbox-smoke/list-sink-captures-entries",
      test_mailbox_list_sink_captures_published_entries);
  g_test_add_func ("/dovecot/plugin-mailbox-smoke/list-publish-maps-entries",
      test_publish_mailbox_list_result_maps_entries);
  g_test_add_func ("/dovecot/plugin-mailbox-smoke/list-publish-empty-result",
      test_publish_mailbox_list_result_accepts_empty_result);
  g_test_add_func ("/dovecot/plugin-mailbox-smoke/list-publish-null-inputs",
      test_publish_mailbox_list_result_rejects_null_inputs);
  g_test_add_func
      ("/dovecot/plugin-mailbox-smoke/list-publish-invalid-delimiter",
      test_publish_mailbox_list_result_rejects_invalid_delimiter);
  g_test_add_func
      ("/dovecot/plugin-mailbox-smoke/list-publish-invalid-child-state",
      test_publish_mailbox_list_result_rejects_invalid_child_state);
  g_test_add_func
      ("/dovecot/plugin-mailbox-smoke/list-publish-validates-before-publish",
      test_publish_mailbox_list_result_validates_before_publishing);
  g_test_add_func ("/dovecot/plugin-mailbox-smoke/list-publish-failure",
      test_publish_mailbox_list_result_reports_publish_failure);
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
