#include "wyrebox-daemon-fact-mutation-request.h"
#include "wyrebox-daemon-request-adapter.h"
#include "wyrebox-daemon-request-router.h"
#include "wyrebox-journal-reader.h"
#include "wyrebox-journal-writer.h"

#include <gio/gio.h>
#include <glib.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

typedef enum
{
  TEST_REQUEST_ADAPTER_SCENARIO_MAILBOX_LIST_SUCCESS,
  TEST_REQUEST_ADAPTER_SCENARIO_FACT_MUTATION_SUCCESS,
  TEST_REQUEST_ADAPTER_SCENARIO_FACT_MUTATION_UNAUTHORIZED,
  TEST_REQUEST_ADAPTER_SCENARIO_FACT_MUTATION_MISSING_PAYLOAD,
  TEST_REQUEST_ADAPTER_SCENARIO_DECODE_FAIL,
} TestRequestAdapterScenario;

typedef struct
{
  TestRequestAdapterScenario scenario;
  const char *request_payload;
  const char *request_id;
  const WyreboxDaemonPeerCredentials expected_peer_credentials;
  gboolean expected_peer_credentials_match;
  gboolean peer_credentials_seen;
  gboolean encode_should_fail;
  guint decode_calls;
  guint encode_calls;
  guint clear_calls;
} TestRequestAdapterCodecState;

typedef struct
{
  TestRequestAdapterCodecState *state;
  TestRequestAdapterScenario scenario;
  WyreboxDaemonMailboxListRequest mailbox_list_request;
  WyreboxDaemonFactMutationRequest fact_mutation_request;
} TestRequestAdapterDecodedState;

static void
test_remove_tree (const char *path)
{
  g_autoptr (GDir) dir = NULL;
  const char *entry = NULL;

  dir = g_dir_open (path, 0, NULL);
  if (dir == NULL) {
    (void) remove (path);
    return;
  }

  while ((entry = g_dir_read_name (dir)) != NULL) {
    g_autofree char *child = g_build_filename (path, entry, NULL);

    test_remove_tree (child);
  }

  (void) rmdir (path);
}

static void
test_assert_journal_is_empty (const char *root)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxJournalReader) reader = NULL;
  g_auto (WyreboxJournalRecord) record = { 0 };
  gboolean eof = FALSE;

  reader = wyrebox_journal_reader_new (root, &error);
  g_assert_no_error (error);
  g_assert_false (wyrebox_journal_reader_read_next (reader,
          &record, &eof, &error));
  g_assert_no_error (error);
  g_assert_true (eof);
}

static void
test_assert_journal_is_not_empty (const char *root)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxJournalReader) reader = NULL;
  g_auto (WyreboxJournalRecord) record = { 0 };
  gboolean eof = FALSE;

  reader = wyrebox_journal_reader_new (root, &error);
  g_assert_no_error (error);
  g_assert_true (wyrebox_journal_reader_read_next (reader,
          &record, &eof, &error));
  g_assert_no_error (error);
  g_assert_false (eof);
}

static void
assert_bytes_equal (GBytes *actual, const char *expected)
{
  gsize actual_size = 0;
  const guint8 *actual_data = NULL;

  g_assert_nonnull (actual);
  g_assert_nonnull (expected);

  actual_data = g_bytes_get_data (actual, &actual_size);
  g_assert_cmpuint (actual_size, ==, strlen (expected));
  g_assert_cmpint (memcmp (actual_data, expected, actual_size), ==, 0);
}

static gboolean
request_payload_matches (GBytes *request, const char *expected_payload)
{
  const guint8 *request_data = NULL;
  gsize request_size = 0;
  gsize expected_size = 0;

  request_data = g_bytes_get_data (request, &request_size);
  expected_size = strlen (expected_payload);
  return request_size == expected_size
      && memcmp (request_data, expected_payload, expected_size) == 0;
}

static void
test_request_adapter_clear_decoded_state (gpointer user_data)
{
  TestRequestAdapterDecodedState *state = user_data;
  g_assert_nonnull (state);
  g_assert_nonnull (state->state);

  state->state->clear_calls++;

  switch (state->scenario) {
    case TEST_REQUEST_ADAPTER_SCENARIO_MAILBOX_LIST_SUCCESS:
      wyrebox_daemon_mailbox_list_request_clear (&state->mailbox_list_request);
      break;
    case TEST_REQUEST_ADAPTER_SCENARIO_FACT_MUTATION_SUCCESS:
    case TEST_REQUEST_ADAPTER_SCENARIO_FACT_MUTATION_UNAUTHORIZED:
    case TEST_REQUEST_ADAPTER_SCENARIO_FACT_MUTATION_MISSING_PAYLOAD:
      wyrebox_daemon_fact_mutation_request_clear
          (&state->fact_mutation_request);
      break;
    case TEST_REQUEST_ADAPTER_SCENARIO_DECODE_FAIL:
      break;
  }

  g_free (state);
}

static gboolean
test_request_adapter_decode (const WyreboxDaemonPeerCredentials
    *peer_credentials, GBytes *request,
    WyreboxDaemonDecodedRequestFrame *out_request, gpointer *out_decoded_state,
    GDestroyNotify *out_decoded_state_clear, gpointer user_data, GError **error)
{
  TestRequestAdapterCodecState *state = user_data;
  TestRequestAdapterDecodedState *decoded_state = NULL;
  const char *args[] = { "message-1", NULL };
  g_return_val_if_fail (state != NULL, FALSE);

  state->decode_calls++;

  if (!request_payload_matches (request, state->request_payload))
    g_set_error (error,
        G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "request payload is unexpected");

  if (state->expected_peer_credentials_match) {
    state->peer_credentials_seen = peer_credentials != NULL
        && peer_credentials->uid == state->expected_peer_credentials.uid
        && peer_credentials->gid == state->expected_peer_credentials.gid
        && peer_credentials->pid == state->expected_peer_credentials.pid;
  }

  if (error != NULL && *error != NULL)
    return FALSE;

  decoded_state = g_new0 (TestRequestAdapterDecodedState, 1);
  decoded_state->state = state;
  decoded_state->scenario = state->scenario;
  *out_decoded_state = decoded_state;
  *out_decoded_state_clear = test_request_adapter_clear_decoded_state;

  switch (state->scenario) {
    case TEST_REQUEST_ADAPTER_SCENARIO_MAILBOX_LIST_SUCCESS:
      out_request->request_id = "request-mailbox-list";
      out_request->caller_identity = "dovecot";
      out_request->account_identity = "account-1";
      out_request->tool_identity = "dovecot-storage";
      out_request->correlation_id = "imap-list-1";
      if (!wyrebox_daemon_mailbox_list_request_init
          (&decoded_state->mailbox_list_request, "account-1", NULL, error))
        return FALSE;
      out_request->operation =
          WYREBOX_DAEMON_REQUEST_FRAME_OPERATION_MAILBOX_LIST;
      out_request->mailbox_list = &decoded_state->mailbox_list_request;
      return TRUE;

    case TEST_REQUEST_ADAPTER_SCENARIO_FACT_MUTATION_SUCCESS:
      out_request->request_id = "request-fact-success";
      out_request->caller_identity = "skill";
      out_request->account_identity = "account-1";
      out_request->tool_identity = "fact-importer";
      out_request->correlation_id = "correlation-1";
      if (!wyrebox_daemon_fact_mutation_request_init
          (&decoded_state->fact_mutation_request,
              WYREBOX_DAEMON_FACT_MUTATION_INSERT,
              "project_mention", "account-1", args, error))
        return FALSE;
      out_request->operation =
          WYREBOX_DAEMON_REQUEST_FRAME_OPERATION_FACT_MUTATION;
      out_request->fact_mutation = &decoded_state->fact_mutation_request;
      return TRUE;

    case TEST_REQUEST_ADAPTER_SCENARIO_FACT_MUTATION_UNAUTHORIZED:
      out_request->request_id = "request-fact-unauthorized";
      out_request->caller_identity = "dovecot";
      out_request->account_identity = "account-1";
      out_request->tool_identity = "fact-importer";
      out_request->correlation_id = "correlation-1";
      if (!wyrebox_daemon_fact_mutation_request_init
          (&decoded_state->fact_mutation_request,
              WYREBOX_DAEMON_FACT_MUTATION_INSERT,
              "project_mention", "account-1", args, error))
        return FALSE;
      out_request->operation =
          WYREBOX_DAEMON_REQUEST_FRAME_OPERATION_FACT_MUTATION;
      out_request->fact_mutation = &decoded_state->fact_mutation_request;
      return TRUE;

    case TEST_REQUEST_ADAPTER_SCENARIO_FACT_MUTATION_MISSING_PAYLOAD:
      out_request->request_id = "request-fact-missing-payload";
      out_request->caller_identity = "skill";
      out_request->account_identity = "account-1";
      out_request->tool_identity = "fact-importer";
      out_request->correlation_id = "correlation-1";
      out_request->operation =
          WYREBOX_DAEMON_REQUEST_FRAME_OPERATION_FACT_MUTATION;
      out_request->fact_mutation = NULL;
      return TRUE;

    case TEST_REQUEST_ADAPTER_SCENARIO_DECODE_FAIL:
      g_set_error (error,
          G_IO_ERROR,
          G_IO_ERROR_INVALID_DATA, "decode callback failed intentionally");
      return FALSE;

    default:
      g_set_error (error,
          G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "unknown adapter test scenario");
      return FALSE;
  }
}

static GBytes *
test_request_adapter_encode (const WyreboxDaemonResponseFrame *response_frame,
    gpointer user_data, GError **error)
{
  TestRequestAdapterCodecState *state = user_data;

  g_assert_nonnull (state);
  state->encode_calls++;

  if (state->encode_should_fail) {
    g_set_error (error,
        G_IO_ERROR, G_IO_ERROR_FAILED, "response encoder failed intentionally");
    return NULL;
  }

  g_assert_cmpstr (response_frame->request_id, ==, state->request_id);

  switch (response_frame->kind) {
    case WYREBOX_DAEMON_RESPONSE_FRAME_MAILBOX_LIST:
      return g_bytes_new_static ("encoded-mailbox-list",
          strlen ("encoded-mailbox-list"));
    case WYREBOX_DAEMON_RESPONSE_FRAME_SUCCESS:
      return g_bytes_new_static ("encoded-success", strlen ("encoded-success"));
    case WYREBOX_DAEMON_RESPONSE_FRAME_ERROR:
      return g_bytes_new_static ("encoded-error", strlen ("encoded-error"));
    case WYREBOX_DAEMON_RESPONSE_FRAME_NONE:
    default:
      g_set_error (error,
          G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "response frame has no payload");
      return NULL;
  }
}

static gboolean
append_fixture_mailbox_list (const WyreboxDaemonRequestIdentity *identity,
    const WyreboxDaemonMailboxListRequest *request,
    WyreboxDaemonMailboxListResult *out_result,
    gpointer user_data, GError **error)
{
  gboolean *was_called = user_data;

  g_assert_cmpstr (identity->request_id, ==, "request-mailbox-list");
  g_assert_cmpstr (identity->caller_identity, ==, "dovecot");
  g_assert_cmpstr (request->account_identity, ==, "account-1");

  *was_called = TRUE;

  wyrebox_daemon_mailbox_list_result_init_empty (out_result);
  return wyrebox_daemon_mailbox_list_result_append_entry (out_result,
      WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_ORDINARY, "inbox", "INBOX", "/", NULL,
      TRUE, WYREBOX_DAEMON_MAILBOX_LIST_CHILD_STATE_UNKNOWN, error);
}

static void
test_request_adapter_routes_mailbox_list (void)
{
  TestRequestAdapterCodecState codec_state = {
    .scenario = TEST_REQUEST_ADAPTER_SCENARIO_MAILBOX_LIST_SUCCESS,
    .request_payload = "request-mailbox-list",
    .request_id = "request-mailbox-list",
    .expected_peer_credentials = {
          .uid = 2000,
          .gid = 3000,
          .pid = 4000,
        },
    .expected_peer_credentials_match = TRUE,
  };
  const WyreboxDaemonPeerCredentials credentials = {
    .uid = 2000,
    .gid = 3000,
    .pid = 4000,
  };
  gboolean mailbox_list_was_called = FALSE;
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxDaemonRequestAdapter) adapter = NULL;
  g_autoptr (WyreboxDaemonMailboxListService) mailbox_list_service = NULL;
  g_autoptr (GBytes) request = NULL;
  g_autoptr (GBytes) response = NULL;

  mailbox_list_service =
      wyrebox_daemon_mailbox_list_service_new (append_fixture_mailbox_list,
      &mailbox_list_was_called, NULL);
  g_assert_nonnull (mailbox_list_service);

  adapter =
      wyrebox_daemon_request_adapter_new (NULL, mailbox_list_service, NULL,
      NULL,
      test_request_adapter_decode, &codec_state, NULL,
      test_request_adapter_encode, &codec_state, NULL);
  g_assert_nonnull (adapter);

  request = g_bytes_new_static ("request-mailbox-list",
      strlen ("request-mailbox-list"));
  response = wyrebox_daemon_request_adapter_handle_payload (&credentials,
      request, adapter, &error);
  g_assert_no_error (error);
  g_assert_nonnull (response);
  assert_bytes_equal (response, "encoded-mailbox-list");
  g_assert_true (mailbox_list_was_called);
  g_assert_true (codec_state.peer_credentials_seen);
  g_assert_cmpuint (codec_state.decode_calls, ==, 1);
  g_assert_cmpuint (codec_state.encode_calls, ==, 1);
  g_assert_cmpuint (codec_state.clear_calls, ==, 1);
}

static void
test_request_adapter_routes_fact_mutation (void)
{
  TestRequestAdapterCodecState codec_state = {
    .scenario = TEST_REQUEST_ADAPTER_SCENARIO_FACT_MUTATION_SUCCESS,
    .request_payload = "request-fact-success",
    .request_id = "request-fact-success",
    .expected_peer_credentials = {
          .uid = 5000,
          .gid = 6000,
          .pid = 7000,
        },
    .expected_peer_credentials_match = TRUE,
  };
  const WyreboxDaemonPeerCredentials credentials = {
    .uid = 5000,
    .gid = 6000,
    .pid = 7000,
  };
  g_autofree char *root = g_dir_make_tmp ("wyrebox-request-adapter-fact-XXXXXX",
      NULL);
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxJournalWriter) writer = NULL;
  g_autoptr (WyreboxDaemonFactMutationService) fact_mutation_service = NULL;
  g_autoptr (WyreboxDaemonRequestAdapter) adapter = NULL;
  g_autoptr (GBytes) request = NULL;
  g_autoptr (GBytes) response = NULL;

  g_assert_nonnull (root);

  writer = wyrebox_journal_writer_new (root, &error);
  g_assert_no_error (error);

  fact_mutation_service = wyrebox_daemon_fact_mutation_service_new (writer);
  g_assert_nonnull (fact_mutation_service);

  adapter =
      wyrebox_daemon_request_adapter_new (fact_mutation_service, NULL, NULL,
      NULL,
      test_request_adapter_decode, &codec_state, NULL,
      test_request_adapter_encode, &codec_state, NULL);
  g_assert_nonnull (adapter);

  request = g_bytes_new_static ("request-fact-success",
      strlen ("request-fact-success"));
  response = wyrebox_daemon_request_adapter_handle_payload (&credentials,
      request, adapter, &error);
  g_assert_no_error (error);
  g_assert_nonnull (response);
  assert_bytes_equal (response, "encoded-success");
  g_assert_cmpuint (codec_state.decode_calls, ==, 1);
  g_assert_cmpuint (codec_state.encode_calls, ==, 1);
  g_assert_cmpuint (codec_state.clear_calls, ==, 1);
  test_assert_journal_is_not_empty (root);

  test_remove_tree (root);
}

static void
test_request_adapter_rejects_unauthorized_fact_mutation (void)
{
  TestRequestAdapterCodecState codec_state = {
    .scenario = TEST_REQUEST_ADAPTER_SCENARIO_FACT_MUTATION_UNAUTHORIZED,
    .request_payload = "request-fact-unauthorized",
    .request_id = "request-fact-unauthorized",
    .expected_peer_credentials = {
          .uid = 123,
          .gid = 124,
          .pid = 125,
        },
    .expected_peer_credentials_match = TRUE,
  };
  const WyreboxDaemonPeerCredentials credentials = {
    .uid = 123,
    .gid = 124,
    .pid = 125,
  };
  g_autofree char *root = g_dir_make_tmp ("wyrebox-request-adapter-fact-XXXXXX",
      NULL);
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxJournalWriter) writer = NULL;
  g_autoptr (WyreboxDaemonFactMutationService) fact_mutation_service = NULL;
  g_autoptr (WyreboxDaemonRequestAdapter) adapter = NULL;
  g_autoptr (GBytes) request = NULL;
  g_autoptr (GBytes) response = NULL;

  g_assert_nonnull (root);

  writer = wyrebox_journal_writer_new (root, &error);
  g_assert_no_error (error);

  fact_mutation_service = wyrebox_daemon_fact_mutation_service_new (writer);
  g_assert_nonnull (fact_mutation_service);

  adapter =
      wyrebox_daemon_request_adapter_new (fact_mutation_service, NULL, NULL,
      NULL,
      test_request_adapter_decode, &codec_state, NULL,
      test_request_adapter_encode, &codec_state, NULL);
  g_assert_nonnull (adapter);

  request = g_bytes_new_static ("request-fact-unauthorized",
      strlen ("request-fact-unauthorized"));
  response = wyrebox_daemon_request_adapter_handle_payload (&credentials,
      request, adapter, &error);
  g_assert_no_error (error);
  g_assert_nonnull (response);
  assert_bytes_equal (response, "encoded-error");
  g_assert_cmpuint (codec_state.decode_calls, ==, 1);
  g_assert_cmpuint (codec_state.encode_calls, ==, 1);
  g_assert_cmpuint (codec_state.clear_calls, ==, 1);
  test_assert_journal_is_empty (root);

  test_remove_tree (root);
}

static void
test_request_adapter_rejects_missing_fact_mutation_payload (void)
{
  TestRequestAdapterCodecState codec_state = {
    .scenario = TEST_REQUEST_ADAPTER_SCENARIO_FACT_MUTATION_MISSING_PAYLOAD,
    .request_payload = "request-fact-missing-payload",
    .request_id = "request-fact-missing-payload",
    .expected_peer_credentials = {
          .uid = 300,
          .gid = 301,
          .pid = 302,
        },
    .expected_peer_credentials_match = TRUE,
  };
  const WyreboxDaemonPeerCredentials credentials = {
    .uid = 300,
    .gid = 301,
    .pid = 302,
  };
  g_autofree char *root = g_dir_make_tmp ("wyrebox-request-adapter-fact-XXXXXX",
      NULL);
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxJournalWriter) writer = NULL;
  g_autoptr (WyreboxDaemonFactMutationService) fact_mutation_service = NULL;
  g_autoptr (WyreboxDaemonRequestAdapter) adapter = NULL;
  g_autoptr (GBytes) request = NULL;
  g_autoptr (GBytes) response = NULL;

  g_assert_nonnull (root);

  writer = wyrebox_journal_writer_new (root, &error);
  g_assert_no_error (error);

  fact_mutation_service = wyrebox_daemon_fact_mutation_service_new (writer);
  g_assert_nonnull (fact_mutation_service);

  adapter =
      wyrebox_daemon_request_adapter_new (fact_mutation_service, NULL, NULL,
      NULL,
      test_request_adapter_decode, &codec_state, NULL,
      test_request_adapter_encode, &codec_state, NULL);
  g_assert_nonnull (adapter);

  request = g_bytes_new_static ("request-fact-missing-payload",
      strlen ("request-fact-missing-payload"));
  response = wyrebox_daemon_request_adapter_handle_payload (&credentials,
      request, adapter, &error);
  g_assert_no_error (error);
  g_assert_nonnull (response);
  assert_bytes_equal (response, "encoded-error");
  g_assert_cmpuint (codec_state.decode_calls, ==, 1);
  g_assert_cmpuint (codec_state.encode_calls, ==, 1);
  g_assert_cmpuint (codec_state.clear_calls, ==, 1);
  test_assert_journal_is_empty (root);

  test_remove_tree (root);
}

static void
test_request_adapter_decode_failure_skips_router_and_encoder (void)
{
  TestRequestAdapterCodecState codec_state = {
    .scenario = TEST_REQUEST_ADAPTER_SCENARIO_DECODE_FAIL,
    .request_payload = "request-decode-fail",
    .request_id = "request-decode-fail",
    .expected_peer_credentials = {
          .uid = 11,
          .gid = 12,
          .pid = 13,
        },
    .expected_peer_credentials_match = TRUE,
  };
  const WyreboxDaemonPeerCredentials credentials = {
    .uid = 11,
    .gid = 12,
    .pid = 13,
  };
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxDaemonRequestAdapter) adapter = NULL;
  g_autoptr (GBytes) request = NULL;
  g_autoptr (GBytes) response = NULL;

  adapter = wyrebox_daemon_request_adapter_new (NULL, NULL, NULL,
      NULL,
      test_request_adapter_decode, &codec_state, NULL,
      test_request_adapter_encode, &codec_state, NULL);
  g_assert_nonnull (adapter);

  request = g_bytes_new_static ("request-decode-fail",
      strlen ("request-decode-fail"));
  response = wyrebox_daemon_request_adapter_handle_payload (&credentials,
      request, adapter, &error);
  g_assert_null (response);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
  g_assert_cmpuint (codec_state.decode_calls, ==, 1);
  g_assert_cmpuint (codec_state.encode_calls, ==, 0);
  g_assert_cmpuint (codec_state.clear_calls, ==, 1);
}

static void
test_request_adapter_encode_failure_is_propagated (void)
{
  TestRequestAdapterCodecState codec_state = {
    .scenario = TEST_REQUEST_ADAPTER_SCENARIO_MAILBOX_LIST_SUCCESS,
    .request_payload = "request-mailbox-list",
    .request_id = "request-mailbox-list",
    .expected_peer_credentials = {
          .uid = 77,
          .gid = 78,
          .pid = 79,
        },
    .expected_peer_credentials_match = TRUE,
    .encode_should_fail = TRUE,
  };
  const WyreboxDaemonPeerCredentials credentials = {
    .uid = 77,
    .gid = 78,
    .pid = 79,
  };
  gboolean mailbox_list_was_called = FALSE;
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxDaemonRequestAdapter) adapter = NULL;
  g_autoptr (WyreboxDaemonMailboxListService) mailbox_list_service = NULL;
  g_autoptr (GBytes) request = NULL;
  g_autoptr (GBytes) response = NULL;

  mailbox_list_service =
      wyrebox_daemon_mailbox_list_service_new (append_fixture_mailbox_list,
      &mailbox_list_was_called, NULL);
  g_assert_nonnull (mailbox_list_service);

  adapter =
      wyrebox_daemon_request_adapter_new (NULL, mailbox_list_service, NULL,
      NULL,
      test_request_adapter_decode, &codec_state, NULL,
      test_request_adapter_encode, &codec_state, NULL);
  g_assert_nonnull (adapter);

  request = g_bytes_new_static ("request-mailbox-list",
      strlen ("request-mailbox-list"));
  response = wyrebox_daemon_request_adapter_handle_payload (&credentials,
      request, adapter, &error);
  g_assert_null (response);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_FAILED);
  g_assert_true (mailbox_list_was_called);
  g_assert_cmpuint (codec_state.decode_calls, ==, 1);
  g_assert_cmpuint (codec_state.encode_calls, ==, 1);
  g_assert_cmpuint (codec_state.clear_calls, ==, 1);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/daemon-api/request-adapter/routes-mailbox-list",
      test_request_adapter_routes_mailbox_list);
  g_test_add_func ("/daemon-api/request-adapter/routes-fact-mutation",
      test_request_adapter_routes_fact_mutation);
  g_test_add_func ("/daemon-api/request-adapter/"
      "rejects-unauthorized-fact-mutation-with-error-frame",
      test_request_adapter_rejects_unauthorized_fact_mutation);
  g_test_add_func ("/daemon-api/request-adapter/"
      "rejects-missing-fact-mutation-payload-with-error-frame",
      test_request_adapter_rejects_missing_fact_mutation_payload);
  g_test_add_func ("/daemon-api/request-adapter/"
      "decode-failure-skips-router-and-encoder",
      test_request_adapter_decode_failure_skips_router_and_encoder);
  g_test_add_func ("/daemon-api/request-adapter/"
      "encode-failure-is-propagated",
      test_request_adapter_encode_failure_is_propagated);

  return g_test_run ();
}
