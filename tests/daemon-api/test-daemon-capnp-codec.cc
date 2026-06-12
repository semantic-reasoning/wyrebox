#include "wyrebox-daemon-capnp-codec.h"
#include "wyrebox-daemon-mailbox-list-request.h"
#include "wyrebox-daemon-mailbox-list-result.h"
#include "wyrebox-daemon-mailbox-list-service.h"
#include "wyrebox-daemon-request-adapter.h"

#include <capnp/message.h>
#include <capnp/serialize.h>

#include <glib.h>

#include "wyrebox-daemon-api.capnp.h"

typedef struct
{
  gboolean was_called;
} FixtureState;

static GBytes *
build_mailbox_list_request_bytes (const char *namespace_prefix,
    const char *request_id)
{
  capnp::MallocMessageBuilder request_builder;
  auto request_frame = request_builder.initRoot<RequestFrame> ();

  auto identity = request_frame.initIdentity ();
  identity.setRequestId (request_id);
  identity.setCallerIdentity ("dovecot");
  identity.setAccountIdentity ("account-1");
  identity.setToolIdentity ("dovecot-storage");
  identity.setCorrelationId ("corr-1");

  auto mailbox_list_request = request_frame.initMailboxList ();
  mailbox_list_request.setAccountIdentity ("account-1");
  mailbox_list_request.setNamespacePrefix (namespace_prefix);

  auto words = capnp::messageToFlatArray (request_builder);
  auto bytes = words.asBytes ();

  return g_bytes_new (bytes.begin (), bytes.size ());
}

static GBytes *
build_delivery_ingestion_request_bytes (void)
{
  capnp::MallocMessageBuilder request_builder;
  auto request_frame = request_builder.initRoot<RequestFrame> ();

  auto identity = request_frame.initIdentity ();
  identity.setRequestId ("request-delivery");
  identity.setCallerIdentity ("caller");
  identity.setAccountIdentity ("account-1");
  identity.setToolIdentity ("tool");
  identity.setCorrelationId ("corr-2");

  request_frame.initDeliveryIngestion ().setQueueId ("QID");

  auto words = capnp::messageToFlatArray (request_builder);
  auto bytes = words.asBytes ();

  return g_bytes_new (bytes.begin (), bytes.size ());
}

static gboolean
append_mailbox_fixture (const WyreboxDaemonRequestIdentity *identity,
    const WyreboxDaemonMailboxListRequest *request,
    WyreboxDaemonMailboxListResult *out_result,
    gpointer user_data,
    GError **error)
{
  FixtureState *state = static_cast<FixtureState *> (user_data);

  g_assert_nonnull (identity);
  g_assert_nonnull (request);

  g_assert_cmpstr (identity->request_id, ==, "request-1");
  g_assert_cmpstr (request->account_identity, ==, "account-1");

  state->was_called = TRUE;

  wyrebox_daemon_mailbox_list_result_init_empty (out_result);
  g_assert_true (wyrebox_daemon_mailbox_list_result_append_entry (
      out_result,
      WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_ORDINARY,
      "mailbox-inbox",
      "INBOX",
      "/",
      NULL,
      TRUE,
      WYREBOX_DAEMON_MAILBOX_LIST_CHILD_STATE_UNKNOWN,
      error));

  return TRUE;
}

static void
assert_request_bytes_decode_mailbox_list (void)
{
  g_autoptr (GBytes) request = build_mailbox_list_request_bytes ("INBOX", "request-1");
  g_autoptr (GError) error = NULL;
  WyreboxDaemonDecodedRequestFrame decoded = { 0 };
  gpointer decoded_state = NULL;
  GDestroyNotify decoded_state_clear = NULL;

  g_assert_true (wyrebox_daemon_capnp_codec_decode_request_frame (NULL,
      request,
      &decoded,
      &decoded_state,
      &decoded_state_clear,
      NULL,
      &error));
  g_assert_no_error (error);
  g_assert_cmpstr (decoded.request_id, ==, "request-1");
  g_assert_cmpstr (decoded.caller_identity, ==, "dovecot");
  g_assert_cmpstr (decoded.account_identity, ==, "account-1");
  g_assert_cmpstr (decoded.tool_identity, ==, "dovecot-storage");
  g_assert_cmpstr (decoded.correlation_id, ==, "corr-1");
  g_assert_cmpint (decoded.operation, ==,
      WYREBOX_DAEMON_REQUEST_FRAME_OPERATION_MAILBOX_LIST);
  g_assert_nonnull (decoded.mailbox_list);
  g_assert_cmpstr (decoded.mailbox_list->account_identity, ==, "account-1");
  g_assert_cmpstr (decoded.mailbox_list->namespace_prefix, ==, "INBOX");

  g_assert_nonnull (decoded_state_clear);
  g_assert_nonnull (decoded_state);
  decoded_state_clear (decoded_state);
}

static void
assert_request_bytes_unsupported_arm_is_rejected (void)
{
  g_autoptr (GBytes) request = build_delivery_ingestion_request_bytes ();
  g_autoptr (GError) error = NULL;
  WyreboxDaemonDecodedRequestFrame decoded = { 0 };
  gpointer decoded_state = NULL;
  GDestroyNotify decoded_state_clear = NULL;

  g_assert_false (wyrebox_daemon_capnp_codec_decode_request_frame (NULL,
      request,
      &decoded,
      &decoded_state,
      &decoded_state_clear,
      NULL,
      &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED);
  g_assert_null (decoded_state);
  g_assert_null (decoded_state_clear);
}

static void
assert_response_bytes_encode_mailbox_list (void)
{
  g_auto (WyreboxDaemonMailboxListResult) result = { 0 };
  g_auto (WyreboxDaemonResponseFrame) response = { 0 };
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) encoded = NULL;

  wyrebox_daemon_mailbox_list_result_init_empty (&result);
  g_assert_true (wyrebox_daemon_mailbox_list_result_append_entry (&result,
      WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_ORDINARY,
      "mailbox-inbox",
      "INBOX",
      "/",
      "\\Inbox",
      TRUE,
      WYREBOX_DAEMON_MAILBOX_LIST_CHILD_STATE_HAS_NO_CHILDREN,
      &error));
  g_assert_no_error (error);

  g_assert_true (wyrebox_daemon_response_frame_init_mailbox_list (&response,
      "request-1",
      "corr-1",
      &result,
      &error));
  g_assert_no_error (error);

  encoded = wyrebox_daemon_capnp_codec_encode_response_frame (&response,
      NULL,
      &error);
  g_assert_no_error (error);
  g_assert_nonnull (encoded);

  gsize size = 0;
  const guint8 *data = static_cast<const guint8 *> (
      g_bytes_get_data (encoded, &size));
  auto words = kj::arrayPtr (
      reinterpret_cast<const capnp::word *> (data), size / sizeof (capnp::word));
  capnp::FlatArrayMessageReader reader (words);

  auto response_frame = reader.getRoot<ResponseFrame> ();
  g_assert_true (response_frame.which () == ResponseFrame::MAILBOX_LIST);
  g_assert_cmpstr (response_frame.getMailboxList ().getRequestId ().cStr (), ==,
      "request-1");
  g_assert_cmpuint (response_frame.getMailboxList ().getEntries ().size (), ==, 1);
  g_assert_cmpstr (response_frame.getMailboxList ().getEntries ()[0].getMailboxId ().cStr (),
      ==,
      "mailbox-inbox");
  g_assert_cmpstr (
      response_frame.getMailboxList ().getEntries ()[0].getHierarchyDelimiter ().cStr (),
      ==,
      "/");
}

static void
assert_response_bytes_encode_error (void)
{
  g_auto (WyreboxDaemonErrorFrame) error_frame = { 0 };
  g_auto (WyreboxDaemonResponseFrame) response = { 0 };
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) encoded = NULL;

  g_assert_true (wyrebox_daemon_error_frame_init (&error_frame,
      "request-2",
      WYREBOX_DAEMON_ERROR_PERMISSION_DENIED,
      "denied",
      "retry",
      &error));
  g_assert_no_error (error);

  g_assert_true (wyrebox_daemon_response_frame_init_error (&response,
      &error_frame,
      "corr-2",
      &error));
  g_assert_no_error (error);

  encoded = wyrebox_daemon_capnp_codec_encode_response_frame (&response,
      NULL,
      &error);
  g_assert_no_error (error);
  g_assert_nonnull (encoded);

  gsize size = 0;
  const guint8 *data = static_cast<const guint8 *> (
      g_bytes_get_data (encoded, &size));
  auto words = kj::arrayPtr (
      reinterpret_cast<const capnp::word *> (data), size / sizeof (capnp::word));
  capnp::FlatArrayMessageReader reader (words);

  auto response_frame = reader.getRoot<ResponseFrame> ();
  g_assert_true (response_frame.which () == ResponseFrame::ERROR);
  g_assert_cmpstr (response_frame.getError ().getRequestId ().cStr (), ==,
      "request-2");
  g_assert_true (
      response_frame.getError ().getErrorClass () == ErrorClass::PERMISSION_DENIED);
  g_assert_cmpstr (response_frame.getError ().getMessage ().cStr (), ==, "denied");
}

static void
assert_request_adapter_callbacks_are_usable (void)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) request = NULL;
  g_autoptr (GBytes) response_bytes = NULL;
  g_autoptr (WyreboxDaemonMailboxListService) service = NULL;
  g_autoptr (WyreboxDaemonRequestAdapter) adapter = NULL;
  g_autofree FixtureState *service_state = g_new0 (FixtureState, 1);

  service = wyrebox_daemon_mailbox_list_service_new (append_mailbox_fixture,
      service_state,
      NULL);
  g_assert_nonnull (service);

  adapter = wyrebox_daemon_request_adapter_new (NULL,
      service,
      wyrebox_daemon_capnp_codec_decode_request_frame,
      NULL,
      NULL,
      wyrebox_daemon_capnp_codec_encode_response_frame,
      NULL,
      NULL);
  g_assert_nonnull (adapter);

  request = build_mailbox_list_request_bytes ("", "request-1");
  const WyreboxDaemonPeerCredentials peer_credentials = {
    .uid = 100,
    .gid = 101,
    .pid = 102,
  };

  response_bytes = wyrebox_daemon_request_adapter_handle_payload (&peer_credentials,
      request,
      adapter,
      &error);
  g_assert_no_error (error);
  g_assert_nonnull (response_bytes);
  g_assert_true (service_state->was_called);

  gsize size = 0;
  const guint8 *data = static_cast<const guint8 *> (
      g_bytes_get_data (response_bytes, &size));
  auto words = kj::arrayPtr (
      reinterpret_cast<const capnp::word *> (data), size / sizeof (capnp::word));
  capnp::FlatArrayMessageReader reader (words);
  auto response_frame = reader.getRoot<ResponseFrame> ();
  g_assert_true (response_frame.which () == ResponseFrame::MAILBOX_LIST);
  g_assert_cmpstr (response_frame.getMailboxList ().getRequestId ().cStr (), ==,
      "request-1");
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/daemon-api/capnp/codec/decode-mailbox-list",
      assert_request_bytes_decode_mailbox_list);
  g_test_add_func ("/daemon-api/capnp/codec/decode-unsupported-arm",
      assert_request_bytes_unsupported_arm_is_rejected);
  g_test_add_func ("/daemon-api/capnp/codec/encode-mailbox-list",
      assert_response_bytes_encode_mailbox_list);
  g_test_add_func ("/daemon-api/capnp/codec/encode-error",
      assert_response_bytes_encode_error);
  g_test_add_func ("/daemon-api/capnp/codec/request-adapter-compatible",
      assert_request_adapter_callbacks_are_usable);

  return g_test_run ();
}
