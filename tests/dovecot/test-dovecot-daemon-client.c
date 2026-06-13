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
  FAKE_SERVER_MESSAGE_FETCH_RESPONSE,
  FAKE_SERVER_DAEMON_ERROR_RESPONSE,
  FAKE_SERVER_UNEXPECTED_SUCCESS_RESPONSE,
  FAKE_SERVER_UID_MAP_UNEXPECTED_SUCCESS_RESPONSE,
  FAKE_SERVER_CLOSE_WITHOUT_RESPONSE,
  FAKE_SERVER_TRUNCATED_RESPONSE,
  FAKE_SERVER_UID_MAP_CSV_RESPONSE,
} FakeServerBehavior;

typedef struct
{
  GSocketListener *listener;
  GThread *thread;
  FakeServerBehavior behavior;
  const guint8 *response;
  gsize response_size;
  const char *response_request_id_override;
  const char *uid_map_response_query_id_override;
  const char *uid_map_csv;
  const char *expected_uid_map_mailbox_id;
  WyreboxDaemonMailboxListEntryKind expected_uid_map_kind;
  const char *expected_fetch_account_id;
  const char *expected_fetch_mailbox_id;
  guint64 expected_fetch_uid_validity;
  guint64 expected_fetch_mailbox_uid;
  const char *message_fetch_response_message_id;
  const char *message_fetch_response_query_id;
  const guint8 *message_fetch_response_payload;
  gsize message_fetch_response_payload_size;
  guint64 message_fetch_response_chunk_index;
  gboolean message_fetch_response_end_of_stream;
  gboolean expect_message_fetch_request;
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
  g_assert_cmpuint (result->message_count, ==, 0);
}

static void
assert_uid_map_snapshot_is_cleared (const WyreboxDovecotMailboxUidMapSnapshot
    *snapshot)
{
  g_assert_null (snapshot->rows);
}

static void
assert_uid_map_rows (const WyreboxDovecotMailboxUidMapSnapshot *snapshot,
    const gchar *account_id,
    const gchar *mailbox_id,
    guint64 uid_validity,
    guint row_count,
    const gchar *first_uid,
    const gchar *first_message_id,
    const gchar *first_object_id,
    const gchar *second_uid,
    const gchar *second_message_id, const gchar *second_object_id)
{
  g_assert_nonnull (snapshot->rows);
  g_assert_cmpuint (snapshot->rows->len, ==, row_count);

  if (row_count > 0) {
    const WyreboxDovecotMailboxUidMapRow *first =
        g_ptr_array_index (snapshot->rows, 0);
    g_assert_cmpstr (first->account_id, ==, account_id);
    g_assert_cmpstr (first->mailbox_id, ==, mailbox_id);
    g_assert_cmpuint (first->uid_validity, ==, uid_validity);
    g_assert_cmpstr (first->message_id, ==, first_message_id);
    g_assert_cmpstr (first->object_id, ==, first_object_id);
    g_assert_cmpuint (first->uid, ==, g_ascii_strtoull (first_uid, NULL, 10));
  }

  if (row_count > 1) {
    const WyreboxDovecotMailboxUidMapRow *second =
        g_ptr_array_index (snapshot->rows, 1);
    g_assert_cmpuint (second->uid, ==, g_ascii_strtoull (second_uid, NULL, 10));
    g_assert_cmpstr (second->message_id, ==, second_message_id);
    g_assert_cmpstr (second->object_id, ==, second_object_id);
  }
}

static GBytes *encode_uid_map_response (const char *request_id,
    const char *query_id, const char *uid_map_csv);

static void
init_stale_result (WyreboxDaemonMailboxSelectResult *result)
{
  g_autoptr (GError) error = NULL;

  g_assert_true (wyrebox_daemon_mailbox_select_result_init (result,
          WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_VIRTUAL,
          "stale-id", "stale-name", 1, 2, 0, &error));
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
static GBytes *encode_message_fetch_response (const char *request_id,
    const char *message_id,
    const char *query_id,
    guint64 chunk_index,
    const guint8 * payload, gsize payload_size, gboolean end_of_stream);
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

static char *
assert_decoded_message_fetch_request (GBytes *request,
    const char *expected_mailbox_id,
    guint64 expected_uid_validity,
    guint64 expected_mailbox_uid, const char *expected_account_id)
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
      WYREBOX_DAEMON_REQUEST_FRAME_OPERATION_MESSAGE_FETCH);
  g_assert_nonnull (decoded.message_fetch);
  g_assert_cmpstr (decoded.caller_identity, ==, "dovecot");
  g_assert_cmpstr (decoded.account_identity, ==, "account-1");
  g_assert_cmpstr (decoded.tool_identity, ==, "dovecot-storage");
  g_assert_cmpstr (decoded.correlation_id, ==, "");
  g_assert_cmpstr (decoded.message_fetch->account_identity,
      ==, expected_account_id);
  g_assert_cmpstr (decoded.message_fetch->mailbox_id, ==, expected_mailbox_id);
  g_assert_cmpuint (decoded.message_fetch->uid_validity, ==,
      expected_uid_validity);
  g_assert_cmpuint (decoded.message_fetch->mailbox_uid, ==,
      expected_mailbox_uid);

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

static gpointer
fake_server_thread_main (gpointer user_data)
{
  FakeServer *server = user_data;
  g_autoptr (GError) error = NULL;
  g_autoptr (GSocketConnection) connection = NULL;
  g_autoptr (GBytes) request = NULL;
  g_autoptr (GBytes) response = NULL;
  g_autofree char *request_id = NULL;
  g_autofree char *query_id = NULL;
  GInputStream *input = NULL;
  GOutputStream *output = NULL;
  const char *expected_mailbox_id = server->expected_uid_map_mailbox_id != NULL
      ? server->expected_uid_map_mailbox_id : "mailbox-inbox";
  const char *fetch_message_id =
      server->message_fetch_response_message_id != NULL
      ? server->message_fetch_response_message_id : "message-1";
  const char *fetch_payload_string = "RFC5322 test body";
  gsize fetch_payload_size = strlen (fetch_payload_string);
  const guint8 *fetch_payload =
      server->message_fetch_response_payload != NULL
      ? server->message_fetch_response_payload
      : (const guint8 *) fetch_payload_string;

  if (server->message_fetch_response_payload != NULL)
    fetch_payload_size = server->message_fetch_response_payload_size;

  connection = g_socket_listener_accept (server->listener, NULL, NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (connection);

  input = g_io_stream_get_input_stream (G_IO_STREAM (connection));
  output = g_io_stream_get_output_stream (G_IO_STREAM (connection));
  g_assert_nonnull (input);
  g_assert_nonnull (output);

  request = wyrebox_daemon_frame_io_read_payload (input, &error);
  g_assert_no_error (error);
  if (server->behavior == FAKE_SERVER_UID_MAP_CSV_RESPONSE
      || server->behavior == FAKE_SERVER_UID_MAP_UNEXPECTED_SUCCESS_RESPONSE) {
    request_id =
        assert_decoded_uid_map_request (request, expected_mailbox_id,
        server->expected_uid_map_kind, &query_id);
  } else if (server->behavior == FAKE_SERVER_MESSAGE_FETCH_RESPONSE
      || (server->behavior == FAKE_SERVER_DAEMON_ERROR_RESPONSE
          && server->expect_message_fetch_request)
      || (server->behavior == FAKE_SERVER_RESPONSE_BYTES
          && server->expect_message_fetch_request)) {
    request_id =
        assert_decoded_message_fetch_request (request,
        server->expected_fetch_mailbox_id != NULL
        ? server->expected_fetch_mailbox_id : "mailbox-inbox",
        server->expected_fetch_uid_validity != 0
        ? server->expected_fetch_uid_validity : 77,
        server->expected_fetch_mailbox_uid != 0
        ? server->expected_fetch_mailbox_uid : 42,
        server->expected_fetch_account_id != NULL
        ? server->expected_fetch_account_id : "account-1");
  } else {
    request_id = assert_decoded_select_request (request);
  }

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
    case FAKE_SERVER_MESSAGE_FETCH_RESPONSE:
      response =
          encode_message_fetch_response (server->response_request_id_override !=
          NULL ? server->response_request_id_override : request_id,
          fetch_message_id, server->message_fetch_response_query_id,
          server->message_fetch_response_chunk_index, fetch_payload,
          fetch_payload_size, server->message_fetch_response_end_of_stream);
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
    case FAKE_SERVER_UID_MAP_CSV_RESPONSE:
      g_assert_nonnull (server->uid_map_csv);
      response =
          encode_uid_map_response (server->response_request_id_override != NULL
          ? server->response_request_id_override : request_id,
          server->uid_map_response_query_id_override != NULL
          ? server->uid_map_response_query_id_override
          : query_id, server->uid_map_csv);
      g_assert_nonnull (response);
      server->response = g_bytes_get_data (response, &server->response_size);
      g_assert_true (wyrebox_daemon_frame_io_write_payload (output,
              server->response, server->response_size, &error));
      g_assert_no_error (error);
      break;
    case FAKE_SERVER_UID_MAP_UNEXPECTED_SUCCESS_RESPONSE:
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
    const char *response_request_id_override,
    const char *uid_map_query_id_override,
    WyreboxDaemonMailboxListEntryKind expected_uid_map_kind)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (GSocketAddress) address = NULL;

  server->listener = g_socket_listener_new ();
  server->behavior = behavior;
  server->response_request_id_override = response_request_id_override;
  server->uid_map_response_query_id_override = uid_map_query_id_override;
  server->expected_uid_map_kind = expected_uid_map_kind;
  server->expected_fetch_account_id = "account-1";
  server->expected_fetch_mailbox_id = "mailbox-inbox";
  server->expected_fetch_uid_validity = 77;
  server->expected_fetch_mailbox_uid = 42;
  server->message_fetch_response_message_id = "message-1";
  server->message_fetch_response_query_id = NULL;
  server->message_fetch_response_payload = NULL;
  server->message_fetch_response_payload_size = 0;
  server->message_fetch_response_chunk_index = 0;
  server->message_fetch_response_end_of_stream = TRUE;
  server->expect_message_fetch_request = FALSE;
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
          "view-projects", "Projects", 77, 42, 7, &error));
  g_assert_no_error (error);
  g_assert_true (wyrebox_daemon_response_frame_init_mailbox_select (&frame,
          request_id, NULL, &select, &error));
  g_assert_no_error (error);

  return wyrebox_daemon_capnp_codec_encode_response_frame (&frame, NULL,
      &error);
}

static GBytes *
encode_message_fetch_response (const char *request_id,
    const char *message_id,
    const char *query_id,
    guint64 chunk_index,
    const guint8 *payload, gsize payload_size, gboolean end_of_stream)
{
  g_auto (WyreboxDaemonStreamChunkFrame) chunk = { 0 };
  g_auto (WyreboxDaemonResponseFrame) frame = { 0 };
  g_autoptr (GBytes) payload_bytes = NULL;
  g_autoptr (GBytes) response_payload = NULL;
  g_autoptr (GError) error = NULL;

  payload_bytes = g_bytes_new (payload, payload_size);
  g_assert_nonnull (payload_bytes);
  g_assert_true (wyrebox_daemon_stream_chunk_frame_init (&chunk,
          request_id,
          message_id,
          query_id, NULL, chunk_index, payload_bytes, end_of_stream, &error));
  g_assert_no_error (error);

  g_assert_true (wyrebox_daemon_response_frame_init_stream_chunk (&frame,
          &chunk, &error));
  g_assert_no_error (error);

  response_payload =
      wyrebox_daemon_capnp_codec_encode_response_frame (&frame, NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (response_payload);

  return g_steal_pointer (&response_payload);
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

  response_payload =
      wyrebox_daemon_capnp_codec_encode_response_frame (&frame, NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (response_payload);

  return g_steal_pointer (&response_payload);
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
      response_payload, NULL, NULL, WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_ORDINARY);

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
      NULL, NULL, NULL, WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_ORDINARY);

  g_assert_true (wyrebox_dovecot_daemon_client_select_mailbox (socket_path,
          "account-1", "Projects", &result, &error));

  g_assert_no_error (error);
  g_assert_cmpint (result.kind, ==, WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_VIRTUAL);
  g_assert_cmpstr (result.mailbox_id, ==, "view-projects");
  g_assert_cmpstr (result.mailbox_name, ==, "Projects");
  g_assert_cmpuint (result.uid_validity, ==, 77);
  g_assert_cmpuint (result.uid_next, ==, 42);
  g_assert_cmpuint (result.message_count, ==, 7);

  fake_server_join (&server);
  remove_tree (root);
}

static void
test_dovecot_daemon_client_fetch_message_bytes_succeeds (void)
{
  g_autofree char *root = NULL;
  g_autofree char *socket_path = make_socket_path (&root);
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) payload = NULL;
  FakeServer server = { 0 };
  g_autofree char *payload_text = g_strdup ("RFC5322 test body\n");
  gsize payload_size = 0;
  const guint8 *payload_data = NULL;

  fake_server_start (&server, socket_path, FAKE_SERVER_MESSAGE_FETCH_RESPONSE,
      NULL, NULL, NULL, WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_ORDINARY);
  server.expect_message_fetch_request = TRUE;
  server.message_fetch_response_payload = (const guint8 *) payload_text;
  server.message_fetch_response_payload_size = strlen (payload_text);

  payload = wyrebox_dovecot_daemon_client_fetch_message_bytes (socket_path,
      "account-1", "mailbox-inbox", 77, 42, "message-1", &error);

  g_assert_nonnull (payload);
  g_assert_no_error (error);
  payload_data = g_bytes_get_data (payload, &payload_size);
  g_assert_cmpuint (payload_size, ==, strlen (payload_text));
  g_assert_cmpmem (payload_data, payload_size, payload_text, payload_size);

  fake_server_join (&server);
  remove_tree (root);
}

static void
test_dovecot_daemon_client_fetch_message_bytes_rejects_bad_request_id (void)
{
  g_autofree char *root = NULL;
  g_autofree char *socket_path = make_socket_path (&root);
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) payload = NULL;
  FakeServer server = { 0 };

  fake_server_start (&server, socket_path, FAKE_SERVER_MESSAGE_FETCH_RESPONSE,
      NULL, "mismatched-request-id", NULL,
      WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_ORDINARY);
  server.message_fetch_response_message_id = "message-1";
  server.expect_message_fetch_request = TRUE;

  payload = wyrebox_dovecot_daemon_client_fetch_message_bytes (socket_path,
      "account-1", "mailbox-inbox", 77, 42, "message-1", &error);
  g_assert_null (payload);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);

  fake_server_join (&server);
  remove_tree (root);
}

static void
    test_dovecot_daemon_client_fetch_message_bytes_rejects_mismatched_message_id
    (void)
{
  g_autofree char *root = NULL;
  g_autofree char *socket_path = make_socket_path (&root);
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) payload = NULL;
  FakeServer server = { 0 };

  fake_server_start (&server, socket_path, FAKE_SERVER_MESSAGE_FETCH_RESPONSE,
      NULL, NULL, NULL, WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_ORDINARY);
  server.message_fetch_response_message_id = "different-message";
  server.expect_message_fetch_request = TRUE;

  payload = wyrebox_dovecot_daemon_client_fetch_message_bytes (socket_path,
      "account-1", "mailbox-inbox", 77, 42, "message-1", &error);
  g_assert_null (payload);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);

  fake_server_join (&server);
  remove_tree (root);
}

static void
test_dovecot_daemon_client_fetch_message_bytes_rejects_query_id (void)
{
  g_autofree char *root = NULL;
  g_autofree char *socket_path = make_socket_path (&root);
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) payload = NULL;
  FakeServer server = { 0 };

  fake_server_start (&server, socket_path, FAKE_SERVER_MESSAGE_FETCH_RESPONSE,
      NULL, NULL, NULL, WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_ORDINARY);
  server.message_fetch_response_message_id = "";
  server.message_fetch_response_query_id = "query-1";
  server.expect_message_fetch_request = TRUE;

  payload = wyrebox_dovecot_daemon_client_fetch_message_bytes (socket_path,
      "account-1", "mailbox-inbox", 77, 42, "message-1", &error);
  g_assert_null (payload);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);

  fake_server_join (&server);
  remove_tree (root);
}

static void
    test_dovecot_daemon_client_fetch_message_bytes_rejects_nonzero_chunk_index
    (void)
{
  g_autofree char *root = NULL;
  g_autofree char *socket_path = make_socket_path (&root);
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) payload = NULL;
  FakeServer server = { 0 };

  fake_server_start (&server, socket_path, FAKE_SERVER_MESSAGE_FETCH_RESPONSE,
      NULL, NULL, NULL, WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_ORDINARY);
  server.message_fetch_response_chunk_index = 2;
  server.expect_message_fetch_request = TRUE;

  payload = wyrebox_dovecot_daemon_client_fetch_message_bytes (socket_path,
      "account-1", "mailbox-inbox", 77, 42, "message-1", &error);
  g_assert_null (payload);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);

  fake_server_join (&server);
  remove_tree (root);
}

static void
test_dovecot_daemon_client_fetch_message_bytes_rejects_partial_chunk (void)
{
  g_autofree char *root = NULL;
  g_autofree char *socket_path = make_socket_path (&root);
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) payload = NULL;
  FakeServer server = { 0 };

  fake_server_start (&server, socket_path, FAKE_SERVER_MESSAGE_FETCH_RESPONSE,
      NULL, NULL, NULL, WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_ORDINARY);
  server.message_fetch_response_end_of_stream = FALSE;
  server.expect_message_fetch_request = TRUE;

  payload = wyrebox_dovecot_daemon_client_fetch_message_bytes (socket_path,
      "account-1", "mailbox-inbox", 77, 42, "message-1", &error);
  g_assert_null (payload);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);

  fake_server_join (&server);
  remove_tree (root);
}

static void
test_dovecot_daemon_client_fetch_message_bytes_rejects_empty_payload (void)
{
  g_autofree char *root = NULL;
  g_autofree char *socket_path = make_socket_path (&root);
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) payload = NULL;
  FakeServer server = { 0 };
  g_autofree char *empty_payload = g_strdup ("");

  fake_server_start (&server, socket_path, FAKE_SERVER_MESSAGE_FETCH_RESPONSE,
      NULL, NULL, NULL, WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_ORDINARY);
  server.message_fetch_response_payload = (const guint8 *) empty_payload;
  server.message_fetch_response_payload_size = 0;
  server.expect_message_fetch_request = TRUE;

  payload = wyrebox_dovecot_daemon_client_fetch_message_bytes (socket_path,
      "account-1", "mailbox-inbox", 77, 42, "message-1", &error);
  g_assert_null (payload);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);

  fake_server_join (&server);
  remove_tree (root);
}

static void
test_dovecot_daemon_client_fetch_message_bytes_returns_daemon_error (void)
{
  g_autofree char *root = NULL;
  g_autofree char *socket_path = make_socket_path (&root);
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) payload = NULL;
  FakeServer server = { 0 };

  fake_server_start (&server, socket_path, FAKE_SERVER_DAEMON_ERROR_RESPONSE,
      NULL, NULL, NULL, WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_ORDINARY);
  server.expect_message_fetch_request = TRUE;

  payload = wyrebox_dovecot_daemon_client_fetch_message_bytes (socket_path,
      "account-1", "mailbox-inbox", 77, 42, "message-1", &error);
  g_assert_null (payload);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_FAILED);
  g_assert_nonnull (g_strstr_len (error->message, -1, "mailbox not found"));
  fake_server_join (&server);
  remove_tree (root);
}

static void
test_dovecot_daemon_client_load_uid_map_succeeds_with_ordered_rows (void)
{
  g_autofree char *root = NULL;
  g_autofree char *socket_path = make_socket_path (&root);
  g_auto (WyreboxDovecotMailboxUidMapSnapshot) snapshot = { 0 };
  g_autoptr (GError) error = NULL;
  FakeServer server = { 0 };
  const char *uid_map_csv =
      "account_id,mailbox_id,uidvalidity,uid,message_id,object_id\n"
      "account-1,mailbox-inbox,77,42,message-1,object-1\n"
      "account-1,mailbox-inbox,77,43,message-2,object-2\n";

  fake_server_start (&server, socket_path, FAKE_SERVER_UID_MAP_CSV_RESPONSE,
      NULL, NULL, NULL, WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_ORDINARY);
  server.uid_map_csv = uid_map_csv;

  g_assert_true (wyrebox_dovecot_daemon_client_load_uid_map (socket_path,
          "account-1", "mailbox-inbox",
          WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_ORDINARY, 77, &snapshot, &error));

  g_assert_no_error (error);
  assert_uid_map_rows (&snapshot,
      "account-1",
      "mailbox-inbox",
      77, 2, "42", "message-1", "object-1", "43", "message-2", "object-2");
  fake_server_join (&server);
  remove_tree (root);
}

static void
test_dovecot_daemon_client_load_uid_map_succeeds_with_empty_map (void)
{
  g_autofree char *root = NULL;
  g_autofree char *socket_path = make_socket_path (&root);
  g_auto (WyreboxDovecotMailboxUidMapSnapshot) snapshot = { 0 };
  g_autoptr (GError) error = NULL;
  FakeServer server = { 0 };
  const char *uid_map_csv =
      "account_id,mailbox_id,uidvalidity,uid,message_id,object_id\n";

  fake_server_start (&server, socket_path, FAKE_SERVER_UID_MAP_CSV_RESPONSE,
      NULL, NULL, NULL, WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_ORDINARY);
  server.uid_map_csv = uid_map_csv;

  g_assert_true (wyrebox_dovecot_daemon_client_load_uid_map (socket_path,
          "account-1", "mailbox-inbox",
          WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_ORDINARY, 77, &snapshot, &error));

  g_assert_no_error (error);
  g_assert_nonnull (snapshot.rows);
  g_assert_cmpuint (snapshot.rows->len, ==, 0);
  fake_server_join (&server);
  remove_tree (root);
}

static void
test_dovecot_daemon_client_load_uid_map_succeeds_with_derived_view_schema (void)
{
  g_autofree char *root = NULL;
  g_autofree char *socket_path = make_socket_path (&root);
  g_auto (WyreboxDovecotMailboxUidMapSnapshot) snapshot = { 0 };
  g_autoptr (GError) error = NULL;
  FakeServer server = { 0 };
  const char *uid_map_csv =
      "account_id,view_id,uidvalidity,uid,message_id,object_id,"
      "rule_version_hash\n"
      "account-1,view-projects,77,42,message-1,object-1,hash-1\n";

  fake_server_start (&server, socket_path, FAKE_SERVER_UID_MAP_CSV_RESPONSE,
      NULL, NULL, NULL, WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_VIRTUAL);
  server.uid_map_csv = uid_map_csv;
  server.expected_uid_map_mailbox_id = "view-projects";

  g_assert_true (wyrebox_dovecot_daemon_client_load_uid_map (socket_path,
          "account-1", "view-projects",
          WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_VIRTUAL, 77, &snapshot, &error));

  g_assert_no_error (error);
  g_assert_nonnull (snapshot.rows);
  g_assert_cmpuint (snapshot.rows->len, ==, 1);
  assert_uid_map_rows (&snapshot,
      "account-1", "view-projects", 77, 1, "42", "message-1", "object-1",
      NULL, NULL, NULL);
  fake_server_join (&server);
  remove_tree (root);
}

static void
    test_dovecot_daemon_client_load_uid_map_rejects_derived_headers_in_mailbox_mode
    (void)
{
  g_autofree char *root = NULL;
  g_autofree char *socket_path = make_socket_path (&root);
  g_auto (WyreboxDovecotMailboxUidMapSnapshot) snapshot = { 0 };
  g_autoptr (GError) error = NULL;
  FakeServer server = { 0 };
  const char *uid_map_csv =
      "account_id,view_id,uidvalidity,uid,message_id,object_id,"
      "rule_version_hash\n"
      "account-1,view-projects,77,42,message-1,object-1,hash-1\n";

  fake_server_start (&server, socket_path, FAKE_SERVER_UID_MAP_CSV_RESPONSE,
      NULL, NULL, NULL, WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_ORDINARY);
  server.uid_map_csv = uid_map_csv;
  server.expected_uid_map_mailbox_id = "view-projects";

  g_assert_false (wyrebox_dovecot_daemon_client_load_uid_map (socket_path,
          "account-1", "view-projects",
          WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_ORDINARY, 77, &snapshot, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
  assert_uid_map_snapshot_is_cleared (&snapshot);
  fake_server_join (&server);
  remove_tree (root);
}

static void
test_dovecot_daemon_client_load_uid_map_rejects_mismatched_rows (void)
{
  g_autofree char *root = NULL;
  g_autofree char *socket_path = make_socket_path (&root);
  g_auto (WyreboxDovecotMailboxUidMapSnapshot) snapshot = { 0 };
  g_autoptr (GError) error = NULL;
  FakeServer server = { 0 };
  const char *uid_map_csv =
      "account_id,mailbox_id,uidvalidity,uid,message_id,object_id\n"
      "account-2,mailbox-inbox,77,42,message-1,object-1\n";

  fake_server_start (&server, socket_path, FAKE_SERVER_UID_MAP_CSV_RESPONSE,
      NULL, NULL, NULL, WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_ORDINARY);
  server.uid_map_csv = uid_map_csv;

  g_assert_false (wyrebox_dovecot_daemon_client_load_uid_map (socket_path,
          "account-1", "mailbox-inbox",
          WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_ORDINARY, 77, &snapshot, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
  assert_uid_map_snapshot_is_cleared (&snapshot);
  fake_server_join (&server);
  remove_tree (root);
}

static void
test_dovecot_daemon_client_load_uid_map_rejects_mismatched_request_id (void)
{
  g_autofree char *root = NULL;
  g_autofree char *socket_path = make_socket_path (&root);
  g_auto (WyreboxDovecotMailboxUidMapSnapshot) snapshot = { 0 };
  g_autoptr (GError) error = NULL;
  FakeServer server = { 0 };
  const char *uid_map_csv =
      "account_id,mailbox_id,uidvalidity,uid,message_id,object_id\n"
      "account-1,mailbox-inbox,77,42,message-1,object-1\n";

  fake_server_start (&server, socket_path, FAKE_SERVER_UID_MAP_CSV_RESPONSE,
      NULL, "mismatched-request-id", NULL,
      WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_ORDINARY);
  server.uid_map_csv = uid_map_csv;

  g_assert_false (wyrebox_dovecot_daemon_client_load_uid_map (socket_path,
          "account-1", "mailbox-inbox",
          WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_ORDINARY, 77, &snapshot, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
  assert_uid_map_snapshot_is_cleared (&snapshot);
  fake_server_join (&server);
  remove_tree (root);
}

static void
test_dovecot_daemon_client_load_uid_map_rejects_unexpected_response_kind (void)
{
  g_autofree char *root = NULL;
  g_autofree char *socket_path = make_socket_path (&root);
  g_auto (WyreboxDovecotMailboxUidMapSnapshot) snapshot = { 0 };
  g_autoptr (GError) error = NULL;
  FakeServer server = { 0 };

  fake_server_start (&server, socket_path,
      FAKE_SERVER_UID_MAP_UNEXPECTED_SUCCESS_RESPONSE, NULL, NULL, NULL,
      WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_ORDINARY);

  g_assert_false (wyrebox_dovecot_daemon_client_load_uid_map (socket_path,
          "account-1", "mailbox-inbox",
          WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_ORDINARY, 77, &snapshot, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
  assert_uid_map_snapshot_is_cleared (&snapshot);
  fake_server_join (&server);
  remove_tree (root);
}

static void
test_dovecot_daemon_client_load_uid_map_rejects_malformed_csv (void)
{
  g_autofree char *root = NULL;
  g_autofree char *socket_path = make_socket_path (&root);
  g_auto (WyreboxDovecotMailboxUidMapSnapshot) snapshot = { 0 };
  g_autoptr (GError) error = NULL;
  FakeServer server = { 0 };
  const char *uid_map_csv = "bad,csv";

  fake_server_start (&server, socket_path, FAKE_SERVER_UID_MAP_CSV_RESPONSE,
      NULL, NULL, NULL, WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_ORDINARY);
  server.uid_map_csv = uid_map_csv;

  g_assert_false (wyrebox_dovecot_daemon_client_load_uid_map (socket_path,
          "account-1", "mailbox-inbox",
          WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_ORDINARY, 77, &snapshot, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
  assert_uid_map_snapshot_is_cleared (&snapshot);
  fake_server_join (&server);
  remove_tree (root);
}

static void
test_dovecot_daemon_client_load_uid_map_rejects_mismatched_query_id (void)
{
  g_autofree char *root = NULL;
  g_autofree char *socket_path = make_socket_path (&root);
  g_auto (WyreboxDovecotMailboxUidMapSnapshot) snapshot = { 0 };
  g_autoptr (GError) error = NULL;
  FakeServer server = { 0 };
  const char *uid_map_csv =
      "account_id,mailbox_id,uidvalidity,uid,message_id,object_id\n"
      "account-1,mailbox-inbox,77,42,message-1,object-1\n";

  fake_server_start (&server, socket_path, FAKE_SERVER_UID_MAP_CSV_RESPONSE,
      NULL, NULL, "mismatched-query-id",
      WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_ORDINARY);
  server.uid_map_csv = uid_map_csv;

  g_assert_false (wyrebox_dovecot_daemon_client_load_uid_map (socket_path,
          "account-1", "mailbox-inbox",
          WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_ORDINARY, 77, &snapshot, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
  assert_uid_map_snapshot_is_cleared (&snapshot);
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
      NULL, NULL, NULL, WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_ORDINARY);

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
      NULL, "mismatched-request-id", NULL,
      WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_ORDINARY);

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
      FAKE_SERVER_UNEXPECTED_SUCCESS_RESPONSE, NULL, NULL, NULL,
      WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_ORDINARY);

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
      NULL, NULL, NULL, WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_ORDINARY);

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
      NULL, NULL, NULL, WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_ORDINARY);

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

  g_clear_error (&error);
  g_assert_false (wyrebox_dovecot_daemon_client_fetch_message_bytes
      ("/tmp/wyrebox.sock", "account-1", "mailbox-inbox", 77, 42,
          "message-1", &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED);
}

#endif

static void
test_dovecot_daemon_client_invalid_inputs_fail_cleanly (void)
{
  g_auto (WyreboxDaemonMailboxSelectResult) result = { 0 };
  g_auto (WyreboxDovecotMailboxUidMapSnapshot) uid_map_snapshot = { 0 };
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

  g_clear_error (&error);
  g_assert_false (wyrebox_dovecot_daemon_client_load_uid_map
      ("/tmp/wyrebox.sock", "account-1", "mailbox-inbox",
          WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_ORDINARY, 77, NULL, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  assert_uid_map_snapshot_is_cleared (&uid_map_snapshot);

  g_clear_error (&error);
  g_assert_false (wyrebox_dovecot_daemon_client_load_uid_map (NULL,
          "account-1", "mailbox-inbox",
          WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_ORDINARY, 77, &uid_map_snapshot,
          &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  assert_uid_map_snapshot_is_cleared (&uid_map_snapshot);

  g_clear_error (&error);
  g_assert_false (wyrebox_dovecot_daemon_client_load_uid_map
      ("/tmp/wyrebox.sock", "", "mailbox-inbox",
          WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_ORDINARY, 77, &uid_map_snapshot,
          &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  assert_uid_map_snapshot_is_cleared (&uid_map_snapshot);

  g_clear_error (&error);
  g_assert_false (wyrebox_dovecot_daemon_client_load_uid_map
      ("/tmp/wyrebox.sock", "account-1", "",
          WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_ORDINARY, 77, &uid_map_snapshot,
          &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  assert_uid_map_snapshot_is_cleared (&uid_map_snapshot);

  g_clear_error (&error);
  g_assert_false (wyrebox_dovecot_daemon_client_load_uid_map
      ("/tmp/wyrebox.sock", "account-1", "mailbox-inbox",
          (WyreboxDaemonMailboxListEntryKind) 9999,
          77, &uid_map_snapshot, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  assert_uid_map_snapshot_is_cleared (&uid_map_snapshot);

  g_clear_error (&error);
  g_assert_null (wyrebox_dovecot_daemon_client_fetch_message_bytes
      ("/tmp/wyrebox.sock", "", "mailbox-inbox", 77, 42, "message-1", &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);

  g_clear_error (&error);
  g_assert_null (wyrebox_dovecot_daemon_client_fetch_message_bytes
      ("/tmp/wyrebox.sock", "account-1", "", 77, 42, "message-1", &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);

  g_clear_error (&error);
  g_assert_null (wyrebox_dovecot_daemon_client_fetch_message_bytes
      ("/tmp/wyrebox.sock", "account-1", "mailbox-inbox", 0, 42, "message-1",
          &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);

  g_clear_error (&error);
  g_assert_null (wyrebox_dovecot_daemon_client_fetch_message_bytes
      ("/tmp/wyrebox.sock", "account-1", "mailbox-inbox", 77, 0, "message-1",
          &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);

  g_clear_error (&error);
  g_assert_null (wyrebox_dovecot_daemon_client_fetch_message_bytes
      ("/tmp/wyrebox.sock", "account-1", "mailbox-inbox", 77, 42, "", &error));
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
  g_test_add_func ("/dovecot/daemon-client/fetch-message-bytes-succeeds",
      test_dovecot_daemon_client_fetch_message_bytes_succeeds);
  g_test_add_func
      ("/dovecot/daemon-client/fetch-message-bytes-rejects-bad-request-id",
      test_dovecot_daemon_client_fetch_message_bytes_rejects_bad_request_id);
  g_test_add_func
      ("/dovecot/daemon-client/fetch-message-bytes-rejects-mismatched-message-id",
      test_dovecot_daemon_client_fetch_message_bytes_rejects_mismatched_message_id);
  g_test_add_func
      ("/dovecot/daemon-client/fetch-message-bytes-rejects-query-id",
      test_dovecot_daemon_client_fetch_message_bytes_rejects_query_id);
  g_test_add_func
      ("/dovecot/daemon-client/fetch-message-bytes-rejects-nonzero-chunk-index",
      test_dovecot_daemon_client_fetch_message_bytes_rejects_nonzero_chunk_index);
  g_test_add_func
      ("/dovecot/daemon-client/fetch-message-bytes-rejects-partial-chunk",
      test_dovecot_daemon_client_fetch_message_bytes_rejects_partial_chunk);
  g_test_add_func
      ("/dovecot/daemon-client/fetch-message-bytes-rejects-empty-payload",
      test_dovecot_daemon_client_fetch_message_bytes_rejects_empty_payload);
  g_test_add_func
      ("/dovecot/daemon-client/fetch-message-bytes-returns-daemon-error",
      test_dovecot_daemon_client_fetch_message_bytes_returns_daemon_error);
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
  g_test_add_func
      ("/dovecot/daemon-client/load-uid-map-succeeds-with-ordered-rows",
      test_dovecot_daemon_client_load_uid_map_succeeds_with_ordered_rows);
  g_test_add_func
      ("/dovecot/daemon-client/load-uid-map-succeeds-with-empty-map",
      test_dovecot_daemon_client_load_uid_map_succeeds_with_empty_map);
  g_test_add_func
      ("/dovecot/daemon-client/load-uid-map-succeeds-with-derived-view-schema",
      test_dovecot_daemon_client_load_uid_map_succeeds_with_derived_view_schema);
  g_test_add_func ("/dovecot/daemon-client/"
      "load-uid-map-rejects-derived-headers-in-mailbox-mode",
      test_dovecot_daemon_client_load_uid_map_rejects_derived_headers_in_mailbox_mode);
  g_test_add_func
      ("/dovecot/daemon-client/load-uid-map-rejects-mismatched-rows",
      test_dovecot_daemon_client_load_uid_map_rejects_mismatched_rows);
  g_test_add_func
      ("/dovecot/daemon-client/load-uid-map-rejects-mismatched-request-id",
      test_dovecot_daemon_client_load_uid_map_rejects_mismatched_request_id);
  g_test_add_func
      ("/dovecot/daemon-client/load-uid-map-rejects-mismatched-query-id",
      test_dovecot_daemon_client_load_uid_map_rejects_mismatched_query_id);
  g_test_add_func
      ("/dovecot/daemon-client/load-uid-map-rejects-unexpected-response-kind",
      test_dovecot_daemon_client_load_uid_map_rejects_unexpected_response_kind);
  g_test_add_func ("/dovecot/daemon-client/load-uid-map-rejects-malformed-csv",
      test_dovecot_daemon_client_load_uid_map_rejects_malformed_csv);
#else
  g_test_add_func ("/dovecot/daemon-client/capnp-disabled-not-supported",
      test_dovecot_daemon_client_capnp_disabled_is_not_supported);
#endif
  g_test_add_func ("/dovecot/daemon-client/invalid-inputs",
      test_dovecot_daemon_client_invalid_inputs_fail_cleanly);

  return g_test_run ();
}
