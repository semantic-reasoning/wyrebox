#include "wyrebox-daemon-capnp-codec.h"
#include "wyrebox-daemon-fact-mutation-service.h"
#include "wyrebox-daemon-mailbox-list-request.h"
#include "wyrebox-daemon-mailbox-list-result.h"
#include "wyrebox-daemon-mailbox-list-service.h"
#include "wyrebox-daemon-mailbox-select-request.h"
#include "wyrebox-daemon-mailbox-select-result.h"
#include "wyrebox-daemon-mailbox-select-service.h"
#include "wyrebox-daemon-message-fetch-request.h"
#include "wyrebox-daemon-message-fetch-service.h"
#include "wyrebox-daemon-flag-keyword-update-request.h"
#include "wyrebox-daemon-flag-keyword-update-service.h"
#include "wyrebox-daemon-request-adapter.h"
#include "wyrebox-journal-writer.h"

#include <capnp/message.h>
#include <capnp/serialize.h>

#include <glib.h>
#include <glib/gstdio.h>

#include "wyrebox-daemon-api.capnp.h"

typedef struct
{
  gboolean was_called;
} FixtureState;

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
build_mailbox_select_request_bytes (const char *mailbox_id,
    const char *mailbox_name,
    const char *request_id)
{
  capnp::MallocMessageBuilder request_builder;
  auto request_frame = request_builder.initRoot<RequestFrame> ();

  auto identity = request_frame.initIdentity ();
  identity.setRequestId (request_id);
  identity.setCallerIdentity ("dovecot");
  identity.setAccountIdentity ("account-1");
  identity.setToolIdentity ("dovecot-storage");
  identity.setCorrelationId ("corr-select");

  auto mailbox_select_request = request_frame.initMailboxSelect ();
  mailbox_select_request.setAccountIdentity ("account-1");
  mailbox_select_request.setMailboxId (mailbox_id);
  mailbox_select_request.setMailboxName (mailbox_name);

  auto words = capnp::messageToFlatArray (request_builder);
  auto bytes = words.asBytes ();

  return g_bytes_new (bytes.begin (), bytes.size ());
}

static GBytes *
build_fact_mutation_request_bytes (FactMutationKind mutation,
    const char *request_id,
    const char *caller_identity,
    const char *account_identity,
    const char *predicate_id,
    const char *scope_id,
    const char * const *arguments)
{
  guint argument_count = 0;
  capnp::MallocMessageBuilder request_builder;
  auto request_frame = request_builder.initRoot<RequestFrame> ();

  auto identity = request_frame.initIdentity ();
  identity.setRequestId (request_id);
  identity.setCallerIdentity (caller_identity);
  identity.setAccountIdentity (account_identity);
  identity.setToolIdentity ("fact-skill");
  identity.setCorrelationId ("corr-fact");

  auto fact_mutation = request_frame.initFactMutation ();
  fact_mutation.setMutation (mutation);
  fact_mutation.setPredicateId (predicate_id);
  fact_mutation.setScopeId (scope_id);

  while (arguments != NULL && arguments[argument_count] != NULL)
    argument_count++;

  auto encoded_arguments = fact_mutation.initArguments (argument_count);
  for (guint i = 0; i < argument_count; i++)
    encoded_arguments.set (i, arguments[i]);

  auto words = capnp::messageToFlatArray (request_builder);
  auto bytes = words.asBytes ();

  return g_bytes_new (bytes.begin (), bytes.size ());
}

static GBytes *
build_message_fetch_request_bytes (const char *request_id,
    const char *identity_account,
    const char *message_fetch_account,
    const char *mailbox_id,
    guint64 uid_validity,
    guint64 mailbox_uid)
{
  capnp::MallocMessageBuilder request_builder;
  auto request_frame = request_builder.initRoot<RequestFrame> ();

  auto identity = request_frame.initIdentity ();
  identity.setRequestId (request_id);
  identity.setCallerIdentity ("dovecot");
  identity.setAccountIdentity (identity_account);
  identity.setToolIdentity ("dovecot-storage");
  identity.setCorrelationId ("corr-fetch");

  auto message_fetch_request = request_frame.initMessageFetch ();
  message_fetch_request.setAccountIdentity (message_fetch_account);
  message_fetch_request.setMailboxId (mailbox_id);
  message_fetch_request.setUidValidity (uid_validity);
  message_fetch_request.setMailboxUid (mailbox_uid);

  auto words = capnp::messageToFlatArray (request_builder);
  auto bytes = words.asBytes ();

  return g_bytes_new (bytes.begin (), bytes.size ());
}

static GBytes *
build_flag_keyword_update_request_bytes (
    const char *request_id,
    const char *request_account,
    const char *flag_account,
    const char *mailbox_id,
    guint64 uid_validity,
    guint64 mailbox_uid,
    FlagKeywordUpdateMode mode,
    const char * const *system_flags,
    const char * const *user_keywords)
{
  gsize system_flag_count = 0;
  gsize user_keyword_count = 0;
  capnp::MallocMessageBuilder request_builder;
  auto request_frame = request_builder.initRoot<RequestFrame> ();

  auto identity = request_frame.initIdentity ();
  identity.setRequestId (request_id);
  identity.setCallerIdentity ("dovecot");
  identity.setAccountIdentity (request_account);
  identity.setToolIdentity ("dovecot-storage");
  identity.setCorrelationId ("corr-flag");

  auto flag_keyword_update_request = request_frame.initFlagKeywordUpdate ();
  flag_keyword_update_request.setAccountIdentity (flag_account);
  flag_keyword_update_request.setMailboxId (mailbox_id);
  flag_keyword_update_request.setUidValidity (uid_validity);
  flag_keyword_update_request.setMailboxUid (mailbox_uid);
  flag_keyword_update_request.setMode (mode);

  if (system_flags != NULL) {
    while (system_flags[system_flag_count] != NULL)
      system_flag_count++;
    auto encoded_system_flags = flag_keyword_update_request.initSystemFlags (
        system_flag_count);
    for (guint i = 0; i < system_flag_count; i++)
      encoded_system_flags.set (i, system_flags[i]);
  }

  if (user_keywords != NULL) {
    while (user_keywords[user_keyword_count] != NULL)
      user_keyword_count++;
    auto encoded_user_keywords = flag_keyword_update_request.initUserKeywords (
        user_keyword_count);
    for (guint i = 0; i < user_keyword_count; i++)
      encoded_user_keywords.set (i, user_keywords[i]);
  }

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
select_mailbox_fixture (const WyreboxDaemonRequestIdentity *identity,
    const WyreboxDaemonMailboxSelectRequest *request,
    WyreboxDaemonMailboxSelectResult *out_result,
    gpointer user_data,
    GError **error)
{
  FixtureState *state = static_cast<FixtureState *> (user_data);

  g_assert_nonnull (identity);
  g_assert_nonnull (request);

  g_assert_cmpstr (identity->request_id, ==, "request-select-route");
  g_assert_cmpstr (request->account_identity, ==, "account-1");
  g_assert_cmpstr (request->mailbox_id, ==, "mailbox-inbox");
  g_assert_null (request->mailbox_name);

  state->was_called = TRUE;

  return wyrebox_daemon_mailbox_select_result_init (out_result,
      WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_ORDINARY,
      "mailbox-inbox",
      "INBOX",
      77,
      42,
      error);
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

static gboolean
fetch_message_fixture (const WyreboxDaemonRequestIdentity *identity,
    const WyreboxDaemonMessageFetchRequest *request,
    WyreboxDaemonStreamChunkFrame *out_chunk,
    gpointer user_data,
    GError **error)
{
  const char *payload = "message-1-body";
  g_autoptr (GBytes) bytes = NULL;
  FixtureState *state = static_cast<FixtureState *> (user_data);

  g_assert_cmpstr (identity->request_id, ==, "request-fetch-route");
  g_assert_cmpstr (identity->caller_identity, ==, "dovecot");
  g_assert_cmpstr (identity->account_identity, ==, "account-1");
  g_assert_cmpstr (identity->tool_identity, ==, "dovecot-storage");
  g_assert_cmpstr (identity->correlation_id, ==, "corr-fetch");
  g_assert_cmpstr (request->account_identity, ==, "account-1");
  g_assert_cmpstr (request->mailbox_id, ==, "mailbox-inbox");
  g_assert_cmpuint (request->uid_validity, ==, 77);
  g_assert_cmpuint (request->mailbox_uid, ==, 42);

  state->was_called = TRUE;
  bytes = g_bytes_new_static (payload, strlen (payload));

  return wyrebox_daemon_stream_chunk_frame_init (out_chunk,
      identity->request_id,
      "message-1",
      NULL,
      identity->correlation_id,
      0,
      bytes,
      TRUE,
      error);
}

static gboolean
update_flag_keyword_fixture (const WyreboxDaemonRequestIdentity *identity,
    const WyreboxDaemonFlagKeywordUpdateRequest *request,
    WyreboxDaemonSuccessReceipt *out_receipt, gpointer user_data,
    GError **error)
{
  gboolean *was_called = static_cast<gboolean *> (user_data);

  g_assert_cmpstr (identity->request_id, ==, "request-flag-keyword");
  g_assert_cmpstr (identity->caller_identity, ==, "dovecot");
  g_assert_cmpstr (identity->account_identity, ==, "account-1");
  g_assert_cmpstr (identity->tool_identity, ==, "dovecot-storage");
  g_assert_cmpstr (identity->correlation_id, ==, "corr-flag");
  g_assert_cmpstr (request->account_identity, ==, "account-1");
  g_assert_cmpstr (request->mailbox_id, ==, "mailbox-inbox");
  g_assert_cmpuint (request->uid_validity, ==, 77);
  g_assert_cmpuint (request->mailbox_uid, ==, 42);
  g_assert_cmpint (request->mode, ==,
      WYREBOX_DAEMON_FLAG_KEYWORD_UPDATE_MODE_SET);
  g_assert_cmpstr (request->system_flags[0], ==, "\\Seen");
  g_assert_cmpstr (request->system_flags[1], ==, "\\Flagged");
  g_assert_cmpstr (request->user_keywords[0], ==, "project");
  g_assert_cmpstr (request->user_keywords[1], ==, "todo");

  *was_called = TRUE;

  out_receipt->request_id = g_strdup (identity->request_id);
  out_receipt->durable_marker = g_strdup ("journal:123:456");
  out_receipt->journal_offset = 123;
  out_receipt->journal_sequence = 456;
  out_receipt->summary = g_strdup ("flag keyword update done");

  return TRUE;
}

static void
assert_request_bytes_decode_mailbox_select_by_id (void)
{
  g_autoptr (GBytes) request = build_mailbox_select_request_bytes (
      "mailbox-inbox",
      "",
      "request-select-id");
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
  g_assert_cmpstr (decoded.request_id, ==, "request-select-id");
  g_assert_cmpstr (decoded.caller_identity, ==, "dovecot");
  g_assert_cmpstr (decoded.account_identity, ==, "account-1");
  g_assert_cmpstr (decoded.tool_identity, ==, "dovecot-storage");
  g_assert_cmpstr (decoded.correlation_id, ==, "corr-select");
  g_assert_cmpint (decoded.operation, ==,
      WYREBOX_DAEMON_REQUEST_FRAME_OPERATION_MAILBOX_SELECT);
  g_assert_nonnull (decoded.mailbox_select);
  g_assert_cmpstr (decoded.mailbox_select->account_identity, ==, "account-1");
  g_assert_cmpstr (decoded.mailbox_select->mailbox_id, ==, "mailbox-inbox");
  g_assert_null (decoded.mailbox_select->mailbox_name);
  g_assert_null (decoded.mailbox_list);
  g_assert_null (decoded.fact_mutation);

  g_assert_nonnull (decoded_state_clear);
  g_assert_nonnull (decoded_state);
  decoded_state_clear (decoded_state);
}

static void
assert_request_bytes_decode_mailbox_select_by_name (void)
{
  g_autoptr (GBytes) request = build_mailbox_select_request_bytes (
      "",
      "Projects/Project A",
      "request-select-name");
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
  g_assert_cmpstr (decoded.request_id, ==, "request-select-name");
  g_assert_cmpint (decoded.operation, ==,
      WYREBOX_DAEMON_REQUEST_FRAME_OPERATION_MAILBOX_SELECT);
  g_assert_nonnull (decoded.mailbox_select);
  g_assert_cmpstr (decoded.mailbox_select->account_identity, ==, "account-1");
  g_assert_null (decoded.mailbox_select->mailbox_id);
  g_assert_cmpstr (decoded.mailbox_select->mailbox_name, ==,
      "Projects/Project A");

  g_assert_nonnull (decoded_state_clear);
  g_assert_nonnull (decoded_state);
  decoded_state_clear (decoded_state);
}

static void
assert_mailbox_select_decode_rejects_missing_selector (void)
{
  g_autoptr (GBytes) request = build_mailbox_select_request_bytes ("",
      "",
      "request-select-invalid");
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
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_null (decoded_state);
  g_assert_null (decoded_state_clear);
}

static void
assert_mailbox_select_decode_rejects_both_selectors (void)
{
  g_autoptr (GBytes) request = build_mailbox_select_request_bytes (
      "mailbox-inbox",
      "INBOX",
      "request-select-invalid");
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
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_null (decoded_state);
  g_assert_null (decoded_state_clear);
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
assert_request_bytes_decode_message_fetch (void)
{
  g_autoptr (GBytes) request = build_message_fetch_request_bytes (
      "request-fetch",
      "account-1",
      "account-1",
      "mailbox-inbox",
      77,
      42);
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
  g_assert_cmpstr (decoded.request_id, ==, "request-fetch");
  g_assert_cmpstr (decoded.caller_identity, ==, "dovecot");
  g_assert_cmpstr (decoded.account_identity, ==, "account-1");
  g_assert_cmpstr (decoded.tool_identity, ==, "dovecot-storage");
  g_assert_cmpstr (decoded.correlation_id, ==, "corr-fetch");
  g_assert_cmpint (decoded.operation, ==,
      WYREBOX_DAEMON_REQUEST_FRAME_OPERATION_MESSAGE_FETCH);
  g_assert_nonnull (decoded.message_fetch);
  g_assert_cmpstr (decoded.message_fetch->account_identity, ==, "account-1");
  g_assert_cmpstr (decoded.message_fetch->mailbox_id, ==, "mailbox-inbox");
  g_assert_cmpuint (decoded.message_fetch->uid_validity, ==, 77);
  g_assert_cmpuint (decoded.message_fetch->mailbox_uid, ==, 42);
  g_assert_null (decoded.mailbox_list);
  g_assert_null (decoded.mailbox_select);
  g_assert_null (decoded.fact_mutation);

  g_assert_nonnull (decoded_state_clear);
  g_assert_nonnull (decoded_state);
  decoded_state_clear (decoded_state);
}

static void
assert_request_bytes_decode_flag_keyword_update_set (void)
{
  const char *system_flags[] = { "\\Seen", "\\Flagged", NULL };
  const char *user_keywords[] = { "project", "todo", NULL };
  g_autoptr (GBytes) request = build_flag_keyword_update_request_bytes (
      "request-flag-keyword",
      "account-1",
      "account-1",
      "mailbox-inbox",
      77,
      42,
      FlagKeywordUpdateMode::SET,
      system_flags,
      user_keywords);
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
  g_assert_cmpstr (decoded.request_id, ==, "request-flag-keyword");
  g_assert_cmpstr (decoded.caller_identity, ==, "dovecot");
  g_assert_cmpstr (decoded.account_identity, ==, "account-1");
  g_assert_cmpstr (decoded.tool_identity, ==, "dovecot-storage");
  g_assert_cmpstr (decoded.correlation_id, ==, "corr-flag");
  g_assert_cmpint (decoded.operation, ==,
      WYREBOX_DAEMON_REQUEST_FRAME_OPERATION_FLAG_KEYWORD_UPDATE);
  g_assert_nonnull (decoded.flag_keyword_update);
  g_assert_cmpint (decoded.flag_keyword_update->mode, ==,
      WYREBOX_DAEMON_FLAG_KEYWORD_UPDATE_MODE_SET);
  g_assert_cmpstr (decoded.flag_keyword_update->system_flags[0], ==, "\\Seen");
  g_assert_cmpstr (decoded.flag_keyword_update->system_flags[1], ==, "\\Flagged");
  g_assert_cmpstr (decoded.flag_keyword_update->user_keywords[0], ==, "project");
  g_assert_cmpstr (decoded.flag_keyword_update->user_keywords[1], ==, "todo");

  g_assert_nonnull (decoded_state_clear);
  g_assert_nonnull (decoded_state);
  decoded_state_clear (decoded_state);
}

static void
assert_request_bytes_decode_flag_keyword_update_mode_mapping (void)
{
  const char *system_flags[] = { "\\Seen", NULL };
  const char *user_keywords[] = { "project", NULL };
  g_autoptr (GBytes) clear_request = build_flag_keyword_update_request_bytes (
      "request-flag-keyword-clear",
      "account-1",
      "account-1",
      "mailbox-inbox",
      77,
      42,
      FlagKeywordUpdateMode::CLEAR,
      system_flags,
      user_keywords);
  g_autoptr (GBytes) replace_request = build_flag_keyword_update_request_bytes (
      "request-flag-keyword-replace",
      "account-1",
      "account-1",
      "mailbox-inbox",
      77,
      42,
      FlagKeywordUpdateMode::REPLACE,
      NULL,
      NULL);
  g_autoptr (GBytes) set_request = build_flag_keyword_update_request_bytes (
      "request-flag-keyword-set",
      "account-1",
      "account-1",
      "mailbox-inbox",
      77,
      42,
      FlagKeywordUpdateMode::SET,
      system_flags,
      NULL);
  g_autoptr (GError) error = NULL;
  WyreboxDaemonDecodedRequestFrame decoded = { 0 };
  gpointer decoded_state = NULL;
  GDestroyNotify decoded_state_clear = NULL;

  g_assert_true (wyrebox_daemon_capnp_codec_decode_request_frame (NULL,
      set_request,
      &decoded,
      &decoded_state,
      &decoded_state_clear,
      NULL,
      &error));
  g_assert_no_error (error);
  g_assert_cmpint (decoded.flag_keyword_update->mode, ==,
      WYREBOX_DAEMON_FLAG_KEYWORD_UPDATE_MODE_SET);
  g_assert_nonnull (decoded_state_clear);
  decoded_state_clear (decoded_state);

  g_assert_true (wyrebox_daemon_capnp_codec_decode_request_frame (NULL,
      clear_request,
      &decoded,
      &decoded_state,
      &decoded_state_clear,
      NULL,
      &error));
  g_assert_no_error (error);
  g_assert_cmpint (decoded.flag_keyword_update->mode, ==,
      WYREBOX_DAEMON_FLAG_KEYWORD_UPDATE_MODE_CLEAR);
  g_assert_nonnull (decoded_state_clear);
  decoded_state_clear (decoded_state);

  g_assert_true (wyrebox_daemon_capnp_codec_decode_request_frame (NULL,
      replace_request,
      &decoded,
      &decoded_state,
      &decoded_state_clear,
      NULL,
      &error));
  g_assert_no_error (error);
  g_assert_cmpint (decoded.flag_keyword_update->mode, ==,
      WYREBOX_DAEMON_FLAG_KEYWORD_UPDATE_MODE_REPLACE);
  g_assert_nonnull (decoded_state_clear);
  decoded_state_clear (decoded_state);
}

static void
assert_request_bytes_decode_flag_keyword_update_replace_empty_payload (void)
{
  g_autoptr (GBytes) request = build_flag_keyword_update_request_bytes (
      "request-flag-keyword-replace-empty",
      "account-1",
      "account-1",
      "mailbox-inbox",
      77,
      42,
      FlagKeywordUpdateMode::REPLACE,
      NULL,
      NULL);
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
  g_assert_cmpint (decoded.flag_keyword_update->mode, ==,
      WYREBOX_DAEMON_FLAG_KEYWORD_UPDATE_MODE_REPLACE);
  g_assert_null (decoded.flag_keyword_update->system_flags);
  g_assert_null (decoded.flag_keyword_update->user_keywords);

  g_assert_nonnull (decoded_state_clear);
  g_assert_nonnull (decoded_state);
  decoded_state_clear (decoded_state);
}

static void
assert_request_bytes_rejects_flag_keyword_update_invalid_mode (void)
{
  const char *system_flags[] = { "\\Seen", NULL };
  g_autoptr (GBytes) request = NULL;
  g_autoptr (GError) error = NULL;
  WyreboxDaemonDecodedRequestFrame decoded = { 0 };
  gpointer decoded_state = NULL;
  GDestroyNotify decoded_state_clear = NULL;
  capnp::MallocMessageBuilder request_builder;
  auto request_frame = request_builder.initRoot<RequestFrame> ();
  auto identity = request_frame.initIdentity ();

  identity.setRequestId ("request-flag-keyword-invalid-mode");
  identity.setCallerIdentity ("dovecot");
  identity.setAccountIdentity ("account-1");
  identity.setToolIdentity ("dovecot-storage");
  identity.setCorrelationId ("corr-flag");

  auto flag_keyword_update_request = request_frame.initFlagKeywordUpdate ();
  flag_keyword_update_request.setAccountIdentity ("account-1");
  flag_keyword_update_request.setMailboxId ("mailbox-inbox");
  flag_keyword_update_request.setUidValidity (77);
  flag_keyword_update_request.setMailboxUid (42);
  flag_keyword_update_request.setMode (static_cast<FlagKeywordUpdateMode> (99));
  auto encoded_system_flags = flag_keyword_update_request.initSystemFlags (1);
  encoded_system_flags.set (0, system_flags[0]);

  auto words = capnp::messageToFlatArray (request_builder);
  auto bytes = words.asBytes ();
  request = g_bytes_new (bytes.begin (), bytes.size ());

  g_assert_false (wyrebox_daemon_capnp_codec_decode_request_frame (NULL,
      request,
      &decoded,
      &decoded_state,
      &decoded_state_clear,
      NULL,
      &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_null (decoded_state);
  g_assert_null (decoded_state_clear);
}

static void
assert_request_bytes_rejects_flag_keyword_update_missing_payload_for_set (void)
{
  g_autoptr (GBytes) request = build_flag_keyword_update_request_bytes (
      "request-flag-keyword-missing-payload",
      "account-1",
      "account-1",
      "mailbox-inbox",
      77,
      42,
      FlagKeywordUpdateMode::SET,
      NULL,
      NULL);
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
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_null (decoded_state);
  g_assert_null (decoded_state_clear);
}

static void
assert_request_bytes_rejects_flag_keyword_update_missing_account (void)
{
  const char *system_flags[] = { "\\Seen", NULL };
  g_autoptr (GBytes) request = build_flag_keyword_update_request_bytes (
      "request-flag-keyword-missing-account",
      "account-1",
      "",
      "mailbox-inbox",
      77,
      42,
      FlagKeywordUpdateMode::SET,
      system_flags,
      NULL);
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
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_null (decoded_state);
  g_assert_null (decoded_state_clear);
}

static void
assert_request_bytes_rejects_flag_keyword_update_missing_mailbox (void)
{
  const char *system_flags[] = { "\\Seen", NULL };
  g_autoptr (GBytes) request = build_flag_keyword_update_request_bytes (
      "request-flag-keyword-missing-mailbox",
      "account-1",
      "account-1",
      "",
      77,
      42,
      FlagKeywordUpdateMode::SET,
      system_flags,
      NULL);
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
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_null (decoded_state);
  g_assert_null (decoded_state_clear);
}

static void
assert_request_bytes_rejects_flag_keyword_update_zero_uid_validity (void)
{
  const char *system_flags[] = { "\\Seen", NULL };
  g_autoptr (GBytes) request = build_flag_keyword_update_request_bytes (
      "request-flag-keyword-zero-uid-validity",
      "account-1",
      "account-1",
      "mailbox-inbox",
      0,
      42,
      FlagKeywordUpdateMode::SET,
      system_flags,
      NULL);
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
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_null (decoded_state);
  g_assert_null (decoded_state_clear);
}

static void
assert_request_bytes_rejects_flag_keyword_update_zero_mailbox_uid (void)
{
  const char *system_flags[] = { "\\Seen", NULL };
  g_autoptr (GBytes) request = build_flag_keyword_update_request_bytes (
      "request-flag-keyword-zero-mailbox-uid",
      "account-1",
      "account-1",
      "mailbox-inbox",
      77,
      0,
      FlagKeywordUpdateMode::SET,
      system_flags,
      NULL);
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
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_null (decoded_state);
  g_assert_null (decoded_state_clear);
}

static void
assert_request_bytes_rejects_flag_keyword_update_unknown_system_flag (void)
{
  const char *system_flags[] = { "not-a-system-flag", NULL };
  g_autoptr (GBytes) request = build_flag_keyword_update_request_bytes (
      "request-flag-keyword-unknown-system-flag",
      "account-1",
      "account-1",
      "mailbox-inbox",
      77,
      42,
      FlagKeywordUpdateMode::SET,
      system_flags,
      NULL);
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
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_null (decoded_state);
  g_assert_null (decoded_state_clear);
}

static void
assert_request_bytes_rejects_flag_keyword_update_recent_system_flag (void)
{
  const char *system_flags[] = { "\\Recent", NULL };
  g_autoptr (GBytes) request = build_flag_keyword_update_request_bytes (
      "request-flag-keyword-recent-system-flag",
      "account-1",
      "account-1",
      "mailbox-inbox",
      77,
      42,
      FlagKeywordUpdateMode::SET,
      system_flags,
      NULL);
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
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_null (decoded_state);
  g_assert_null (decoded_state_clear);
}

static void
assert_request_bytes_rejects_flag_keyword_update_system_flag_keyword (void)
{
  const char *user_keywords[] = { "\\Seen", NULL };
  g_autoptr (GBytes) request = build_flag_keyword_update_request_bytes (
      "request-flag-keyword-system-flag-keyword",
      "account-1",
      "account-1",
      "mailbox-inbox",
      77,
      42,
      FlagKeywordUpdateMode::SET,
      NULL,
      user_keywords);
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
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_null (decoded_state);
  g_assert_null (decoded_state_clear);
}

static void
assert_request_bytes_rejects_flag_keyword_update_invalid_keyword (void)
{
  const char *user_keywords[] = { "needs review", NULL };
  g_autoptr (GBytes) request = build_flag_keyword_update_request_bytes (
      "request-flag-keyword-invalid-keyword",
      "account-1",
      "account-1",
      "mailbox-inbox",
      77,
      42,
      FlagKeywordUpdateMode::SET,
      NULL,
      user_keywords);
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
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_null (decoded_state);
  g_assert_null (decoded_state_clear);
}

static void
assert_request_bytes_rejects_flag_keyword_update_duplicate_flag (void)
{
  const char *system_flags[] = { "\\Seen", "\\Seen", NULL };
  g_autoptr (GBytes) request = build_flag_keyword_update_request_bytes (
      "request-flag-keyword-duplicate-flag",
      "account-1",
      "account-1",
      "mailbox-inbox",
      77,
      42,
      FlagKeywordUpdateMode::SET,
      system_flags,
      NULL);
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
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_null (decoded_state);
  g_assert_null (decoded_state_clear);
}

static void
assert_request_bytes_rejects_flag_keyword_update_duplicate_keyword (void)
{
  const char *user_keywords[] = { "project", "project", NULL };
  g_autoptr (GBytes) request = build_flag_keyword_update_request_bytes (
      "request-flag-keyword-duplicate-keyword",
      "account-1",
      "account-1",
      "mailbox-inbox",
      77,
      42,
      FlagKeywordUpdateMode::SET,
      NULL,
      user_keywords);
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
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_null (decoded_state);
  g_assert_null (decoded_state_clear);
}

static void
assert_request_bytes_rejects_message_fetch_missing_account_identity (void)
{
  g_autoptr (GBytes) request = build_message_fetch_request_bytes (
      "request-fetch-missing-account",
      "account-1",
      "",
      "mailbox-inbox",
      77,
      42);
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
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_null (decoded_state);
  g_assert_null (decoded_state_clear);
}

static void
assert_request_bytes_rejects_message_fetch_missing_mailbox_id (void)
{
  g_autoptr (GBytes) request = build_message_fetch_request_bytes (
      "request-fetch-missing-mailbox",
      "account-1",
      "account-1",
      "",
      77,
      42);
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
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_null (decoded_state);
  g_assert_null (decoded_state_clear);
}

static void
assert_request_bytes_rejects_message_fetch_zero_uid_validity (void)
{
  g_autoptr (GBytes) request = build_message_fetch_request_bytes (
      "request-fetch-zero-uid-validity",
      "account-1",
      "account-1",
      "mailbox-inbox",
      0,
      42);
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
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_null (decoded_state);
  g_assert_null (decoded_state_clear);
}

static void
assert_request_bytes_rejects_message_fetch_zero_mailbox_uid (void)
{
  g_autoptr (GBytes) request = build_message_fetch_request_bytes (
      "request-fetch-zero-mailbox-uid",
      "account-1",
      "account-1",
      "mailbox-inbox",
      77,
      0);
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
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_null (decoded_state);
  g_assert_null (decoded_state_clear);
}

static void
assert_request_bytes_decode_fact_mutation_insert (void)
{
  const char *arguments[] = { "mail-1", "project-a", NULL };
  g_autoptr (GBytes) request = build_fact_mutation_request_bytes (
      FactMutationKind::INSERT,
      "request-fact-insert",
      "skill",
      "account-1",
      "project_mention",
      "account-1",
      arguments);
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
  g_assert_cmpstr (decoded.request_id, ==, "request-fact-insert");
  g_assert_cmpstr (decoded.caller_identity, ==, "skill");
  g_assert_cmpstr (decoded.account_identity, ==, "account-1");
  g_assert_cmpstr (decoded.tool_identity, ==, "fact-skill");
  g_assert_cmpstr (decoded.correlation_id, ==, "corr-fact");
  g_assert_cmpint (decoded.operation, ==,
      WYREBOX_DAEMON_REQUEST_FRAME_OPERATION_FACT_MUTATION);
  g_assert_nonnull (decoded.fact_mutation);
  g_assert_cmpint (decoded.fact_mutation->mutation, ==,
      WYREBOX_DAEMON_FACT_MUTATION_INSERT);
  g_assert_cmpstr (decoded.fact_mutation->predicate_id, ==,
      "project_mention");
  g_assert_cmpstr (decoded.fact_mutation->scope_id, ==, "account-1");
  g_assert_cmpstr (decoded.fact_mutation->arguments[0], ==, "mail-1");
  g_assert_cmpstr (decoded.fact_mutation->arguments[1], ==, "project-a");
  g_assert_null (decoded.fact_mutation->arguments[2]);
  g_assert_null (decoded.mailbox_list);

  g_assert_nonnull (decoded_state_clear);
  g_assert_nonnull (decoded_state);
  decoded_state_clear (decoded_state);
}

static void
assert_request_bytes_decode_fact_mutation_retract (void)
{
  const char *arguments[] = { "mail-1", NULL };
  g_autoptr (GBytes) request = build_fact_mutation_request_bytes (
      FactMutationKind::RETRACT,
      "request-fact-retract",
      "skill",
      "account-1",
      "project_mention",
      "account-1",
      arguments);
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
  g_assert_cmpint (decoded.operation, ==,
      WYREBOX_DAEMON_REQUEST_FRAME_OPERATION_FACT_MUTATION);
  g_assert_nonnull (decoded.fact_mutation);
  g_assert_cmpint (decoded.fact_mutation->mutation, ==,
      WYREBOX_DAEMON_FACT_MUTATION_RETRACT);
  g_assert_cmpstr (decoded.fact_mutation->arguments[0], ==, "mail-1");
  g_assert_null (decoded.fact_mutation->arguments[1]);

  g_assert_nonnull (decoded_state_clear);
  g_assert_nonnull (decoded_state);
  decoded_state_clear (decoded_state);
}

static void
assert_fact_mutation_decode_rejects_missing_predicate (void)
{
  const char *arguments[] = { "mail-1", NULL };
  g_autoptr (GBytes) request = build_fact_mutation_request_bytes (
      FactMutationKind::INSERT,
      "request-fact-invalid",
      "skill",
      "account-1",
      "",
      "account-1",
      arguments);
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
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_null (decoded_state);
  g_assert_null (decoded_state_clear);
}

static void
assert_fact_mutation_decode_rejects_missing_scope (void)
{
  const char *arguments[] = { "mail-1", NULL };
  g_autoptr (GBytes) request = build_fact_mutation_request_bytes (
      FactMutationKind::INSERT,
      "request-fact-invalid",
      "skill",
      "account-1",
      "project_mention",
      "",
      arguments);
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
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_null (decoded_state);
  g_assert_null (decoded_state_clear);
}

static void
assert_fact_mutation_decode_rejects_empty_argument (void)
{
  const char *arguments[] = { "", NULL };
  g_autoptr (GBytes) request = build_fact_mutation_request_bytes (
      FactMutationKind::INSERT,
      "request-fact-invalid",
      "skill",
      "account-1",
      "project_mention",
      "account-1",
      arguments);
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
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_null (decoded_state);
  g_assert_null (decoded_state_clear);
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
assert_response_bytes_encode_mailbox_select (void)
{
  g_auto (WyreboxDaemonMailboxSelectResult) result = {};
  g_auto (WyreboxDaemonResponseFrame) response = { 0 };
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) encoded = NULL;

  g_assert_true (wyrebox_daemon_mailbox_select_result_init (&result,
      WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_VIRTUAL,
      "view-project-a",
      "Projects/Project A",
      99,
      1234,
      &error));
  g_assert_no_error (error);

  g_assert_true (wyrebox_daemon_response_frame_init_mailbox_select (&response,
      "request-select-response",
      "corr-select-response",
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
  g_assert_true (response_frame.which () == ResponseFrame::MAILBOX_SELECT);
  g_assert_cmpstr (response_frame.getRequestId ().cStr (), ==,
      "request-select-response");
  g_assert_cmpstr (response_frame.getCorrelationId ().cStr (), ==,
      "corr-select-response");
  g_assert_cmpstr (response_frame.getMailboxSelect ().getRequestId ().cStr (),
      ==,
      "request-select-response");
  g_assert_true (response_frame.getMailboxSelect ().getKind () ==
      MailboxListEntryKind::VIRTUAL);
  g_assert_cmpstr (response_frame.getMailboxSelect ().getMailboxId ().cStr (),
      ==,
      "view-project-a");
  g_assert_cmpstr (response_frame.getMailboxSelect ().getMailboxName ().cStr (),
      ==,
      "Projects/Project A");
  g_assert_cmpuint (response_frame.getMailboxSelect ().getUidValidity (), ==,
      99);
  g_assert_cmpuint (response_frame.getMailboxSelect ().getUidNext (), ==,
      1234);
}

static void
assert_response_bytes_encode_stream_chunk (void)
{
  const guint8 payload[] = { 0xde, 0xad, 0xbe, 0xef };
  g_autoptr (GBytes) chunk_bytes = g_bytes_new_static (payload,
      sizeof (payload));
  g_auto (WyreboxDaemonStreamChunkFrame) chunk = { 0 };
  g_auto (WyreboxDaemonResponseFrame) response = { 0 };
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) encoded = NULL;

  g_assert_true (wyrebox_daemon_stream_chunk_frame_init (&chunk,
      "request-stream",
      "message-1",
      NULL,
      "corr-stream",
      42,
      chunk_bytes,
      FALSE,
      &error));
  g_assert_no_error (error);

  g_assert_true (wyrebox_daemon_response_frame_init_stream_chunk (&response,
      &chunk,
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
  g_assert_true (response_frame.which () == ResponseFrame::STREAM_CHUNK);
  g_assert_cmpstr (response_frame.getRequestId ().cStr (), ==,
      "request-stream");
  g_assert_cmpstr (response_frame.getCorrelationId ().cStr (), ==,
      "corr-stream");
  g_assert_cmpstr (response_frame.getStreamChunk ().getRequestId ().cStr (), ==,
      "request-stream");
  g_assert_cmpstr (response_frame.getStreamChunk ().getMessageId ().cStr (), ==,
      "message-1");
  g_assert_cmpstr (response_frame.getStreamChunk ().getQueryId ().cStr (), ==,
      "");
  g_assert_cmpstr (response_frame.getStreamChunk ().getCorrelationId ().cStr (),
      ==,
      "corr-stream");
  g_assert_cmpuint (response_frame.getStreamChunk ().getChunkIndex (), ==, 42);
  g_assert_false (response_frame.getStreamChunk ().getEndOfStream ());
  g_assert_cmpuint (response_frame.getStreamChunk ().getBytes ().size (), ==,
      sizeof (payload));
  g_assert_cmpuint (response_frame.getStreamChunk ().getBytes ()[0], ==, 0xde);
  g_assert_cmpuint (response_frame.getStreamChunk ().getBytes ()[1], ==, 0xad);
  g_assert_cmpuint (response_frame.getStreamChunk ().getBytes ()[2], ==, 0xbe);
  g_assert_cmpuint (response_frame.getStreamChunk ().getBytes ()[3], ==, 0xef);
}

static void
assert_response_bytes_reject_ambiguous_stream_chunk (void)
{
  const guint8 payload[] = { 0xde, 0xad };
  g_autoptr (GBytes) chunk_bytes = g_bytes_new_static (payload,
      sizeof (payload));
  g_auto (WyreboxDaemonResponseFrame) response = { 0 };
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) encoded = NULL;

  response.kind = WYREBOX_DAEMON_RESPONSE_FRAME_STREAM_CHUNK;
  response.request_id = g_strdup ("request-stream");
  response.stream_chunk.request_id = g_strdup ("request-stream");
  response.stream_chunk.message_id = g_strdup ("message-1");
  response.stream_chunk.query_id = g_strdup ("query-1");
  response.stream_chunk.chunk_index = 0;
  response.stream_chunk.bytes = g_bytes_ref (chunk_bytes);

  encoded = wyrebox_daemon_capnp_codec_encode_response_frame (&response,
      NULL,
      &error);
  g_assert_null (encoded);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
}

static void
assert_response_bytes_reject_mismatched_stream_chunk_request (void)
{
  const guint8 payload[] = { 0xca, 0xfe };
  g_autoptr (GBytes) chunk_bytes = g_bytes_new_static (payload,
      sizeof (payload));
  g_auto (WyreboxDaemonResponseFrame) response = { 0 };
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) encoded = NULL;

  response.kind = WYREBOX_DAEMON_RESPONSE_FRAME_STREAM_CHUNK;
  response.request_id = g_strdup ("request-envelope");
  response.stream_chunk.request_id = g_strdup ("request-payload");
  response.stream_chunk.message_id = g_strdup ("message-1");
  response.stream_chunk.chunk_index = 0;
  response.stream_chunk.bytes = g_bytes_ref (chunk_bytes);

  encoded = wyrebox_daemon_capnp_codec_encode_response_frame (&response,
      NULL,
      &error);
  g_assert_null (encoded);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
}

static void
assert_response_bytes_encode_final_empty_stream_chunk (void)
{
  g_auto (WyreboxDaemonStreamChunkFrame) chunk = { 0 };
  g_auto (WyreboxDaemonResponseFrame) response = { 0 };
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) encoded = NULL;

  g_assert_true (wyrebox_daemon_stream_chunk_frame_init (&chunk,
      "request-stream-final",
      NULL,
      "query-1",
      NULL,
      43,
      NULL,
      TRUE,
      &error));
  g_assert_no_error (error);

  g_assert_true (wyrebox_daemon_response_frame_init_stream_chunk (&response,
      &chunk,
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
  g_assert_true (response_frame.which () == ResponseFrame::STREAM_CHUNK);
  g_assert_cmpstr (response_frame.getStreamChunk ().getRequestId ().cStr (), ==,
      "request-stream-final");
  g_assert_cmpstr (response_frame.getStreamChunk ().getMessageId ().cStr (), ==,
      "");
  g_assert_cmpstr (response_frame.getStreamChunk ().getQueryId ().cStr (), ==,
      "query-1");
  g_assert_cmpuint (response_frame.getStreamChunk ().getChunkIndex (), ==, 43);
  g_assert_true (response_frame.getStreamChunk ().getEndOfStream ());
  g_assert_cmpuint (response_frame.getStreamChunk ().getBytes ().size (), ==, 0);
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
assert_response_bytes_encode_success (void)
{
  const char *arguments[] = { "mail-1", NULL };
  g_auto (WyreboxDaemonFactMutationRequest) request = {};
  g_auto (WyreboxDaemonResponseFrame) response = { 0 };
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) encoded = NULL;

  g_assert_true (wyrebox_daemon_fact_mutation_request_init (&request,
      WYREBOX_DAEMON_FACT_MUTATION_INSERT,
      "project_mention",
      "account-1",
      arguments,
      &error));
  g_assert_no_error (error);

  g_assert_true (wyrebox_daemon_response_frame_init_fact_mutation_success (
      &response,
      "request-fact-success",
      "corr-fact",
      &request,
      4096,
      7,
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
  g_assert_true (response_frame.which () == ResponseFrame::SUCCESS);
  g_assert_cmpstr (response_frame.getRequestId ().cStr (), ==,
      "request-fact-success");
  g_assert_cmpstr (response_frame.getCorrelationId ().cStr (), ==,
      "corr-fact");
  g_assert_cmpstr (response_frame.getSuccess ().getRequestId ().cStr (), ==,
      "request-fact-success");
  g_assert_cmpstr (response_frame.getSuccess ().getDurableMarker ().cStr (), ==,
      "journal:4096:7");
  g_assert_cmpuint (response_frame.getSuccess ().getJournalOffset (), ==,
      4096);
  g_assert_cmpuint (response_frame.getSuccess ().getJournalSequence (), ==,
      7);
  g_assert_cmpstr (response_frame.getSuccess ().getSummary ().cStr (), ==,
      "fact_mutation mutation=insert predicate_id=project_mention "
      "scope_id=account-1 argument_count=1");
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
      NULL,
      NULL,
      NULL,
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

static void
assert_request_adapter_routes_mailbox_select (void)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) request = NULL;
  g_autoptr (GBytes) response_bytes = NULL;
  g_autoptr (WyreboxDaemonMailboxSelectService) service = NULL;
  g_autoptr (WyreboxDaemonRequestAdapter) adapter = NULL;
  g_autofree FixtureState *service_state = g_new0 (FixtureState, 1);

  service = wyrebox_daemon_mailbox_select_service_new (select_mailbox_fixture,
      service_state,
      NULL);
  g_assert_nonnull (service);

  adapter = wyrebox_daemon_request_adapter_new (NULL,
          NULL,
          service,
          NULL,
          NULL,
          wyrebox_daemon_capnp_codec_decode_request_frame,
          NULL,
          NULL,
          wyrebox_daemon_capnp_codec_encode_response_frame,
      NULL,
      NULL);
  g_assert_nonnull (adapter);

  request = build_mailbox_select_request_bytes ("mailbox-inbox",
      "",
      "request-select-route");
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
  g_assert_true (response_frame.which () == ResponseFrame::MAILBOX_SELECT);
  g_assert_cmpstr (response_frame.getMailboxSelect ().getRequestId ().cStr (),
      ==,
      "request-select-route");
  g_assert_cmpstr (response_frame.getMailboxSelect ().getMailboxId ().cStr (),
      ==,
      "mailbox-inbox");
  g_assert_cmpstr (response_frame.getMailboxSelect ().getMailboxName ().cStr (),
      ==,
      "INBOX");
  g_assert_cmpuint (response_frame.getMailboxSelect ().getUidValidity (), ==,
      77);
  g_assert_cmpuint (response_frame.getMailboxSelect ().getUidNext (), ==, 42);
}

static void
assert_request_adapter_routes_fact_mutation (void)
{
  const char *arguments[] = { "mail-1", "project-a", NULL };
  g_autofree char *root =
      g_dir_make_tmp ("wyrebox-capnp-fact-mutation-XXXXXX", NULL);
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) request = NULL;
  g_autoptr (GBytes) response_bytes = NULL;
  g_autoptr (WyreboxJournalWriter) writer = NULL;
  g_autoptr (WyreboxDaemonFactMutationService) service = NULL;
  g_autoptr (WyreboxDaemonRequestAdapter) adapter = NULL;

  g_assert_nonnull (root);

  writer = wyrebox_journal_writer_new (root, &error);
  g_assert_no_error (error);
  g_assert_nonnull (writer);

  service = wyrebox_daemon_fact_mutation_service_new (writer);
  g_assert_nonnull (service);

  adapter = wyrebox_daemon_request_adapter_new (service,
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
  g_assert_nonnull (adapter);

  request = build_fact_mutation_request_bytes (FactMutationKind::INSERT,
      "request-fact-route",
      "skill",
      "account-1",
      "project_mention",
      "account-1",
      arguments);
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

  gsize size = 0;
  const guint8 *data = static_cast<const guint8 *> (
      g_bytes_get_data (response_bytes, &size));
  auto words = kj::arrayPtr (
      reinterpret_cast<const capnp::word *> (data), size / sizeof (capnp::word));
  capnp::FlatArrayMessageReader reader (words);
  auto response_frame = reader.getRoot<ResponseFrame> ();
  g_assert_true (response_frame.which () == ResponseFrame::SUCCESS);
  g_assert_cmpstr (response_frame.getSuccess ().getRequestId ().cStr (), ==,
      "request-fact-route");
  g_assert_cmpstr (response_frame.getSuccess ().getDurableMarker ().cStr (), ==,
      "journal:0:1");
  g_assert_cmpuint (response_frame.getSuccess ().getJournalSequence (), ==, 1);

  remove_tree (root);
}

static void
assert_request_adapter_routes_message_fetch (void)
{
  g_autoptr (GError) error = NULL;
  g_autofree FixtureState *service_state = g_new0 (FixtureState, 1);
  g_autoptr (GBytes) request = NULL;
  g_autoptr (GBytes) response_bytes = NULL;
  g_autoptr (WyreboxDaemonMessageFetchService) service = NULL;
  g_autoptr (WyreboxDaemonRequestAdapter) adapter = NULL;

  service = wyrebox_daemon_message_fetch_service_new (fetch_message_fixture,
      service_state,
      NULL);
  g_assert_nonnull (service);

  adapter =
      wyrebox_daemon_request_adapter_new (NULL,
          NULL,
          NULL,
          service,
          NULL,
          wyrebox_daemon_capnp_codec_decode_request_frame,
          NULL,
          NULL,
          wyrebox_daemon_capnp_codec_encode_response_frame,
          NULL,
          NULL);
  g_assert_nonnull (adapter);

  request = build_message_fetch_request_bytes ("request-fetch-route",
      "account-1",
      "account-1",
      "mailbox-inbox",
      77,
      42);
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

  g_assert_true (response_frame.which () == ResponseFrame::STREAM_CHUNK);
  g_assert_cmpstr (response_frame.getStreamChunk ().getRequestId ().cStr (), ==,
      "request-fetch-route");
  g_assert_cmpstr (response_frame.getStreamChunk ().getMessageId ().cStr (), ==,
      "message-1");
  g_assert_cmpstr (response_frame.getStreamChunk ().getQueryId ().cStr (), ==, "");
  g_assert_cmpstr (response_frame.getStreamChunk ().getCorrelationId ().cStr (),
      ==,
      "corr-fetch");
  g_assert_cmpuint (response_frame.getStreamChunk ().getChunkIndex (), ==, 0);
  g_assert_true (response_frame.getStreamChunk ().getEndOfStream ());
}

static void
assert_request_adapter_routes_flag_keyword_update (void)
{
  const char *system_flags[] = { "\\Seen", "\\Flagged", NULL };
  const char *user_keywords[] = { "project", "todo", NULL };
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) request = NULL;
  g_autoptr (GBytes) response_bytes = NULL;
  g_autoptr (WyreboxDaemonFlagKeywordUpdateService) service = NULL;
  g_autoptr (WyreboxDaemonRequestAdapter) adapter = NULL;
  gboolean was_called = FALSE;
  const WyreboxDaemonPeerCredentials peer_credentials = {
    .uid = 100,
    .gid = 101,
    .pid = 102,
  };

  service = wyrebox_daemon_flag_keyword_update_service_new (
      update_flag_keyword_fixture, &was_called, NULL);
  g_assert_nonnull (service);

  adapter = wyrebox_daemon_request_adapter_new (NULL,
      NULL,
      NULL,
      NULL,
      service,
      wyrebox_daemon_capnp_codec_decode_request_frame,
      NULL,
      NULL,
      wyrebox_daemon_capnp_codec_encode_response_frame,
      NULL,
      NULL);
  g_assert_nonnull (adapter);

  request = build_flag_keyword_update_request_bytes ("request-flag-keyword",
      "account-1",
      "account-1",
      "mailbox-inbox",
      77,
      42,
      FlagKeywordUpdateMode::SET,
      system_flags,
      user_keywords);

  response_bytes = wyrebox_daemon_request_adapter_handle_payload (&peer_credentials,
      request,
      adapter,
      &error);
  g_assert_no_error (error);
  g_assert_nonnull (response_bytes);
  g_assert_true (was_called);

  gsize size = 0;
  const guint8 *data = static_cast<const guint8 *> (
      g_bytes_get_data (response_bytes, &size));
  auto words = kj::arrayPtr (
      reinterpret_cast<const capnp::word *> (data), size / sizeof (capnp::word));
  capnp::FlatArrayMessageReader reader (words);
  auto response_frame = reader.getRoot<ResponseFrame> ();
  g_assert_true (response_frame.which () == ResponseFrame::SUCCESS);
  g_assert_cmpstr (response_frame.getSuccess ().getRequestId ().cStr (), ==,
      "request-flag-keyword");
  g_assert_cmpstr (response_frame.getSuccess ().getDurableMarker ().cStr (), ==,
      "journal:123:456");
  g_assert_cmpuint (response_frame.getSuccess ().getJournalSequence (), ==, 456);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/daemon-api/capnp/codec/decode-mailbox-list",
      assert_request_bytes_decode_mailbox_list);
  g_test_add_func ("/daemon-api/capnp/codec/decode-mailbox-select-id",
      assert_request_bytes_decode_mailbox_select_by_id);
  g_test_add_func ("/daemon-api/capnp/codec/decode-mailbox-select-name",
      assert_request_bytes_decode_mailbox_select_by_name);
  g_test_add_func ("/daemon-api/capnp/codec/reject-mailbox-select-missing-selector",
      assert_mailbox_select_decode_rejects_missing_selector);
  g_test_add_func ("/daemon-api/capnp/codec/reject-mailbox-select-both-selectors",
      assert_mailbox_select_decode_rejects_both_selectors);
  g_test_add_func ("/daemon-api/capnp/codec/decode-fact-mutation-insert",
      assert_request_bytes_decode_fact_mutation_insert);
  g_test_add_func ("/daemon-api/capnp/codec/decode-fact-mutation-retract",
      assert_request_bytes_decode_fact_mutation_retract);
  g_test_add_func ("/daemon-api/capnp/codec/decode-message-fetch-valid",
      assert_request_bytes_decode_message_fetch);
  g_test_add_func ("/daemon-api/capnp/codec/decode-flag-keyword-update-set",
      assert_request_bytes_decode_flag_keyword_update_set);
  g_test_add_func (
      "/daemon-api/capnp/codec/decode-flag-keyword-update-mode-mapping",
      assert_request_bytes_decode_flag_keyword_update_mode_mapping);
  g_test_add_func (
      "/daemon-api/capnp/codec/decode-flag-keyword-update-replace-empty",
      assert_request_bytes_decode_flag_keyword_update_replace_empty_payload);
  g_test_add_func (
      "/daemon-api/capnp/codec/reject-flag-keyword-update-invalid-mode",
      assert_request_bytes_rejects_flag_keyword_update_invalid_mode);
  g_test_add_func (
      "/daemon-api/capnp/codec/reject-flag-keyword-update-missing-payload",
      assert_request_bytes_rejects_flag_keyword_update_missing_payload_for_set);
  g_test_add_func (
      "/daemon-api/capnp/codec/reject-flag-keyword-update-missing-account",
      assert_request_bytes_rejects_flag_keyword_update_missing_account);
  g_test_add_func (
      "/daemon-api/capnp/codec/reject-flag-keyword-update-missing-mailbox",
      assert_request_bytes_rejects_flag_keyword_update_missing_mailbox);
  g_test_add_func (
      "/daemon-api/capnp/codec/reject-flag-keyword-update-zero-uid-validity",
      assert_request_bytes_rejects_flag_keyword_update_zero_uid_validity);
  g_test_add_func (
      "/daemon-api/capnp/codec/reject-flag-keyword-update-zero-mailbox-uid",
      assert_request_bytes_rejects_flag_keyword_update_zero_mailbox_uid);
  g_test_add_func (
      "/daemon-api/capnp/codec/reject-flag-keyword-update-unknown-system-flag",
      assert_request_bytes_rejects_flag_keyword_update_unknown_system_flag);
  g_test_add_func (
      "/daemon-api/capnp/codec/reject-flag-keyword-update-recent-system-flag",
      assert_request_bytes_rejects_flag_keyword_update_recent_system_flag);
  g_test_add_func (
      "/daemon-api/capnp/codec/reject-flag-keyword-update-system-flag-keyword",
      assert_request_bytes_rejects_flag_keyword_update_system_flag_keyword);
  g_test_add_func (
      "/daemon-api/capnp/codec/reject-flag-keyword-update-invalid-keyword",
      assert_request_bytes_rejects_flag_keyword_update_invalid_keyword);
  g_test_add_func (
      "/daemon-api/capnp/codec/reject-flag-keyword-update-duplicate-flag",
      assert_request_bytes_rejects_flag_keyword_update_duplicate_flag);
  g_test_add_func (
      "/daemon-api/capnp/codec/reject-flag-keyword-update-duplicate-keyword",
      assert_request_bytes_rejects_flag_keyword_update_duplicate_keyword);
  g_test_add_func ("/daemon-api/capnp/codec/reject-message-fetch-empty-account",
      assert_request_bytes_rejects_message_fetch_missing_account_identity);
  g_test_add_func ("/daemon-api/capnp/codec/reject-message-fetch-empty-mailbox",
      assert_request_bytes_rejects_message_fetch_missing_mailbox_id);
  g_test_add_func (
      "/daemon-api/capnp/codec/reject-message-fetch-zero-uid-validity",
      assert_request_bytes_rejects_message_fetch_zero_uid_validity);
  g_test_add_func (
      "/daemon-api/capnp/codec/reject-message-fetch-zero-mailbox-uid",
      assert_request_bytes_rejects_message_fetch_zero_mailbox_uid);
  g_test_add_func ("/daemon-api/capnp/codec/reject-fact-mutation-predicate",
      assert_fact_mutation_decode_rejects_missing_predicate);
  g_test_add_func ("/daemon-api/capnp/codec/reject-fact-mutation-scope",
      assert_fact_mutation_decode_rejects_missing_scope);
  g_test_add_func ("/daemon-api/capnp/codec/reject-fact-mutation-argument",
      assert_fact_mutation_decode_rejects_empty_argument);
  g_test_add_func ("/daemon-api/capnp/codec/decode-unsupported-arm",
      assert_request_bytes_unsupported_arm_is_rejected);
  g_test_add_func ("/daemon-api/capnp/codec/encode-mailbox-list",
      assert_response_bytes_encode_mailbox_list);
  g_test_add_func ("/daemon-api/capnp/codec/encode-mailbox-select",
      assert_response_bytes_encode_mailbox_select);
  g_test_add_func ("/daemon-api/capnp/codec/encode-stream-chunk",
      assert_response_bytes_encode_stream_chunk);
  g_test_add_func ("/daemon-api/capnp/codec/reject-ambiguous-stream-chunk",
      assert_response_bytes_reject_ambiguous_stream_chunk);
  g_test_add_func (
      "/daemon-api/capnp/codec/reject-mismatched-stream-chunk-request",
      assert_response_bytes_reject_mismatched_stream_chunk_request);
  g_test_add_func ("/daemon-api/capnp/codec/encode-final-empty-stream-chunk",
      assert_response_bytes_encode_final_empty_stream_chunk);
  g_test_add_func ("/daemon-api/capnp/codec/encode-error",
      assert_response_bytes_encode_error);
  g_test_add_func ("/daemon-api/capnp/codec/encode-success",
      assert_response_bytes_encode_success);
  g_test_add_func ("/daemon-api/capnp/codec/request-adapter-compatible",
      assert_request_adapter_callbacks_are_usable);
  g_test_add_func ("/daemon-api/capnp/codec/request-adapter-mailbox-select",
      assert_request_adapter_routes_mailbox_select);
  g_test_add_func ("/daemon-api/capnp/codec/request-adapter-fact-mutation",
      assert_request_adapter_routes_fact_mutation);
  g_test_add_func ("/daemon-api/capnp/codec/request-adapter-message-fetch",
      assert_request_adapter_routes_message_fetch);
  g_test_add_func ("/daemon-api/capnp/codec/request-adapter-flag-keyword-update",
      assert_request_adapter_routes_flag_keyword_update);

  return g_test_run ();
}
