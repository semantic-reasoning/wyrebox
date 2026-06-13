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
#include "wyrebox-daemon-delivery-ingestion-request.h"
#include "wyrebox-daemon-delivery-ingestion-service.h"
#include "wyrebox-daemon-duckdb-query-template-request.h"
#include "wyrebox-daemon-duckdb-query-template-service.h"
#include "wyrebox-daemon-message-search-request.h"
#include "wyrebox-daemon-message-search-service.h"
#include "wyrebox-daemon-wirelog-predicate-query-request.h"
#include "wyrebox-daemon-wirelog-predicate-query-service.h"
#include "wyrebox-daemon-flag-keyword-update-request.h"
#include "wyrebox-daemon-flag-keyword-update-service.h"
#include "wyrebox-daemon-request-adapter.h"
#include "wyrebox-daemon-audit-payload.h"
#include "wyrebox-journal-reader.h"
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

typedef struct
{
  FactMutationKind mutation;
  const char *predicate_id;
  const char *scope_id;
  const char *const *arguments;
} FactBatchImportEntryFixture;

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
  auto request_frame = request_builder.initRoot < RequestFrame > ();

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
    const char *mailbox_name, const char *request_id)
{
  capnp::MallocMessageBuilder request_builder;
  auto request_frame = request_builder.initRoot < RequestFrame > ();

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
    const char *scope_id, const char *const *arguments)
{
  guint argument_count = 0;
  capnp::MallocMessageBuilder request_builder;
  auto request_frame = request_builder.initRoot < RequestFrame > ();

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

static void
init_fact_batch_import_entry (FactMutationRequest::Builder fact_mutation,
    const FactBatchImportEntryFixture *entry)
{
  guint argument_count = 0;

  fact_mutation.setMutation (entry->mutation);
  fact_mutation.setPredicateId (entry->predicate_id);
  fact_mutation.setScopeId (entry->scope_id);

  while (entry->arguments != NULL && entry->arguments[argument_count] != NULL)
    argument_count++;

  auto encoded_arguments = fact_mutation.initArguments (argument_count);
  for (guint i = 0; i < argument_count; i++)
    encoded_arguments.set (i, entry->arguments[i]);
}

static GBytes *
build_fact_batch_import_request_bytes (const char *request_id,
    const char *caller_identity,
    const char *account_identity,
    const FactBatchImportEntryFixture *entries, guint n_entries)
{
  capnp::MallocMessageBuilder request_builder;
  auto request_frame = request_builder.initRoot < RequestFrame > ();

  auto identity = request_frame.initIdentity ();
  identity.setRequestId (request_id);
  identity.setCallerIdentity (caller_identity);
  identity.setAccountIdentity (account_identity);
  identity.setToolIdentity ("fact-importer");
  identity.setCorrelationId ("corr-fact-batch");

  auto fact_batch_import = request_frame.initFactBatchImport ();
  auto encoded_entries = fact_batch_import.initEntries (n_entries);
  for (guint i = 0; i < n_entries; i++)
    init_fact_batch_import_entry (encoded_entries[i], &entries[i]);

  auto words = capnp::messageToFlatArray (request_builder);
  auto bytes = words.asBytes ();

  return g_bytes_new (bytes.begin (), bytes.size ());
}

static GBytes *
build_message_fetch_request_bytes (const char *request_id,
    const char *identity_account,
    const char *message_fetch_account,
    const char *mailbox_id, guint64 uid_validity, guint64 mailbox_uid)
{
  capnp::MallocMessageBuilder request_builder;
  auto request_frame = request_builder.initRoot < RequestFrame > ();

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
build_message_search_request_bytes (const char *request_id,
    const char *identity_account,
    const char *message_search_account,
    const char *mailbox_id, guint64 uid_validity, const char *criteria_token)
{
  capnp::MallocMessageBuilder request_builder;
  auto request_frame = request_builder.initRoot < RequestFrame > ();

  auto identity = request_frame.initIdentity ();
  identity.setRequestId (request_id);
  identity.setCallerIdentity ("dovecot");
  identity.setAccountIdentity (identity_account);
  identity.setToolIdentity ("dovecot-storage");
  identity.setCorrelationId ("corr-search");

  auto message_search_request = request_frame.initMessageSearch ();
  message_search_request.setAccountIdentity (message_search_account);
  message_search_request.setMailboxId (mailbox_id);
  message_search_request.setUidValidity (uid_validity);
  message_search_request.setCriteriaToken (criteria_token);

  auto words = capnp::messageToFlatArray (request_builder);
  auto bytes = words.asBytes ();

  return g_bytes_new (bytes.begin (), bytes.size ());
}

static GBytes *
build_wirelog_predicate_query_request_bytes (const char *request_id,
    const char *caller_identity,
    const char *identity_account,
    const char *query_id,
    const char *predicate_id, const char *scope_id, const char *const *bindings)
{
  gsize binding_count = 0;
  capnp::MallocMessageBuilder request_builder;
  auto request_frame = request_builder.initRoot < RequestFrame > ();

  auto identity = request_frame.initIdentity ();
  identity.setRequestId (request_id);
  identity.setCallerIdentity (caller_identity);
  identity.setAccountIdentity (identity_account);
  identity.setToolIdentity ("dovecot-storage");
  identity.setCorrelationId ("corr-wirelog");

  auto wirelog_predicate_query = request_frame.initWirelogPredicateQuery ();
  wirelog_predicate_query.setQueryId (query_id);
  wirelog_predicate_query.setPredicateId (predicate_id);
  wirelog_predicate_query.setScopeId (scope_id);

  while (bindings != NULL && bindings[binding_count] != NULL)
    binding_count++;

  auto encoded_bindings = wirelog_predicate_query.initBindings (binding_count);
  for (gsize i = 0; i < binding_count; i++)
    encoded_bindings.set (i, bindings[i]);

  auto words = capnp::messageToFlatArray (request_builder);
  auto bytes = words.asBytes ();

  return g_bytes_new (bytes.begin (), bytes.size ());
}

static GBytes *
build_duckdb_query_template_request_bytes (const char *request_id,
    const char *caller_identity,
    const char *identity_account,
    const char *query_id,
    const char *template_id,
    const char *scope_id, const char *const *parameters)
{
  gsize parameter_count = 0;
  capnp::MallocMessageBuilder request_builder;
  auto request_frame = request_builder.initRoot < RequestFrame > ();

  auto identity = request_frame.initIdentity ();
  identity.setRequestId (request_id);
  identity.setCallerIdentity (caller_identity);
  identity.setAccountIdentity (identity_account);
  identity.setToolIdentity ("duckdb-tool");
  identity.setCorrelationId ("corr-duckdb");

  auto duckdb_query_template = request_frame.initDuckDBQueryTemplate ();
  duckdb_query_template.setQueryId (query_id);
  duckdb_query_template.setTemplateId (template_id);
  duckdb_query_template.setScopeId (scope_id);

  while (parameters != NULL && parameters[parameter_count] != NULL)
    parameter_count++;

  auto encoded_parameters =
      duckdb_query_template.initParameters (parameter_count);
  for (gsize i = 0; i < parameter_count; i++)
    encoded_parameters.set (i, parameters[i]);

  auto words = capnp::messageToFlatArray (request_builder);
  auto bytes = words.asBytes ();

  return g_bytes_new (bytes.begin (), bytes.size ());
}

static GBytes *
build_flag_keyword_update_request_bytes (const char *request_id,
    const char *request_account,
    const char *flag_account,
    const char *mailbox_id,
    guint64 uid_validity,
    guint64 mailbox_uid,
    FlagKeywordUpdateMode mode,
    const char *const *system_flags, const char *const *user_keywords)
{
  gsize system_flag_count = 0;
  gsize user_keyword_count = 0;
  capnp::MallocMessageBuilder request_builder;
  auto request_frame = request_builder.initRoot < RequestFrame > ();

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
    auto encoded_system_flags =
        flag_keyword_update_request.initSystemFlags (system_flag_count);
    for (guint i = 0; i < system_flag_count; i++)
      encoded_system_flags.set (i, system_flags[i]);
  }

  if (user_keywords != NULL) {
    while (user_keywords[user_keyword_count] != NULL)
      user_keyword_count++;
    auto encoded_user_keywords =
        flag_keyword_update_request.initUserKeywords (user_keyword_count);
    for (guint i = 0; i < user_keyword_count; i++)
      encoded_user_keywords.set (i, user_keywords[i]);
  }

  auto words = capnp::messageToFlatArray (request_builder);
  auto bytes = words.asBytes ();

  return g_bytes_new (bytes.begin (), bytes.size ());
}

static GBytes *
build_delivery_ingestion_request_bytes (const char *request_id,
    const char *identity_account,
    const char *caller_identity,
    const char *delivery_id,
    const char *queue_id,
    const char *envelope_sender,
    const char *const *recipients,
    const guint8 *message_bytes, gsize message_size, const char *correlation_id)
{
  gsize recipient_count = 0;
  capnp::MallocMessageBuilder request_builder;
  auto request_frame = request_builder.initRoot < RequestFrame > ();
  auto delivery_ingestion_request = request_frame.initDeliveryIngestion ();

  auto identity = request_frame.initIdentity ();
  identity.setRequestId (request_id);
  identity.setCallerIdentity (caller_identity);
  identity.setAccountIdentity (identity_account);
  identity.setToolIdentity ("tool");
  identity.setCorrelationId (correlation_id);

  if (delivery_id != NULL)
    delivery_ingestion_request.setDeliveryId (delivery_id);
  delivery_ingestion_request.setQueueId (queue_id != NULL ? queue_id : "");
  delivery_ingestion_request.setEnvelopeSender (envelope_sender !=
      NULL ? envelope_sender : "");

  while (recipients != NULL && recipients[recipient_count] != NULL)
    recipient_count++;

  if (recipients != NULL) {
    auto encoded_recipients =
        delivery_ingestion_request.initRecipients (recipient_count);
    for (guint i = 0; i < recipient_count; i++)
      encoded_recipients.set (i, recipients[i]);
  }

  if (message_bytes != NULL || message_size > 0) {
    delivery_ingestion_request.setMessageBytes (kj::arrayPtr (reinterpret_cast <
            const capnp::byte * >(message_bytes), message_size));
  }

  auto words = capnp::messageToFlatArray (request_builder);
  auto bytes = words.asBytes ();

  return g_bytes_new (bytes.begin (), bytes.size ());
}

static gboolean
select_mailbox_fixture (const WyreboxDaemonRequestIdentity *identity,
    const WyreboxDaemonMailboxSelectRequest *request,
    WyreboxDaemonMailboxSelectResult *out_result,
    gpointer user_data, GError **error)
{
  FixtureState *state = static_cast < FixtureState * >(user_data);

  g_assert_nonnull (identity);
  g_assert_nonnull (request);

  g_assert_cmpstr (identity->request_id, ==, "request-select-route");
  g_assert_cmpstr (request->account_identity, ==, "account-1");
  g_assert_cmpstr (request->mailbox_id, ==, "mailbox-inbox");
  g_assert_null (request->mailbox_name);

  state->was_called = TRUE;

  return wyrebox_daemon_mailbox_select_result_init (out_result,
      WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_ORDINARY,
      "mailbox-inbox", "INBOX", 77, 42, 123, error);
}

static gboolean
append_mailbox_fixture (const WyreboxDaemonRequestIdentity *identity,
    const WyreboxDaemonMailboxListRequest *request,
    WyreboxDaemonMailboxListResult *out_result,
    gpointer user_data, GError **error)
{
  FixtureState *state = static_cast < FixtureState * >(user_data);

  g_assert_nonnull (identity);
  g_assert_nonnull (request);

  g_assert_cmpstr (identity->request_id, ==, "request-1");
  g_assert_cmpstr (request->account_identity, ==, "account-1");

  state->was_called = TRUE;

  wyrebox_daemon_mailbox_list_result_init_empty (out_result);
  g_assert_true (wyrebox_daemon_mailbox_list_result_append_entry (out_result,
          WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_ORDINARY,
          "mailbox-inbox",
          "INBOX",
          "/",
          NULL, TRUE, WYREBOX_DAEMON_MAILBOX_LIST_CHILD_STATE_UNKNOWN, error));

  return TRUE;
}

static gboolean
fetch_message_fixture (const WyreboxDaemonRequestIdentity *identity,
    const WyreboxDaemonMessageFetchRequest *request,
    WyreboxDaemonStreamChunkFrame *out_chunk,
    gpointer user_data, GError **error)
{
  const char *payload = "message-1-body";
  g_autoptr (GBytes) bytes = NULL;
  FixtureState *state = static_cast < FixtureState * >(user_data);

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
      "message-1", NULL, identity->correlation_id, 0, bytes, TRUE, error);
}

static gboolean
search_message_fixture (const WyreboxDaemonRequestIdentity *identity,
    const WyreboxDaemonMessageSearchRequest *request,
    WyreboxDaemonStreamChunkFrame *out_chunk,
    gpointer user_data, GError **error)
{
  const char *payload = "search-result-ids";
  g_autoptr (GBytes) bytes = NULL;
  FixtureState *state = static_cast < FixtureState * >(user_data);

  g_assert_cmpstr (identity->request_id, ==, "request-search-route");
  g_assert_cmpstr (identity->caller_identity, ==, "dovecot");
  g_assert_cmpstr (identity->account_identity, ==, "account-1");
  g_assert_cmpstr (identity->tool_identity, ==, "dovecot-storage");
  g_assert_cmpstr (identity->correlation_id, ==, "corr-search");
  g_assert_cmpstr (request->account_identity, ==, "account-1");
  g_assert_cmpstr (request->mailbox_id, ==, "mailbox-inbox");
  g_assert_cmpuint (request->uid_validity, ==, 77);
  g_assert_cmpstr (request->criteria_token, ==, "unseen");

  state->was_called = TRUE;
  bytes = g_bytes_new_static (payload, strlen (payload));

  return wyrebox_daemon_stream_chunk_frame_init (out_chunk,
      identity->request_id,
      NULL, "query-search", identity->correlation_id, 0, bytes, TRUE, error);
}

static gboolean
query_wirelog_predicate_fixture (const WyreboxDaemonRequestIdentity *identity,
    const WyreboxDaemonWirelogPredicateQueryRequest *request,
    WyreboxDaemonStreamChunkFrame *out_chunk,
    gpointer user_data, GError **error)
{
  const char *payload = "wirelog-result-rows";
  g_autoptr (GBytes) bytes = NULL;
  FixtureState *state = static_cast < FixtureState * >(user_data);

  g_assert_cmpstr (identity->request_id, ==, "request-wirelog-route");
  g_assert_cmpstr (identity->caller_identity, ==, "trusted-tool");
  g_assert_cmpstr (identity->account_identity, ==, "account-1");
  g_assert_cmpstr (identity->tool_identity, ==, "dovecot-storage");
  g_assert_cmpstr (identity->correlation_id, ==, "corr-wirelog");
  g_assert_cmpstr (request->query_id, ==, "query-wirelog");
  g_assert_cmpstr (request->predicate_id, ==, "show_in_virtual_folder.v1");
  g_assert_cmpstr (request->scope_id, ==, "account-1");
  g_assert_null (request->bindings[0]);

  state->was_called = TRUE;
  bytes = g_bytes_new_static (payload, strlen (payload));

  return wyrebox_daemon_stream_chunk_frame_init (out_chunk,
      identity->request_id,
      NULL, request->query_id, identity->correlation_id, 0, bytes, TRUE, error);
}

static gboolean
query_duckdb_template_fixture (const WyreboxDaemonRequestIdentity *identity,
    const WyreboxDaemonDuckDBQueryTemplateRequest *request,
    WyreboxDaemonStreamChunkFrame *out_chunk,
    gpointer user_data, GError **error)
{
  const char *payload = "duckdb-result-rows";
  g_autoptr (GBytes) bytes = NULL;
  FixtureState *state = static_cast < FixtureState * >(user_data);

  g_assert_cmpstr (identity->request_id, ==, "request-duckdb-route");
  g_assert_cmpstr (identity->caller_identity, ==, "admin-cli");
  g_assert_cmpstr (identity->account_identity, ==, "account-1");
  g_assert_cmpstr (identity->tool_identity, ==, "duckdb-tool");
  g_assert_cmpstr (identity->correlation_id, ==, "corr-duckdb");
  g_assert_cmpstr (request->query_id, ==, "query-duckdb");
  g_assert_cmpstr (request->template_id, ==, "mailbox.uid_map.v1");
  g_assert_cmpstr (request->scope_id, ==, "account-1");
  g_assert_cmpstr (request->parameters[0], ==, "mailbox-inbox");
  g_assert_null (request->parameters[1]);

  state->was_called = TRUE;
  bytes = g_bytes_new_static (payload, strlen (payload));

  return wyrebox_daemon_stream_chunk_frame_init (out_chunk,
      identity->request_id,
      NULL, request->query_id, identity->correlation_id, 0, bytes, TRUE, error);
}

static gboolean
ingest_delivery_fixture (const WyreboxDaemonRequestIdentity *identity,
    const WyreboxDaemonDeliveryIngestionRequest *request,
    WyreboxEmlIngestResult *out_result, gpointer user_data, GError **error)
{
  gboolean *was_called = static_cast < gboolean * >(user_data);
  const char *payload = "message-bytes";
  gsize payload_size = 0;
  const guint8 *payload_data = NULL;

  g_assert_cmpstr (identity->request_id, ==, "request-delivery-route");
  g_assert_cmpstr (identity->caller_identity, ==, "postfix");
  g_assert_cmpstr (identity->account_identity, ==, "account-1");
  g_assert_cmpstr (identity->tool_identity, ==, "tool");
  g_assert_cmpstr (identity->correlation_id, ==, "corr-delivery");
  g_assert_cmpstr (request->delivery_id, ==, "delivery-123");
  g_assert_cmpstr (request->recipients[0], ==, "alice@example.com");
  payload_data =
      static_cast < const guint8 *>(g_bytes_get_data (request->message_bytes,
          &payload_size));
  g_assert_cmpuint (payload_size, ==, strlen (payload));
  g_assert_cmpmem (payload_data, payload_size, payload, strlen (payload));

  *was_called = TRUE;

  out_result->object_key =
      g_strdup
      ("sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
  out_result->size_bytes = 12;
  out_result->journal_offset = 4096;
  out_result->journal_sequence = 7;

  return TRUE;
}

static gboolean
update_flag_keyword_fixture (const WyreboxDaemonRequestIdentity *identity,
    const WyreboxDaemonFlagKeywordUpdateRequest *request,
    WyreboxDaemonSuccessReceipt *out_receipt, gpointer user_data,
    GError **error)
{
  gboolean *was_called = static_cast < gboolean * >(user_data);

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
  g_autoptr (GBytes) request =
      build_mailbox_select_request_bytes ("mailbox-inbox", "",
      "request-select-id");
  g_autoptr (GError) error = NULL;
  WyreboxDaemonDecodedRequestFrame decoded = { 0 };
  gpointer decoded_state = NULL;
  GDestroyNotify decoded_state_clear = NULL;

  g_assert_true (wyrebox_daemon_capnp_codec_decode_request_frame (NULL,
          request,
          &decoded, &decoded_state, &decoded_state_clear, NULL, &error));
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
  g_autoptr (GBytes) request = build_mailbox_select_request_bytes ("",
      "Projects/Project A", "request-select-name");
  g_autoptr (GError) error = NULL;
  WyreboxDaemonDecodedRequestFrame decoded = { 0 };
  gpointer decoded_state = NULL;
  GDestroyNotify decoded_state_clear = NULL;

  g_assert_true (wyrebox_daemon_capnp_codec_decode_request_frame (NULL,
          request,
          &decoded, &decoded_state, &decoded_state_clear, NULL, &error));
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
      "", "request-select-invalid");
  g_autoptr (GError) error = NULL;
  WyreboxDaemonDecodedRequestFrame decoded = { 0 };
  gpointer decoded_state = NULL;
  GDestroyNotify decoded_state_clear = NULL;

  g_assert_false (wyrebox_daemon_capnp_codec_decode_request_frame (NULL,
          request,
          &decoded, &decoded_state, &decoded_state_clear, NULL, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_null (decoded_state);
  g_assert_null (decoded_state_clear);
}

static void
assert_mailbox_select_decode_rejects_both_selectors (void)
{
  g_autoptr (GBytes) request =
      build_mailbox_select_request_bytes ("mailbox-inbox", "INBOX",
      "request-select-invalid");
  g_autoptr (GError) error = NULL;
  WyreboxDaemonDecodedRequestFrame decoded = { 0 };
  gpointer decoded_state = NULL;
  GDestroyNotify decoded_state_clear = NULL;

  g_assert_false (wyrebox_daemon_capnp_codec_decode_request_frame (NULL,
          request,
          &decoded, &decoded_state, &decoded_state_clear, NULL, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_null (decoded_state);
  g_assert_null (decoded_state_clear);
}

static void
assert_mailbox_list_request_encoder_round_trip (void)
{
  g_auto (WyreboxDaemonRequestIdentity) identity = { 0 };
  g_auto (WyreboxDaemonMailboxListRequest) request = { 0 };
  g_autoptr (GBytes) encoded = NULL;
  g_autoptr (GError) error = NULL;
  WyreboxDaemonDecodedRequestFrame decoded = { 0 };
  gpointer decoded_state = NULL;
  GDestroyNotify decoded_state_clear = NULL;

  g_assert_true (wyrebox_daemon_request_identity_init (&identity,
          "request-encode-mailbox-list",
          "dovecot",
          "account-1", "dovecot-storage", "corr-list-encode", &error));
  g_assert_no_error (error);
  g_assert_true (wyrebox_daemon_mailbox_list_request_init (&request,
          "account-1", "Projects/", &error));
  g_assert_no_error (error);

  encoded = wyrebox_daemon_capnp_codec_encode_mailbox_list_request (&identity,
      &request, NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (encoded);

  g_assert_true (wyrebox_daemon_capnp_codec_decode_request_frame (NULL,
          encoded,
          &decoded, &decoded_state, &decoded_state_clear, NULL, &error));
  g_assert_no_error (error);
  g_assert_cmpstr (decoded.request_id, ==, "request-encode-mailbox-list");
  g_assert_cmpstr (decoded.caller_identity, ==, "dovecot");
  g_assert_cmpstr (decoded.account_identity, ==, "account-1");
  g_assert_cmpstr (decoded.tool_identity, ==, "dovecot-storage");
  g_assert_cmpstr (decoded.correlation_id, ==, "corr-list-encode");
  g_assert_cmpint (decoded.operation,
      ==, WYREBOX_DAEMON_REQUEST_FRAME_OPERATION_MAILBOX_LIST);
  g_assert_nonnull (decoded.mailbox_list);
  g_assert_cmpstr (decoded.mailbox_list->account_identity, ==, "account-1");
  g_assert_cmpstr (decoded.mailbox_list->namespace_prefix, ==, "Projects/");

  g_assert_nonnull (decoded_state_clear);
  g_assert_nonnull (decoded_state);
  decoded_state_clear (decoded_state);
}

static void
assert_mailbox_list_request_encoder_rejects_invalid_input (void)
{
  g_auto (WyreboxDaemonRequestIdentity) identity = { 0 };
  g_auto (WyreboxDaemonRequestIdentity) invalid_identity = { 0 };
  g_auto (WyreboxDaemonMailboxListRequest) request = { 0 };
  g_auto (WyreboxDaemonMailboxListRequest) invalid_request = { 0 };
  g_autoptr (GBytes) encoded = NULL;
  g_autoptr (GError) error = NULL;

  g_assert_true (wyrebox_daemon_request_identity_init (&identity,
          "request-encode-mailbox-list-valid",
          "dovecot", "account-1", "dovecot-storage", NULL, &error));
  g_assert_no_error (error);
  g_assert_true (wyrebox_daemon_mailbox_list_request_init (&request,
          "account-1", "", &error));
  g_assert_no_error (error);

  encoded = wyrebox_daemon_capnp_codec_encode_mailbox_list_request (NULL,
      &request, NULL, &error);
  g_assert_null (encoded);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_clear_error (&error);

  encoded = wyrebox_daemon_capnp_codec_encode_mailbox_list_request (&identity,
      NULL, NULL, &error);
  g_assert_null (encoded);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_clear_error (&error);

  encoded =
      wyrebox_daemon_capnp_codec_encode_mailbox_list_request
      (&invalid_identity, &request, NULL, &error);
  g_assert_null (encoded);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_clear_error (&error);

  invalid_request.account_identity = g_strdup ("");
  invalid_request.namespace_prefix = g_strdup ("INBOX");
  encoded = wyrebox_daemon_capnp_codec_encode_mailbox_list_request (&identity,
      &invalid_request, NULL, &error);
  g_assert_null (encoded);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
}

static void
assert_mailbox_select_request_encoder_round_trip_by_id (void)
{
  g_auto (WyreboxDaemonRequestIdentity) identity = { 0 };
  g_auto (WyreboxDaemonMailboxSelectRequest) request = { 0 };
  g_autoptr (GBytes) encoded = NULL;
  g_autoptr (GError) error = NULL;
  WyreboxDaemonDecodedRequestFrame decoded = { 0 };
  gpointer decoded_state = NULL;
  GDestroyNotify decoded_state_clear = NULL;

  g_assert_true (wyrebox_daemon_request_identity_init (&identity,
          "request-encode-mailbox-select-id",
          "dovecot",
          "account-1", "dovecot-storage", "corr-select-encode", &error));
  g_assert_no_error (error);
  g_assert_true (wyrebox_daemon_mailbox_select_request_init (&request,
          "account-1", "mailbox-inbox", NULL, &error));
  g_assert_no_error (error);

  encoded = wyrebox_daemon_capnp_codec_encode_mailbox_select_request (&identity,
      &request, NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (encoded);

  g_assert_true (wyrebox_daemon_capnp_codec_decode_request_frame (NULL,
          encoded,
          &decoded, &decoded_state, &decoded_state_clear, NULL, &error));
  g_assert_no_error (error);
  g_assert_cmpstr (decoded.request_id, ==, "request-encode-mailbox-select-id");
  g_assert_cmpstr (decoded.caller_identity, ==, "dovecot");
  g_assert_cmpstr (decoded.account_identity, ==, "account-1");
  g_assert_cmpstr (decoded.tool_identity, ==, "dovecot-storage");
  g_assert_cmpstr (decoded.correlation_id, ==, "corr-select-encode");
  g_assert_cmpint (decoded.operation,
      ==, WYREBOX_DAEMON_REQUEST_FRAME_OPERATION_MAILBOX_SELECT);
  g_assert_nonnull (decoded.mailbox_select);
  g_assert_cmpstr (decoded.mailbox_select->account_identity, ==, "account-1");
  g_assert_cmpstr (decoded.mailbox_select->mailbox_id, ==, "mailbox-inbox");
  g_assert_null (decoded.mailbox_select->mailbox_name);

  g_assert_nonnull (decoded_state_clear);
  g_assert_nonnull (decoded_state);
  decoded_state_clear (decoded_state);
}

static void
assert_mailbox_select_request_encoder_round_trip_by_name (void)
{
  g_auto (WyreboxDaemonRequestIdentity) identity = { 0 };
  g_auto (WyreboxDaemonMailboxSelectRequest) request = { 0 };
  g_autoptr (GBytes) encoded = NULL;
  g_autoptr (GError) error = NULL;
  WyreboxDaemonDecodedRequestFrame decoded = { 0 };
  gpointer decoded_state = NULL;
  GDestroyNotify decoded_state_clear = NULL;

  g_assert_true (wyrebox_daemon_request_identity_init (&identity,
          "request-encode-mailbox-select-name",
          "dovecot", "account-1", "dovecot-storage", NULL, &error));
  g_assert_no_error (error);
  g_assert_true (wyrebox_daemon_mailbox_select_request_init (&request,
          "account-1", NULL, "Projects/Project A", &error));
  g_assert_no_error (error);

  encoded = wyrebox_daemon_capnp_codec_encode_mailbox_select_request (&identity,
      &request, NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (encoded);

  g_assert_true (wyrebox_daemon_capnp_codec_decode_request_frame (NULL,
          encoded,
          &decoded, &decoded_state, &decoded_state_clear, NULL, &error));
  g_assert_no_error (error);
  g_assert_cmpstr (decoded.request_id, ==,
      "request-encode-mailbox-select-name");
  g_assert_cmpstr (decoded.correlation_id, ==, "");
  g_assert_cmpint (decoded.operation,
      ==, WYREBOX_DAEMON_REQUEST_FRAME_OPERATION_MAILBOX_SELECT);
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
assert_mailbox_select_request_encoder_rejects_invalid_input (void)
{
  g_auto (WyreboxDaemonRequestIdentity) identity = { 0 };
  g_auto (WyreboxDaemonRequestIdentity) invalid_identity = { 0 };
  g_auto (WyreboxDaemonMailboxSelectRequest) request = { 0 };
  g_auto (WyreboxDaemonMailboxSelectRequest) invalid_request = { 0 };
  g_autoptr (GBytes) encoded = NULL;
  g_autoptr (GError) error = NULL;

  g_assert_true (wyrebox_daemon_request_identity_init (&identity,
          "request-encode-mailbox-select-valid",
          "dovecot", "account-1", "dovecot-storage", NULL, &error));
  g_assert_no_error (error);
  g_assert_true (wyrebox_daemon_mailbox_select_request_init (&request,
          "account-1", "mailbox-inbox", NULL, &error));
  g_assert_no_error (error);

  encoded = wyrebox_daemon_capnp_codec_encode_mailbox_select_request (NULL,
      &request, NULL, &error);
  g_assert_null (encoded);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_clear_error (&error);

  encoded = wyrebox_daemon_capnp_codec_encode_mailbox_select_request (&identity,
      NULL, NULL, &error);
  g_assert_null (encoded);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_clear_error (&error);

  encoded =
      wyrebox_daemon_capnp_codec_encode_mailbox_select_request
      (&invalid_identity, &request, NULL, &error);
  g_assert_null (encoded);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_clear_error (&error);

  invalid_request.account_identity = g_strdup ("account-1");
  invalid_request.mailbox_id = NULL;
  invalid_request.mailbox_name = NULL;

  encoded = wyrebox_daemon_capnp_codec_encode_mailbox_select_request (&identity,
      &invalid_request, NULL, &error);
  g_assert_null (encoded);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_clear_error (&error);

  g_clear_pointer (&invalid_request.mailbox_id, g_free);
  g_clear_pointer (&invalid_request.mailbox_name, g_free);
  invalid_request.mailbox_id = g_strdup ("mailbox-inbox");
  invalid_request.mailbox_name = g_strdup ("INBOX");

  encoded = wyrebox_daemon_capnp_codec_encode_mailbox_select_request (&identity,
      &invalid_request, NULL, &error);
  g_assert_null (encoded);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
}

static void
assert_message_fetch_request_encoder_round_trip (void)
{
  g_auto (WyreboxDaemonRequestIdentity) identity = { 0 };
  g_auto (WyreboxDaemonMessageFetchRequest) request = { 0 };
  g_autoptr (GBytes) encoded = NULL;
  g_autoptr (GError) error = NULL;
  WyreboxDaemonDecodedRequestFrame decoded = { 0 };
  gpointer decoded_state = NULL;
  GDestroyNotify decoded_state_clear = NULL;

  g_assert_true (wyrebox_daemon_request_identity_init (&identity,
          "request-encode-message-fetch",
          "dovecot",
          "account-1", "dovecot-storage", "corr-message-fetch", &error));
  g_assert_no_error (error);
  g_assert_true (wyrebox_daemon_message_fetch_request_init (&request,
          "account-1", "mailbox-inbox", 77, 42, &error));
  g_assert_no_error (error);

  encoded = wyrebox_daemon_capnp_codec_encode_message_fetch_request (&identity,
      &request, NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (encoded);

  g_assert_true (wyrebox_daemon_capnp_codec_decode_request_frame (NULL,
          encoded,
          &decoded, &decoded_state, &decoded_state_clear, NULL, &error));
  g_assert_no_error (error);
  g_assert_cmpstr (decoded.request_id, ==, "request-encode-message-fetch");
  g_assert_cmpstr (decoded.caller_identity, ==, "dovecot");
  g_assert_cmpstr (decoded.account_identity, ==, "account-1");
  g_assert_cmpstr (decoded.tool_identity, ==, "dovecot-storage");
  g_assert_cmpstr (decoded.correlation_id, ==, "corr-message-fetch");
  g_assert_cmpint (decoded.operation,
      ==, WYREBOX_DAEMON_REQUEST_FRAME_OPERATION_MESSAGE_FETCH);
  g_assert_nonnull (decoded.message_fetch);
  g_assert_cmpstr (decoded.message_fetch->account_identity, ==, "account-1");
  g_assert_cmpstr (decoded.message_fetch->mailbox_id, ==, "mailbox-inbox");
  g_assert_cmpuint (decoded.message_fetch->uid_validity, ==, 77);
  g_assert_cmpuint (decoded.message_fetch->mailbox_uid, ==, 42);

  g_assert_nonnull (decoded_state_clear);
  g_assert_nonnull (decoded_state);
  decoded_state_clear (decoded_state);
}

static void
assert_message_fetch_request_encoder_rejects_invalid_input (void)
{
  g_auto (WyreboxDaemonRequestIdentity) identity = { 0 };
  g_auto (WyreboxDaemonRequestIdentity) invalid_identity = { 0 };
  g_auto (WyreboxDaemonMessageFetchRequest) request = { 0 };
  g_auto (WyreboxDaemonMessageFetchRequest) invalid_request = { 0 };
  g_autoptr (GBytes) encoded = NULL;
  g_autoptr (GError) error = NULL;

  g_assert_true (wyrebox_daemon_request_identity_init (&identity,
          "request-encode-message-fetch-valid",
          "dovecot", "account-1", "dovecot-storage", NULL, &error));
  g_assert_no_error (error);
  g_assert_true (wyrebox_daemon_message_fetch_request_init (&request,
          "account-1", "mailbox-inbox", 77, 42, &error));
  g_assert_no_error (error);

  encoded = wyrebox_daemon_capnp_codec_encode_message_fetch_request (NULL,
      &request, NULL, &error);
  g_assert_null (encoded);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_clear_error (&error);

  encoded = wyrebox_daemon_capnp_codec_encode_message_fetch_request (&identity,
      NULL, NULL, &error);
  g_assert_null (encoded);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_clear_error (&error);

  encoded = wyrebox_daemon_capnp_codec_encode_message_fetch_request
      (&invalid_identity, &request, NULL, &error);
  g_assert_null (encoded);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_clear_error (&error);

  invalid_request.account_identity = g_strdup ("");
  invalid_request.mailbox_id = g_strdup ("mailbox-inbox");
  invalid_request.uid_validity = 77;
  invalid_request.mailbox_uid = 42;
  encoded = wyrebox_daemon_capnp_codec_encode_message_fetch_request (&identity,
      &invalid_request, NULL, &error);
  g_assert_null (encoded);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
}

static void
assert_request_bytes_decode_mailbox_list (void)
{
  g_autoptr (GBytes) request =
      build_mailbox_list_request_bytes ("INBOX", "request-1");
  g_autoptr (GError) error = NULL;
  WyreboxDaemonDecodedRequestFrame decoded = { 0 };
  gpointer decoded_state = NULL;
  GDestroyNotify decoded_state_clear = NULL;

  g_assert_true (wyrebox_daemon_capnp_codec_decode_request_frame (NULL,
          request,
          &decoded, &decoded_state, &decoded_state_clear, NULL, &error));
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
  g_autoptr (GBytes) request =
      build_message_fetch_request_bytes ("request-fetch", "account-1",
      "account-1", "mailbox-inbox", 77, 42);
  g_autoptr (GError) error = NULL;
  WyreboxDaemonDecodedRequestFrame decoded = { 0 };
  gpointer decoded_state = NULL;
  GDestroyNotify decoded_state_clear = NULL;

  g_assert_true (wyrebox_daemon_capnp_codec_decode_request_frame (NULL,
          request,
          &decoded, &decoded_state, &decoded_state_clear, NULL, &error));
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
assert_request_bytes_decode_message_search (void)
{
  g_autoptr (GBytes) request =
      build_message_search_request_bytes ("request-search", "account-1",
      "account-1", "mailbox-inbox", 77, "unseen");
  g_autoptr (GError) error = NULL;
  WyreboxDaemonDecodedRequestFrame decoded = { 0 };
  gpointer decoded_state = NULL;
  GDestroyNotify decoded_state_clear = NULL;

  g_assert_true (wyrebox_daemon_capnp_codec_decode_request_frame (NULL,
          request,
          &decoded, &decoded_state, &decoded_state_clear, NULL, &error));
  g_assert_no_error (error);
  g_assert_cmpstr (decoded.request_id, ==, "request-search");
  g_assert_cmpstr (decoded.caller_identity, ==, "dovecot");
  g_assert_cmpstr (decoded.account_identity, ==, "account-1");
  g_assert_cmpstr (decoded.tool_identity, ==, "dovecot-storage");
  g_assert_cmpstr (decoded.correlation_id, ==, "corr-search");
  g_assert_cmpint (decoded.operation, ==,
      WYREBOX_DAEMON_REQUEST_FRAME_OPERATION_MESSAGE_SEARCH);
  g_assert_nonnull (decoded.message_search);
  g_assert_cmpstr (decoded.message_search->account_identity, ==, "account-1");
  g_assert_cmpstr (decoded.message_search->mailbox_id, ==, "mailbox-inbox");
  g_assert_cmpuint (decoded.message_search->uid_validity, ==, 77);
  g_assert_cmpstr (decoded.message_search->criteria_token, ==, "unseen");
  g_assert_null (decoded.mailbox_list);
  g_assert_null (decoded.mailbox_select);
  g_assert_null (decoded.fact_mutation);
  g_assert_null (decoded.message_fetch);
  g_assert_null (decoded.flag_keyword_update);

  g_assert_nonnull (decoded_state_clear);
  g_assert_nonnull (decoded_state);
  decoded_state_clear (decoded_state);
}

static void
assert_request_bytes_rejects_message_search_missing_account (void)
{
  g_autoptr (GBytes) request =
      build_message_search_request_bytes ("request-search-missing-account",
      "account-1", "", "mailbox-inbox", 77, "unseen");
  g_autoptr (GError) error = NULL;
  WyreboxDaemonDecodedRequestFrame decoded = { 0 };
  gpointer decoded_state = NULL;
  GDestroyNotify decoded_state_clear = NULL;

  g_assert_false (wyrebox_daemon_capnp_codec_decode_request_frame (NULL,
          request,
          &decoded, &decoded_state, &decoded_state_clear, NULL, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_null (decoded_state);
  g_assert_null (decoded_state_clear);
}

static void
assert_request_bytes_rejects_message_search_missing_mailbox (void)
{
  g_autoptr (GBytes) request =
      build_message_search_request_bytes ("request-search-missing-mailbox",
      "account-1", "account-1", "", 77, "unseen");
  g_autoptr (GError) error = NULL;
  WyreboxDaemonDecodedRequestFrame decoded = { 0 };
  gpointer decoded_state = NULL;
  GDestroyNotify decoded_state_clear = NULL;

  g_assert_false (wyrebox_daemon_capnp_codec_decode_request_frame (NULL,
          request,
          &decoded, &decoded_state, &decoded_state_clear, NULL, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_null (decoded_state);
  g_assert_null (decoded_state_clear);
}

static void
assert_request_bytes_rejects_message_search_zero_uid_validity (void)
{
  g_autoptr (GBytes) request =
      build_message_search_request_bytes ("request-search-zero-validity",
      "account-1", "account-1", "mailbox-inbox", 0, "unseen");
  g_autoptr (GError) error = NULL;
  WyreboxDaemonDecodedRequestFrame decoded = { 0 };
  gpointer decoded_state = NULL;
  GDestroyNotify decoded_state_clear = NULL;

  g_assert_false (wyrebox_daemon_capnp_codec_decode_request_frame (NULL,
          request,
          &decoded, &decoded_state, &decoded_state_clear, NULL, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_null (decoded_state);
  g_assert_null (decoded_state_clear);
}

static void
assert_request_bytes_rejects_message_search_empty_criteria (void)
{
  g_autoptr (GBytes) request =
      build_message_search_request_bytes ("request-search-empty-criteria",
      "account-1", "account-1", "mailbox-inbox", 77, "");
  g_autoptr (GError) error = NULL;
  WyreboxDaemonDecodedRequestFrame decoded = { 0 };
  gpointer decoded_state = NULL;
  GDestroyNotify decoded_state_clear = NULL;

  g_assert_false (wyrebox_daemon_capnp_codec_decode_request_frame (NULL,
          request,
          &decoded, &decoded_state, &decoded_state_clear, NULL, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_null (decoded_state);
  g_assert_null (decoded_state_clear);
}

static void
assert_request_bytes_rejects_message_search_invalid_criteria_backslash (void)
{
  g_autoptr (GBytes) request =
      build_message_search_request_bytes
      ("request-search-invalid-criteria-backslash", "account-1", "account-1",
      "mailbox-inbox", 77, "flag\\unseen");
  g_autoptr (GError) error = NULL;
  WyreboxDaemonDecodedRequestFrame decoded = { 0 };
  gpointer decoded_state = NULL;
  GDestroyNotify decoded_state_clear = NULL;

  g_assert_false (wyrebox_daemon_capnp_codec_decode_request_frame (NULL,
          request,
          &decoded, &decoded_state, &decoded_state_clear, NULL, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_null (decoded_state);
  g_assert_null (decoded_state_clear);
}

static void
assert_request_bytes_rejects_message_search_invalid_criteria_sql_like (void)
{
  g_autoptr (GBytes) request =
      build_message_search_request_bytes ("request-search-invalid-criteria-sql",
      "account-1", "account-1", "mailbox-inbox", 77, "select*from");
  g_autoptr (GError) error = NULL;
  WyreboxDaemonDecodedRequestFrame decoded = { 0 };
  gpointer decoded_state = NULL;
  GDestroyNotify decoded_state_clear = NULL;

  g_assert_false (wyrebox_daemon_capnp_codec_decode_request_frame (NULL,
          request,
          &decoded, &decoded_state, &decoded_state_clear, NULL, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_null (decoded_state);
  g_assert_null (decoded_state_clear);
}

static void
assert_request_bytes_decode_wirelog_predicate_query_with_bindings (void)
{
  const char *bindings[] = { "?message", "project-a", NULL };
  g_autoptr (GBytes) request =
      build_wirelog_predicate_query_request_bytes ("request-wirelog",
      "dovecot",
      "account-1", "query-wirelog", "has_label", "account-1", bindings);
  g_autoptr (GError) error = NULL;
  WyreboxDaemonDecodedRequestFrame decoded = { 0 };
  gpointer decoded_state = NULL;
  GDestroyNotify decoded_state_clear = NULL;

  g_assert_true (wyrebox_daemon_capnp_codec_decode_request_frame (NULL,
          request,
          &decoded, &decoded_state, &decoded_state_clear, NULL, &error));
  g_assert_no_error (error);
  g_assert_cmpstr (decoded.request_id, ==, "request-wirelog");
  g_assert_cmpstr (decoded.caller_identity, ==, "dovecot");
  g_assert_cmpstr (decoded.account_identity, ==, "account-1");
  g_assert_cmpstr (decoded.tool_identity, ==, "dovecot-storage");
  g_assert_cmpstr (decoded.correlation_id, ==, "corr-wirelog");
  g_assert_cmpint (decoded.operation, ==,
      WYREBOX_DAEMON_REQUEST_FRAME_OPERATION_WIRELOG_PREDICATE_QUERY);
  g_assert_nonnull (decoded.wirelog_predicate_query);
  g_assert_cmpstr (decoded.wirelog_predicate_query->query_id, ==,
      "query-wirelog");
  g_assert_cmpstr (decoded.wirelog_predicate_query->predicate_id, ==,
      "has_label");
  g_assert_cmpstr (decoded.wirelog_predicate_query->scope_id, ==, "account-1");
  g_assert_cmpstr (decoded.wirelog_predicate_query->bindings[0], ==,
      "?message");
  g_assert_cmpstr (decoded.wirelog_predicate_query->bindings[1], ==,
      "project-a");
  g_assert_null (decoded.wirelog_predicate_query->bindings[2]);
  g_assert_null (decoded.mailbox_list);
  g_assert_null (decoded.mailbox_select);
  g_assert_null (decoded.fact_mutation);
  g_assert_null (decoded.message_fetch);
  g_assert_null (decoded.message_search);
  g_assert_null (decoded.flag_keyword_update);

  g_assert_nonnull (decoded_state_clear);
  g_assert_nonnull (decoded_state);
  decoded_state_clear (decoded_state);
}

static void
assert_request_bytes_decode_wirelog_predicate_query_empty_bindings (void)
{
  const char *bindings[] = { NULL };
  g_autoptr (GBytes) request =
      build_wirelog_predicate_query_request_bytes
      ("request-wirelog-empty-bindings", "dovecot", "account-1",
      "query-wirelog-empty", "has_label", "account-1", bindings);
  g_autoptr (GError) error = NULL;
  WyreboxDaemonDecodedRequestFrame decoded = { 0 };
  gpointer decoded_state = NULL;
  GDestroyNotify decoded_state_clear = NULL;

  g_assert_true (wyrebox_daemon_capnp_codec_decode_request_frame (NULL,
          request,
          &decoded, &decoded_state, &decoded_state_clear, NULL, &error));
  g_assert_no_error (error);
  g_assert_cmpint (decoded.operation, ==,
      WYREBOX_DAEMON_REQUEST_FRAME_OPERATION_WIRELOG_PREDICATE_QUERY);
  g_assert_nonnull (decoded.wirelog_predicate_query);
  g_assert_cmpstr (decoded.wirelog_predicate_query->query_id, ==,
      "query-wirelog-empty");
  g_assert_nonnull (decoded.wirelog_predicate_query->bindings);
  g_assert_null (decoded.wirelog_predicate_query->bindings[0]);

  g_assert_nonnull (decoded_state_clear);
  g_assert_nonnull (decoded_state);
  decoded_state_clear (decoded_state);
}

static void
assert_request_bytes_rejects_wirelog_predicate_query_invalid_query_id (void)
{
  const char *bindings[] = { "?message", NULL };
  g_autoptr (GBytes) request =
      build_wirelog_predicate_query_request_bytes
      ("request-wirelog-invalid-query", "dovecot", "account-1", "query wirelog",
      "has_label", "account-1", bindings);
  g_autoptr (GError) error = NULL;
  WyreboxDaemonDecodedRequestFrame decoded = { 0 };
  gpointer decoded_state = NULL;
  GDestroyNotify decoded_state_clear = NULL;

  g_assert_false (wyrebox_daemon_capnp_codec_decode_request_frame (NULL,
          request,
          &decoded, &decoded_state, &decoded_state_clear, NULL, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_null (decoded_state);
  g_assert_null (decoded_state_clear);
}

static void
assert_request_bytes_rejects_wirelog_predicate_query_invalid_predicate_id (void)
{
  const char *bindings[] = { "?message", NULL };
  g_autoptr (GBytes) request =
      build_wirelog_predicate_query_request_bytes
      ("request-wirelog-invalid-predicate", "dovecot", "account-1",
      "query-wirelog", "has label", "account-1", bindings);
  g_autoptr (GError) error = NULL;
  WyreboxDaemonDecodedRequestFrame decoded = { 0 };
  gpointer decoded_state = NULL;
  GDestroyNotify decoded_state_clear = NULL;

  g_assert_false (wyrebox_daemon_capnp_codec_decode_request_frame (NULL,
          request,
          &decoded, &decoded_state, &decoded_state_clear, NULL, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_null (decoded_state);
  g_assert_null (decoded_state_clear);
}

static void
assert_request_bytes_rejects_wirelog_predicate_query_invalid_scope_id (void)
{
  const char *bindings[] = { "?message", NULL };
  g_autoptr (GBytes) request =
      build_wirelog_predicate_query_request_bytes
      ("request-wirelog-invalid-scope", "dovecot", "account-1", "query-wirelog",
      "has_label", "account\n1", bindings);
  g_autoptr (GError) error = NULL;
  WyreboxDaemonDecodedRequestFrame decoded = { 0 };
  gpointer decoded_state = NULL;
  GDestroyNotify decoded_state_clear = NULL;

  g_assert_false (wyrebox_daemon_capnp_codec_decode_request_frame (NULL,
          request,
          &decoded, &decoded_state, &decoded_state_clear, NULL, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_null (decoded_state);
  g_assert_null (decoded_state_clear);
}

static void
assert_request_bytes_rejects_wirelog_predicate_query_invalid_binding (void)
{
  const char *bindings[] = { "?message\n", NULL };
  g_autoptr (GBytes) request =
      build_wirelog_predicate_query_request_bytes
      ("request-wirelog-invalid-binding", "dovecot", "account-1",
      "query-wirelog", "has_label", "account-1", bindings);
  g_autoptr (GError) error = NULL;
  WyreboxDaemonDecodedRequestFrame decoded = { 0 };
  gpointer decoded_state = NULL;
  GDestroyNotify decoded_state_clear = NULL;

  g_assert_false (wyrebox_daemon_capnp_codec_decode_request_frame (NULL,
          request,
          &decoded, &decoded_state, &decoded_state_clear, NULL, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_null (decoded_state);
  g_assert_null (decoded_state_clear);
}

static void
assert_request_bytes_decode_duckdb_query_template_with_parameters (void)
{
  const char *parameters[] = { "mail-1", "project-a", NULL };
  g_autoptr (GBytes) request =
      build_duckdb_query_template_request_bytes ("request-duckdb",
      "skill",
      "account-1", "query-duckdb", "template.summary", "account-1", parameters);
  g_autoptr (GError) error = NULL;
  WyreboxDaemonDecodedRequestFrame decoded = { 0 };
  gpointer decoded_state = NULL;
  GDestroyNotify decoded_state_clear = NULL;

  g_assert_true (wyrebox_daemon_capnp_codec_decode_request_frame (NULL,
          request,
          &decoded, &decoded_state, &decoded_state_clear, NULL, &error));
  g_assert_no_error (error);
  g_assert_cmpstr (decoded.request_id, ==, "request-duckdb");
  g_assert_cmpstr (decoded.caller_identity, ==, "skill");
  g_assert_cmpstr (decoded.account_identity, ==, "account-1");
  g_assert_cmpstr (decoded.tool_identity, ==, "duckdb-tool");
  g_assert_cmpstr (decoded.correlation_id, ==, "corr-duckdb");
  g_assert_cmpint (decoded.operation, ==,
      WYREBOX_DAEMON_REQUEST_FRAME_OPERATION_DUCKDB_QUERY_TEMPLATE);
  g_assert_nonnull (decoded.duckdb_query_template);
  g_assert_cmpstr (decoded.duckdb_query_template->query_id, ==, "query-duckdb");
  g_assert_cmpstr (decoded.duckdb_query_template->template_id, ==,
      "template.summary");
  g_assert_cmpstr (decoded.duckdb_query_template->scope_id, ==, "account-1");
  g_assert_cmpstr (decoded.duckdb_query_template->parameters[0], ==, "mail-1");
  g_assert_cmpstr (decoded.duckdb_query_template->parameters[1], ==,
      "project-a");
  g_assert_null (decoded.duckdb_query_template->parameters[2]);
  g_assert_null (decoded.mailbox_list);
  g_assert_null (decoded.mailbox_select);
  g_assert_null (decoded.fact_mutation);
  g_assert_null (decoded.message_fetch);
  g_assert_null (decoded.message_search);
  g_assert_null (decoded.wirelog_predicate_query);
  g_assert_null (decoded.flag_keyword_update);

  g_assert_nonnull (decoded_state_clear);
  g_assert_nonnull (decoded_state);
  decoded_state_clear (decoded_state);
}

static void
assert_request_bytes_decode_duckdb_query_template_empty_parameters (void)
{
  const char *parameters[] = { NULL };
  g_autoptr (GBytes) request =
      build_duckdb_query_template_request_bytes
      ("request-duckdb-empty-parameters", "skill", "account-1",
      "query-duckdb-empty", "template.summary", "account-1", parameters);
  g_autoptr (GError) error = NULL;
  WyreboxDaemonDecodedRequestFrame decoded = { 0 };
  gpointer decoded_state = NULL;
  GDestroyNotify decoded_state_clear = NULL;

  g_assert_true (wyrebox_daemon_capnp_codec_decode_request_frame (NULL,
          request,
          &decoded, &decoded_state, &decoded_state_clear, NULL, &error));
  g_assert_no_error (error);
  g_assert_cmpint (decoded.operation, ==,
      WYREBOX_DAEMON_REQUEST_FRAME_OPERATION_DUCKDB_QUERY_TEMPLATE);
  g_assert_nonnull (decoded.duckdb_query_template);
  g_assert_cmpstr (decoded.duckdb_query_template->query_id, ==,
      "query-duckdb-empty");
  g_assert_nonnull (decoded.duckdb_query_template->parameters);
  g_assert_null (decoded.duckdb_query_template->parameters[0]);

  g_assert_nonnull (decoded_state_clear);
  g_assert_nonnull (decoded_state);
  decoded_state_clear (decoded_state);
}

static void
assert_request_bytes_rejects_duckdb_query_template_invalid_query_id (void)
{
  const char *parameters[] = { "mail-1", NULL };
  g_autoptr (GBytes) request =
      build_duckdb_query_template_request_bytes ("request-duckdb-invalid-query",
      "skill",
      "account-1", "query duckdb", "template.summary", "account-1", parameters);
  g_autoptr (GError) error = NULL;
  WyreboxDaemonDecodedRequestFrame decoded = { 0 };
  gpointer decoded_state = NULL;
  GDestroyNotify decoded_state_clear = NULL;

  g_assert_false (wyrebox_daemon_capnp_codec_decode_request_frame (NULL,
          request,
          &decoded, &decoded_state, &decoded_state_clear, NULL, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_null (decoded_state);
  g_assert_null (decoded_state_clear);
}

static void
assert_request_bytes_rejects_duckdb_query_template_invalid_template_id (void)
{
  const char *parameters[] = { "mail-1", NULL };
  g_autoptr (GBytes) request =
      build_duckdb_query_template_request_bytes
      ("request-duckdb-invalid-template", "skill", "account-1", "query-duckdb",
      "template summary", "account-1", parameters);
  g_autoptr (GError) error = NULL;
  WyreboxDaemonDecodedRequestFrame decoded = { 0 };
  gpointer decoded_state = NULL;
  GDestroyNotify decoded_state_clear = NULL;

  g_assert_false (wyrebox_daemon_capnp_codec_decode_request_frame (NULL,
          request,
          &decoded, &decoded_state, &decoded_state_clear, NULL, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_null (decoded_state);
  g_assert_null (decoded_state_clear);
}

static void
assert_request_bytes_rejects_duckdb_query_template_invalid_scope_id (void)
{
  const char *parameters[] = { "mail-1", NULL };
  g_autoptr (GBytes) request =
      build_duckdb_query_template_request_bytes ("request-duckdb-invalid-scope",
      "skill",
      "account-1",
      "query-duckdb", "template.summary", "account\n1", parameters);
  g_autoptr (GError) error = NULL;
  WyreboxDaemonDecodedRequestFrame decoded = { 0 };
  gpointer decoded_state = NULL;
  GDestroyNotify decoded_state_clear = NULL;

  g_assert_false (wyrebox_daemon_capnp_codec_decode_request_frame (NULL,
          request,
          &decoded, &decoded_state, &decoded_state_clear, NULL, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_null (decoded_state);
  g_assert_null (decoded_state_clear);
}

static void
assert_request_bytes_rejects_duckdb_query_template_invalid_parameter (void)
{
  const char *parameters[] = { "mail\n1", NULL };
  g_autoptr (GBytes) request =
      build_duckdb_query_template_request_bytes
      ("request-duckdb-invalid-parameter", "skill", "account-1", "query-duckdb",
      "template.summary", "account-1", parameters);
  g_autoptr (GError) error = NULL;
  WyreboxDaemonDecodedRequestFrame decoded = { 0 };
  gpointer decoded_state = NULL;
  GDestroyNotify decoded_state_clear = NULL;

  g_assert_false (wyrebox_daemon_capnp_codec_decode_request_frame (NULL,
          request,
          &decoded, &decoded_state, &decoded_state_clear, NULL, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_null (decoded_state);
  g_assert_null (decoded_state_clear);
}

static void
assert_request_bytes_decode_delivery_ingestion (void)
{
  const char *recipients[] = { "alice@example.com", "bob@example.com", NULL };
  const char *payload = "message-bytes";
  g_autoptr (GBytes) request =
      build_delivery_ingestion_request_bytes ("request-delivery-valid",
      "account-1", "postfix", "delivery-123", "QID", NULL, recipients,
      reinterpret_cast < const guint8 * >(payload), strlen (payload),
      "corr-delivery");
  g_autoptr (GError) error = NULL;
  WyreboxDaemonDecodedRequestFrame decoded = { 0 };
  gpointer decoded_state = NULL;
  GDestroyNotify decoded_state_clear = NULL;

  g_assert_true (wyrebox_daemon_capnp_codec_decode_request_frame (NULL,
          request,
          &decoded, &decoded_state, &decoded_state_clear, NULL, &error));
  g_assert_no_error (error);
  g_assert_cmpstr (decoded.request_id, ==, "request-delivery-valid");
  g_assert_cmpstr (decoded.caller_identity, ==, "postfix");
  g_assert_cmpstr (decoded.account_identity, ==, "account-1");
  g_assert_cmpstr (decoded.tool_identity, ==, "tool");
  g_assert_cmpstr (decoded.correlation_id, ==, "corr-delivery");
  g_assert_cmpint (decoded.operation, ==,
      WYREBOX_DAEMON_REQUEST_FRAME_OPERATION_DELIVERY_INGESTION);
  g_assert_nonnull (decoded.delivery_ingestion);
  g_assert_cmpstr (decoded.delivery_ingestion->delivery_id, ==, "delivery-123");
  g_assert_cmpstr (decoded.delivery_ingestion->queue_id, ==, "QID");
  g_assert_cmpstr (decoded.delivery_ingestion->envelope_sender, ==, "");
  g_assert_cmpstr (decoded.delivery_ingestion->recipients[0], ==,
      "alice@example.com");
  g_assert_cmpstr (decoded.delivery_ingestion->recipients[1], ==,
      "bob@example.com");
  g_assert_null (decoded.delivery_ingestion->recipients[2]);
  g_assert_cmpuint (g_bytes_get_size (decoded.
          delivery_ingestion->message_bytes), ==, strlen (payload));
  g_assert_null (decoded.mailbox_list);
  g_assert_null (decoded.mailbox_select);
  g_assert_null (decoded.fact_mutation);
  g_assert_null (decoded.message_fetch);
  g_assert_null (decoded.message_search);
  g_assert_null (decoded.flag_keyword_update);

  g_assert_nonnull (decoded_state_clear);
  g_assert_nonnull (decoded_state);
  decoded_state_clear (decoded_state);
}

static void
assert_delivery_ingestion_request_encoder_decodes_valid_request (void)
{
  const char *recipients[] = { "alice@example.com", "bob@example.com", NULL };
  const guint8 message_bytes[] = {
    'F', 'r', 'o', 'm', ':', ' ', 'a', '@', 'e', 'x', 'a', 'm', 'p', 'l',
    'e', '\r', '\n', '\0', 'b', 'o', 'd', 'y', 0xff
  };
  g_auto (WyreboxDaemonRequestIdentity) identity = { 0 };
  g_auto (WyreboxDaemonDeliveryIngestionRequest) request = { 0 };
  g_autoptr (GBytes) message = g_bytes_new_static (message_bytes,
      sizeof (message_bytes));
  g_autoptr (GBytes) encoded = NULL;
  g_autoptr (GError) error = NULL;
  WyreboxDaemonDecodedRequestFrame decoded = { 0 };
  gpointer decoded_state = NULL;
  GDestroyNotify decoded_state_clear = NULL;

  g_assert_true (wyrebox_daemon_request_identity_init (&identity,
          "request-encode-delivery",
          "postfix", "account-1", "pipe-helper", "corr-encode", &error));
  g_assert_no_error (error);
  g_assert_true (wyrebox_daemon_delivery_ingestion_request_init (&request,
          "delivery-encode-1",
          "QID-123", "sender@example.com", recipients, message, &error));
  g_assert_no_error (error);

  encoded =
      wyrebox_daemon_capnp_codec_encode_delivery_ingestion_request (&identity,
      &request, NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (encoded);

  g_assert_true (wyrebox_daemon_capnp_codec_decode_request_frame (NULL,
          encoded,
          &decoded, &decoded_state, &decoded_state_clear, NULL, &error));
  g_assert_no_error (error);
  g_assert_cmpstr (decoded.request_id, ==, "request-encode-delivery");
  g_assert_cmpstr (decoded.caller_identity, ==, "postfix");
  g_assert_cmpstr (decoded.account_identity, ==, "account-1");
  g_assert_cmpstr (decoded.tool_identity, ==, "pipe-helper");
  g_assert_cmpstr (decoded.correlation_id, ==, "corr-encode");
  g_assert_cmpint (decoded.operation, ==,
      WYREBOX_DAEMON_REQUEST_FRAME_OPERATION_DELIVERY_INGESTION);
  g_assert_nonnull (decoded.delivery_ingestion);
  g_assert_cmpstr (decoded.delivery_ingestion->delivery_id, ==,
      "delivery-encode-1");
  g_assert_cmpstr (decoded.delivery_ingestion->queue_id, ==, "QID-123");
  g_assert_cmpstr (decoded.delivery_ingestion->envelope_sender, ==,
      "sender@example.com");
  g_assert_cmpstr (decoded.delivery_ingestion->recipients[0], ==,
      "alice@example.com");
  g_assert_cmpstr (decoded.delivery_ingestion->recipients[1], ==,
      "bob@example.com");
  g_assert_null (decoded.delivery_ingestion->recipients[2]);

  gsize decoded_size = 0;
  const guint8 *decoded_bytes =
      static_cast <
      const guint8 *
      >(g_bytes_get_data (decoded.delivery_ingestion->message_bytes,
          &decoded_size));
  g_assert_cmpuint (decoded_size, ==, sizeof (message_bytes));
  g_assert_cmpmem (decoded_bytes, decoded_size, message_bytes,
      sizeof (message_bytes));

  g_assert_nonnull (decoded_state_clear);
  g_assert_nonnull (decoded_state);
  decoded_state_clear (decoded_state);
}

static void
assert_delivery_ingestion_request_encoder_decodes_missing_optionals (void)
{
  const char *recipients[] = { "rcpt@example.com", NULL };
  const char *payload = "From: sender@example.com\r\n\r\nbody";
  g_auto (WyreboxDaemonRequestIdentity) identity = { 0 };
  g_auto (WyreboxDaemonDeliveryIngestionRequest) request = { 0 };
  g_autoptr (GBytes) message = g_bytes_new_static (payload, strlen (payload));
  g_autoptr (GBytes) encoded = NULL;
  g_autoptr (GError) error = NULL;
  WyreboxDaemonDecodedRequestFrame decoded = { 0 };
  gpointer decoded_state = NULL;
  GDestroyNotify decoded_state_clear = NULL;

  g_assert_true (wyrebox_daemon_request_identity_init (&identity,
          "request-encode-delivery-optionals",
          "postfix", "account-1", "pipe-helper", NULL, &error));
  g_assert_no_error (error);
  g_assert_true (wyrebox_daemon_delivery_ingestion_request_init (&request,
          "delivery-encode-optionals",
          NULL, NULL, recipients, message, &error));
  g_assert_no_error (error);

  encoded =
      wyrebox_daemon_capnp_codec_encode_delivery_ingestion_request (&identity,
      &request, NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (encoded);

  g_assert_true (wyrebox_daemon_capnp_codec_decode_request_frame (NULL,
          encoded,
          &decoded, &decoded_state, &decoded_state_clear, NULL, &error));
  g_assert_no_error (error);
  g_assert_cmpstr (decoded.correlation_id, ==, "");
  g_assert_cmpstr (decoded.delivery_ingestion->queue_id, ==, "");
  g_assert_cmpstr (decoded.delivery_ingestion->envelope_sender, ==, "");
  g_assert_cmpstr (decoded.delivery_ingestion->recipients[0], ==,
      "rcpt@example.com");
  g_assert_null (decoded.delivery_ingestion->recipients[1]);

  gsize decoded_size = 0;
  const guint8 *decoded_bytes =
      static_cast <
      const guint8 *
      >(g_bytes_get_data (decoded.delivery_ingestion->message_bytes,
          &decoded_size));
  g_assert_cmpuint (decoded_size, ==, strlen (payload));
  g_assert_cmpmem (decoded_bytes, decoded_size, payload, strlen (payload));

  g_assert_nonnull (decoded_state_clear);
  g_assert_nonnull (decoded_state);
  decoded_state_clear (decoded_state);
}

static void
assert_delivery_ingestion_request_encoder_rejects_invalid_input (void)
{
  const char *recipients[] = { "rcpt@example.com", NULL };
  const char *payload = "message-bytes";
  g_auto (WyreboxDaemonRequestIdentity) identity = { 0 };
  g_auto (WyreboxDaemonDeliveryIngestionRequest) request = { 0 };
  g_autoptr (GBytes) message = g_bytes_new_static (payload, strlen (payload));
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) encoded = NULL;
  WyreboxDaemonRequestIdentity invalid_identity = { 0 };
  WyreboxDaemonDeliveryIngestionRequest invalid_request = { 0 };
  gchar *invalid_recipients[] = {
    const_cast < gchar * >("rcpt@example.com"),
    NULL
  };

  g_assert_true (wyrebox_daemon_request_identity_init (&identity,
          "request-encode-invalid",
          "postfix", "account-1", "pipe-helper", "corr-invalid", &error));
  g_assert_no_error (error);
  g_assert_true (wyrebox_daemon_delivery_ingestion_request_init (&request,
          "delivery-invalid",
          "QID-123", "sender@example.com", recipients, message, &error));
  g_assert_no_error (error);

  encoded = wyrebox_daemon_capnp_codec_encode_delivery_ingestion_request (NULL,
      &request, NULL, &error);
  g_assert_null (encoded);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_clear_error (&error);

  encoded =
      wyrebox_daemon_capnp_codec_encode_delivery_ingestion_request (&identity,
      NULL, NULL, &error);
  g_assert_null (encoded);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_clear_error (&error);

  encoded =
      wyrebox_daemon_capnp_codec_encode_delivery_ingestion_request
      (&invalid_identity, &request, NULL, &error);
  g_assert_null (encoded);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_clear_error (&error);

  invalid_request.delivery_id = const_cast < char *>("");
  invalid_request.queue_id = const_cast < char *>("QID-123");
  invalid_request.envelope_sender = const_cast < char *>("sender@example.com");
  invalid_request.recipients = invalid_recipients;
  invalid_request.message_bytes = message;

  encoded =
      wyrebox_daemon_capnp_codec_encode_delivery_ingestion_request (&identity,
      &invalid_request, NULL, &error);
  g_assert_null (encoded);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_clear_error (&error);

  invalid_request.delivery_id = const_cast < char *>("delivery-invalid");
  invalid_request.recipients = NULL;

  encoded =
      wyrebox_daemon_capnp_codec_encode_delivery_ingestion_request (&identity,
      &invalid_request, NULL, &error);
  g_assert_null (encoded);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_clear_error (&error);

  invalid_request.recipients = invalid_recipients;
  invalid_request.message_bytes = NULL;

  encoded =
      wyrebox_daemon_capnp_codec_encode_delivery_ingestion_request (&identity,
      &invalid_request, NULL, &error);
  g_assert_null (encoded);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
}

static void
assert_request_bytes_rejects_delivery_ingestion_missing_delivery_id (void)
{
  const char *recipients[] = { "alice@example.com", NULL };
  g_autoptr (GBytes) request =
      build_delivery_ingestion_request_bytes
      ("request-delivery-missing-delivery-id", "account-1", "postfix", NULL,
      "QID", NULL, recipients,
      reinterpret_cast < const guint8 * >("message-bytes"), 12,
      "corr-delivery");
  g_autoptr (GError) error = NULL;
  WyreboxDaemonDecodedRequestFrame decoded = { 0 };
  gpointer decoded_state = NULL;
  GDestroyNotify decoded_state_clear = NULL;

  g_assert_false (wyrebox_daemon_capnp_codec_decode_request_frame (NULL,
          request,
          &decoded, &decoded_state, &decoded_state_clear, NULL, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_null (decoded_state);
  g_assert_null (decoded_state_clear);
}

static void
assert_request_bytes_rejects_delivery_ingestion_control_character (void)
{
  const char *recipients[] = { "alice\nexample.com", NULL };
  g_autoptr (GBytes) request =
      build_delivery_ingestion_request_bytes
      ("request-delivery-invalid-control", "account-1", "postfix",
      "delivery-123", "QID", NULL, recipients,
      reinterpret_cast < const guint8 * >("message-bytes"), 12,
      "corr-delivery");
  g_autoptr (GError) error = NULL;
  WyreboxDaemonDecodedRequestFrame decoded = { 0 };
  gpointer decoded_state = NULL;
  GDestroyNotify decoded_state_clear = NULL;

  g_assert_false (wyrebox_daemon_capnp_codec_decode_request_frame (NULL,
          request,
          &decoded, &decoded_state, &decoded_state_clear, NULL, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_null (decoded_state);
  g_assert_null (decoded_state_clear);
}

static void
assert_request_bytes_rejects_delivery_ingestion_missing_recipients (void)
{
  g_autoptr (GBytes) request =
      build_delivery_ingestion_request_bytes
      ("request-delivery-missing-recipients", "account-1", "postfix",
      "delivery-123", "QID", NULL, NULL,
      reinterpret_cast < const guint8 * >("message-bytes"), 12,
      "corr-delivery");
  g_autoptr (GError) error = NULL;
  WyreboxDaemonDecodedRequestFrame decoded = { 0 };
  gpointer decoded_state = NULL;
  GDestroyNotify decoded_state_clear = NULL;

  g_assert_false (wyrebox_daemon_capnp_codec_decode_request_frame (NULL,
          request,
          &decoded, &decoded_state, &decoded_state_clear, NULL, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_null (decoded_state);
  g_assert_null (decoded_state_clear);
}

static void
assert_request_bytes_rejects_delivery_ingestion_empty_message_bytes (void)
{
  const char *recipients[] = { "alice@example.com", NULL };
  g_autoptr (GBytes) request =
      build_delivery_ingestion_request_bytes
      ("request-delivery-empty-message-bytes", "account-1", "postfix",
      "delivery-123", "QID", NULL, recipients, NULL, 0, "corr-delivery");
  g_autoptr (GError) error = NULL;
  WyreboxDaemonDecodedRequestFrame decoded = { 0 };
  gpointer decoded_state = NULL;
  GDestroyNotify decoded_state_clear = NULL;

  g_assert_false (wyrebox_daemon_capnp_codec_decode_request_frame (NULL,
          request,
          &decoded, &decoded_state, &decoded_state_clear, NULL, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_null (decoded_state);
  g_assert_null (decoded_state_clear);
}

static void
assert_request_bytes_decode_flag_keyword_update_set (void)
{
  const char *system_flags[] = { "\\Seen", "\\Flagged", NULL };
  const char *user_keywords[] = { "project", "todo", NULL };
  g_autoptr (GBytes) request =
      build_flag_keyword_update_request_bytes ("request-flag-keyword",
      "account-1", "account-1", "mailbox-inbox", 77, 42,
      FlagKeywordUpdateMode::SET, system_flags, user_keywords);
  g_autoptr (GError) error = NULL;
  WyreboxDaemonDecodedRequestFrame decoded = { 0 };
  gpointer decoded_state = NULL;
  GDestroyNotify decoded_state_clear = NULL;

  g_assert_true (wyrebox_daemon_capnp_codec_decode_request_frame (NULL,
          request,
          &decoded, &decoded_state, &decoded_state_clear, NULL, &error));
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
  g_assert_cmpstr (decoded.flag_keyword_update->system_flags[1], ==,
      "\\Flagged");
  g_assert_cmpstr (decoded.flag_keyword_update->user_keywords[0], ==,
      "project");
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
  g_autoptr (GBytes) clear_request =
      build_flag_keyword_update_request_bytes ("request-flag-keyword-clear",
      "account-1", "account-1", "mailbox-inbox", 77, 42,
      FlagKeywordUpdateMode::CLEAR, system_flags, user_keywords);
  g_autoptr (GBytes) replace_request =
      build_flag_keyword_update_request_bytes ("request-flag-keyword-replace",
      "account-1", "account-1", "mailbox-inbox", 77, 42,
      FlagKeywordUpdateMode::REPLACE, NULL, NULL);
  g_autoptr (GBytes) set_request =
      build_flag_keyword_update_request_bytes ("request-flag-keyword-set",
      "account-1", "account-1", "mailbox-inbox", 77, 42,
      FlagKeywordUpdateMode::SET, system_flags, NULL);
  g_autoptr (GError) error = NULL;
  WyreboxDaemonDecodedRequestFrame decoded = { 0 };
  gpointer decoded_state = NULL;
  GDestroyNotify decoded_state_clear = NULL;

  g_assert_true (wyrebox_daemon_capnp_codec_decode_request_frame (NULL,
          set_request,
          &decoded, &decoded_state, &decoded_state_clear, NULL, &error));
  g_assert_no_error (error);
  g_assert_cmpint (decoded.flag_keyword_update->mode, ==,
      WYREBOX_DAEMON_FLAG_KEYWORD_UPDATE_MODE_SET);
  g_assert_nonnull (decoded_state_clear);
  decoded_state_clear (decoded_state);

  g_assert_true (wyrebox_daemon_capnp_codec_decode_request_frame (NULL,
          clear_request,
          &decoded, &decoded_state, &decoded_state_clear, NULL, &error));
  g_assert_no_error (error);
  g_assert_cmpint (decoded.flag_keyword_update->mode, ==,
      WYREBOX_DAEMON_FLAG_KEYWORD_UPDATE_MODE_CLEAR);
  g_assert_nonnull (decoded_state_clear);
  decoded_state_clear (decoded_state);

  g_assert_true (wyrebox_daemon_capnp_codec_decode_request_frame (NULL,
          replace_request,
          &decoded, &decoded_state, &decoded_state_clear, NULL, &error));
  g_assert_no_error (error);
  g_assert_cmpint (decoded.flag_keyword_update->mode, ==,
      WYREBOX_DAEMON_FLAG_KEYWORD_UPDATE_MODE_REPLACE);
  g_assert_nonnull (decoded_state_clear);
  decoded_state_clear (decoded_state);
}

static void
assert_request_bytes_decode_flag_keyword_update_replace_empty_payload (void)
{
  g_autoptr (GBytes) request =
      build_flag_keyword_update_request_bytes
      ("request-flag-keyword-replace-empty", "account-1", "account-1",
      "mailbox-inbox", 77, 42, FlagKeywordUpdateMode::REPLACE, NULL, NULL);
  g_autoptr (GError) error = NULL;
  WyreboxDaemonDecodedRequestFrame decoded = { 0 };
  gpointer decoded_state = NULL;
  GDestroyNotify decoded_state_clear = NULL;

  g_assert_true (wyrebox_daemon_capnp_codec_decode_request_frame (NULL,
          request,
          &decoded, &decoded_state, &decoded_state_clear, NULL, &error));
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
  auto request_frame = request_builder.initRoot < RequestFrame > ();
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
  flag_keyword_update_request.setMode (static_cast < FlagKeywordUpdateMode >
      (99));
  auto encoded_system_flags = flag_keyword_update_request.initSystemFlags (1);
  encoded_system_flags.set (0, system_flags[0]);

  auto words = capnp::messageToFlatArray (request_builder);
  auto bytes = words.asBytes ();
  request = g_bytes_new (bytes.begin (), bytes.size ());

  g_assert_false (wyrebox_daemon_capnp_codec_decode_request_frame (NULL,
          request,
          &decoded, &decoded_state, &decoded_state_clear, NULL, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_null (decoded_state);
  g_assert_null (decoded_state_clear);
}

static void
assert_request_bytes_rejects_flag_keyword_update_missing_payload_for_set (void)
{
  g_autoptr (GBytes) request =
      build_flag_keyword_update_request_bytes
      ("request-flag-keyword-missing-payload", "account-1", "account-1",
      "mailbox-inbox", 77, 42, FlagKeywordUpdateMode::SET, NULL, NULL);
  g_autoptr (GError) error = NULL;
  WyreboxDaemonDecodedRequestFrame decoded = { 0 };
  gpointer decoded_state = NULL;
  GDestroyNotify decoded_state_clear = NULL;

  g_assert_false (wyrebox_daemon_capnp_codec_decode_request_frame (NULL,
          request,
          &decoded, &decoded_state, &decoded_state_clear, NULL, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_null (decoded_state);
  g_assert_null (decoded_state_clear);
}

static void
assert_request_bytes_rejects_flag_keyword_update_missing_account (void)
{
  const char *system_flags[] = { "\\Seen", NULL };
  g_autoptr (GBytes) request =
      build_flag_keyword_update_request_bytes
      ("request-flag-keyword-missing-account", "account-1", "", "mailbox-inbox",
      77, 42, FlagKeywordUpdateMode::SET, system_flags, NULL);
  g_autoptr (GError) error = NULL;
  WyreboxDaemonDecodedRequestFrame decoded = { 0 };
  gpointer decoded_state = NULL;
  GDestroyNotify decoded_state_clear = NULL;

  g_assert_false (wyrebox_daemon_capnp_codec_decode_request_frame (NULL,
          request,
          &decoded, &decoded_state, &decoded_state_clear, NULL, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_null (decoded_state);
  g_assert_null (decoded_state_clear);
}

static void
assert_request_bytes_rejects_flag_keyword_update_missing_mailbox (void)
{
  const char *system_flags[] = { "\\Seen", NULL };
  g_autoptr (GBytes) request =
      build_flag_keyword_update_request_bytes
      ("request-flag-keyword-missing-mailbox", "account-1", "account-1", "", 77,
      42, FlagKeywordUpdateMode::SET, system_flags, NULL);
  g_autoptr (GError) error = NULL;
  WyreboxDaemonDecodedRequestFrame decoded = { 0 };
  gpointer decoded_state = NULL;
  GDestroyNotify decoded_state_clear = NULL;

  g_assert_false (wyrebox_daemon_capnp_codec_decode_request_frame (NULL,
          request,
          &decoded, &decoded_state, &decoded_state_clear, NULL, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_null (decoded_state);
  g_assert_null (decoded_state_clear);
}

static void
assert_request_bytes_rejects_flag_keyword_update_zero_uid_validity (void)
{
  const char *system_flags[] = { "\\Seen", NULL };
  g_autoptr (GBytes) request =
      build_flag_keyword_update_request_bytes
      ("request-flag-keyword-zero-uid-validity", "account-1", "account-1",
      "mailbox-inbox", 0, 42, FlagKeywordUpdateMode::SET, system_flags, NULL);
  g_autoptr (GError) error = NULL;
  WyreboxDaemonDecodedRequestFrame decoded = { 0 };
  gpointer decoded_state = NULL;
  GDestroyNotify decoded_state_clear = NULL;

  g_assert_false (wyrebox_daemon_capnp_codec_decode_request_frame (NULL,
          request,
          &decoded, &decoded_state, &decoded_state_clear, NULL, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_null (decoded_state);
  g_assert_null (decoded_state_clear);
}

static void
assert_request_bytes_rejects_flag_keyword_update_zero_mailbox_uid (void)
{
  const char *system_flags[] = { "\\Seen", NULL };
  g_autoptr (GBytes) request =
      build_flag_keyword_update_request_bytes
      ("request-flag-keyword-zero-mailbox-uid", "account-1", "account-1",
      "mailbox-inbox", 77, 0, FlagKeywordUpdateMode::SET, system_flags, NULL);
  g_autoptr (GError) error = NULL;
  WyreboxDaemonDecodedRequestFrame decoded = { 0 };
  gpointer decoded_state = NULL;
  GDestroyNotify decoded_state_clear = NULL;

  g_assert_false (wyrebox_daemon_capnp_codec_decode_request_frame (NULL,
          request,
          &decoded, &decoded_state, &decoded_state_clear, NULL, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_null (decoded_state);
  g_assert_null (decoded_state_clear);
}

static void
assert_request_bytes_rejects_flag_keyword_update_unknown_system_flag (void)
{
  const char *system_flags[] = { "not-a-system-flag", NULL };
  g_autoptr (GBytes) request =
      build_flag_keyword_update_request_bytes
      ("request-flag-keyword-unknown-system-flag", "account-1", "account-1",
      "mailbox-inbox", 77, 42, FlagKeywordUpdateMode::SET, system_flags, NULL);
  g_autoptr (GError) error = NULL;
  WyreboxDaemonDecodedRequestFrame decoded = { 0 };
  gpointer decoded_state = NULL;
  GDestroyNotify decoded_state_clear = NULL;

  g_assert_false (wyrebox_daemon_capnp_codec_decode_request_frame (NULL,
          request,
          &decoded, &decoded_state, &decoded_state_clear, NULL, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_null (decoded_state);
  g_assert_null (decoded_state_clear);
}

static void
assert_request_bytes_rejects_flag_keyword_update_recent_system_flag (void)
{
  const char *system_flags[] = { "\\Recent", NULL };
  g_autoptr (GBytes) request =
      build_flag_keyword_update_request_bytes
      ("request-flag-keyword-recent-system-flag", "account-1", "account-1",
      "mailbox-inbox", 77, 42, FlagKeywordUpdateMode::SET, system_flags, NULL);
  g_autoptr (GError) error = NULL;
  WyreboxDaemonDecodedRequestFrame decoded = { 0 };
  gpointer decoded_state = NULL;
  GDestroyNotify decoded_state_clear = NULL;

  g_assert_false (wyrebox_daemon_capnp_codec_decode_request_frame (NULL,
          request,
          &decoded, &decoded_state, &decoded_state_clear, NULL, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_null (decoded_state);
  g_assert_null (decoded_state_clear);
}

static void
assert_request_bytes_rejects_flag_keyword_update_system_flag_keyword (void)
{
  const char *user_keywords[] = { "\\Seen", NULL };
  g_autoptr (GBytes) request =
      build_flag_keyword_update_request_bytes
      ("request-flag-keyword-system-flag-keyword", "account-1", "account-1",
      "mailbox-inbox", 77, 42, FlagKeywordUpdateMode::SET, NULL, user_keywords);
  g_autoptr (GError) error = NULL;
  WyreboxDaemonDecodedRequestFrame decoded = { 0 };
  gpointer decoded_state = NULL;
  GDestroyNotify decoded_state_clear = NULL;

  g_assert_false (wyrebox_daemon_capnp_codec_decode_request_frame (NULL,
          request,
          &decoded, &decoded_state, &decoded_state_clear, NULL, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_null (decoded_state);
  g_assert_null (decoded_state_clear);
}

static void
assert_request_bytes_rejects_flag_keyword_update_invalid_keyword (void)
{
  const char *user_keywords[] = { "needs review", NULL };
  g_autoptr (GBytes) request =
      build_flag_keyword_update_request_bytes
      ("request-flag-keyword-invalid-keyword", "account-1", "account-1",
      "mailbox-inbox", 77, 42, FlagKeywordUpdateMode::SET, NULL, user_keywords);
  g_autoptr (GError) error = NULL;
  WyreboxDaemonDecodedRequestFrame decoded = { 0 };
  gpointer decoded_state = NULL;
  GDestroyNotify decoded_state_clear = NULL;

  g_assert_false (wyrebox_daemon_capnp_codec_decode_request_frame (NULL,
          request,
          &decoded, &decoded_state, &decoded_state_clear, NULL, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_null (decoded_state);
  g_assert_null (decoded_state_clear);
}

static void
assert_request_bytes_rejects_flag_keyword_update_duplicate_flag (void)
{
  const char *system_flags[] = { "\\Seen", "\\Seen", NULL };
  g_autoptr (GBytes) request =
      build_flag_keyword_update_request_bytes
      ("request-flag-keyword-duplicate-flag", "account-1", "account-1",
      "mailbox-inbox", 77, 42, FlagKeywordUpdateMode::SET, system_flags, NULL);
  g_autoptr (GError) error = NULL;
  WyreboxDaemonDecodedRequestFrame decoded = { 0 };
  gpointer decoded_state = NULL;
  GDestroyNotify decoded_state_clear = NULL;

  g_assert_false (wyrebox_daemon_capnp_codec_decode_request_frame (NULL,
          request,
          &decoded, &decoded_state, &decoded_state_clear, NULL, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_null (decoded_state);
  g_assert_null (decoded_state_clear);
}

static void
assert_request_bytes_rejects_flag_keyword_update_duplicate_keyword (void)
{
  const char *user_keywords[] = { "project", "project", NULL };
  g_autoptr (GBytes) request =
      build_flag_keyword_update_request_bytes
      ("request-flag-keyword-duplicate-keyword", "account-1", "account-1",
      "mailbox-inbox", 77, 42, FlagKeywordUpdateMode::SET, NULL, user_keywords);
  g_autoptr (GError) error = NULL;
  WyreboxDaemonDecodedRequestFrame decoded = { 0 };
  gpointer decoded_state = NULL;
  GDestroyNotify decoded_state_clear = NULL;

  g_assert_false (wyrebox_daemon_capnp_codec_decode_request_frame (NULL,
          request,
          &decoded, &decoded_state, &decoded_state_clear, NULL, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_null (decoded_state);
  g_assert_null (decoded_state_clear);
}

static void
assert_request_bytes_rejects_message_fetch_missing_account_identity (void)
{
  g_autoptr (GBytes) request =
      build_message_fetch_request_bytes ("request-fetch-missing-account",
      "account-1", "", "mailbox-inbox", 77, 42);
  g_autoptr (GError) error = NULL;
  WyreboxDaemonDecodedRequestFrame decoded = { 0 };
  gpointer decoded_state = NULL;
  GDestroyNotify decoded_state_clear = NULL;

  g_assert_false (wyrebox_daemon_capnp_codec_decode_request_frame (NULL,
          request,
          &decoded, &decoded_state, &decoded_state_clear, NULL, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_null (decoded_state);
  g_assert_null (decoded_state_clear);
}

static void
assert_request_bytes_rejects_message_fetch_missing_mailbox_id (void)
{
  g_autoptr (GBytes) request =
      build_message_fetch_request_bytes ("request-fetch-missing-mailbox",
      "account-1", "account-1", "", 77, 42);
  g_autoptr (GError) error = NULL;
  WyreboxDaemonDecodedRequestFrame decoded = { 0 };
  gpointer decoded_state = NULL;
  GDestroyNotify decoded_state_clear = NULL;

  g_assert_false (wyrebox_daemon_capnp_codec_decode_request_frame (NULL,
          request,
          &decoded, &decoded_state, &decoded_state_clear, NULL, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_null (decoded_state);
  g_assert_null (decoded_state_clear);
}

static void
assert_request_bytes_rejects_message_fetch_zero_uid_validity (void)
{
  g_autoptr (GBytes) request =
      build_message_fetch_request_bytes ("request-fetch-zero-uid-validity",
      "account-1", "account-1", "mailbox-inbox", 0, 42);
  g_autoptr (GError) error = NULL;
  WyreboxDaemonDecodedRequestFrame decoded = { 0 };
  gpointer decoded_state = NULL;
  GDestroyNotify decoded_state_clear = NULL;

  g_assert_false (wyrebox_daemon_capnp_codec_decode_request_frame (NULL,
          request,
          &decoded, &decoded_state, &decoded_state_clear, NULL, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_null (decoded_state);
  g_assert_null (decoded_state_clear);
}

static void
assert_request_bytes_rejects_message_fetch_zero_mailbox_uid (void)
{
  g_autoptr (GBytes) request =
      build_message_fetch_request_bytes ("request-fetch-zero-mailbox-uid",
      "account-1", "account-1", "mailbox-inbox", 77, 0);
  g_autoptr (GError) error = NULL;
  WyreboxDaemonDecodedRequestFrame decoded = { 0 };
  gpointer decoded_state = NULL;
  GDestroyNotify decoded_state_clear = NULL;

  g_assert_false (wyrebox_daemon_capnp_codec_decode_request_frame (NULL,
          request,
          &decoded, &decoded_state, &decoded_state_clear, NULL, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_null (decoded_state);
  g_assert_null (decoded_state_clear);
}

static void
assert_request_bytes_decode_fact_mutation_insert (void)
{
  const char *arguments[] = { "mail-1", "project-a", NULL };
  g_autoptr (GBytes) request =
      build_fact_mutation_request_bytes (FactMutationKind::INSERT,
      "request-fact-insert", "skill", "account-1", "project_mention",
      "account-1", arguments);
  g_autoptr (GError) error = NULL;
  WyreboxDaemonDecodedRequestFrame decoded = { 0 };
  gpointer decoded_state = NULL;
  GDestroyNotify decoded_state_clear = NULL;

  g_assert_true (wyrebox_daemon_capnp_codec_decode_request_frame (NULL,
          request,
          &decoded, &decoded_state, &decoded_state_clear, NULL, &error));
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
  g_assert_cmpstr (decoded.fact_mutation->predicate_id, ==, "project_mention");
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
  g_autoptr (GBytes) request =
      build_fact_mutation_request_bytes (FactMutationKind::RETRACT,
      "request-fact-retract", "skill", "account-1", "project_mention",
      "account-1", arguments);
  g_autoptr (GError) error = NULL;
  WyreboxDaemonDecodedRequestFrame decoded = { 0 };
  gpointer decoded_state = NULL;
  GDestroyNotify decoded_state_clear = NULL;

  g_assert_true (wyrebox_daemon_capnp_codec_decode_request_frame (NULL,
          request,
          &decoded, &decoded_state, &decoded_state_clear, NULL, &error));
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
  g_autoptr (GBytes) request =
      build_fact_mutation_request_bytes (FactMutationKind::INSERT,
      "request-fact-invalid", "skill", "account-1", "", "account-1", arguments);
  g_autoptr (GError) error = NULL;
  WyreboxDaemonDecodedRequestFrame decoded = { 0 };
  gpointer decoded_state = NULL;
  GDestroyNotify decoded_state_clear = NULL;

  g_assert_false (wyrebox_daemon_capnp_codec_decode_request_frame (NULL,
          request,
          &decoded, &decoded_state, &decoded_state_clear, NULL, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_null (decoded_state);
  g_assert_null (decoded_state_clear);
}

static void
assert_fact_mutation_decode_rejects_missing_scope (void)
{
  const char *arguments[] = { "mail-1", NULL };
  g_autoptr (GBytes) request =
      build_fact_mutation_request_bytes (FactMutationKind::INSERT,
      "request-fact-invalid", "skill", "account-1", "project_mention", "",
      arguments);
  g_autoptr (GError) error = NULL;
  WyreboxDaemonDecodedRequestFrame decoded = { 0 };
  gpointer decoded_state = NULL;
  GDestroyNotify decoded_state_clear = NULL;

  g_assert_false (wyrebox_daemon_capnp_codec_decode_request_frame (NULL,
          request,
          &decoded, &decoded_state, &decoded_state_clear, NULL, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_null (decoded_state);
  g_assert_null (decoded_state_clear);
}

static void
assert_fact_mutation_decode_rejects_empty_argument (void)
{
  const char *arguments[] = { "", NULL };
  g_autoptr (GBytes) request =
      build_fact_mutation_request_bytes (FactMutationKind::INSERT,
      "request-fact-invalid", "skill", "account-1", "project_mention",
      "account-1", arguments);
  g_autoptr (GError) error = NULL;
  WyreboxDaemonDecodedRequestFrame decoded = { 0 };
  gpointer decoded_state = NULL;
  GDestroyNotify decoded_state_clear = NULL;

  g_assert_false (wyrebox_daemon_capnp_codec_decode_request_frame (NULL,
          request,
          &decoded, &decoded_state, &decoded_state_clear, NULL, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_null (decoded_state);
  g_assert_null (decoded_state_clear);
}

static void
assert_request_bytes_decode_fact_batch_import (void)
{
  const char *insert_arguments[] = { "mail-1", "project-a", NULL };
  const char *retract_arguments[] = { "mail-2", NULL };
  const FactBatchImportEntryFixture entries[] = {
    {
          FactMutationKind::INSERT,
          "project_mention",
          "account-1",
          insert_arguments,
        },
    {
          FactMutationKind::RETRACT,
          "reference_candidate",
          "account-1",
          retract_arguments,
        },
  };
  g_autoptr (GBytes) request =
      build_fact_batch_import_request_bytes ("request-fact-batch",
      "trusted-tool", "account-1", entries, G_N_ELEMENTS (entries));
  g_autoptr (GError) error = NULL;
  WyreboxDaemonDecodedRequestFrame decoded = { 0 };
  gpointer decoded_state = NULL;
  GDestroyNotify decoded_state_clear = NULL;

  g_assert_true (wyrebox_daemon_capnp_codec_decode_request_frame (NULL,
          request,
          &decoded, &decoded_state, &decoded_state_clear, NULL, &error));
  g_assert_no_error (error);
  g_assert_cmpstr (decoded.request_id, ==, "request-fact-batch");
  g_assert_cmpstr (decoded.caller_identity, ==, "trusted-tool");
  g_assert_cmpstr (decoded.account_identity, ==, "account-1");
  g_assert_cmpstr (decoded.tool_identity, ==, "fact-importer");
  g_assert_cmpstr (decoded.correlation_id, ==, "corr-fact-batch");
  g_assert_cmpint (decoded.operation, ==,
      WYREBOX_DAEMON_REQUEST_FRAME_OPERATION_FACT_BATCH_IMPORT);
  g_assert_nonnull (decoded.fact_batch_import);
  g_assert_cmpuint (wyrebox_daemon_fact_batch_import_request_get_n_entries
      (decoded.fact_batch_import), ==, 2);

  const WyreboxDaemonFactMutationRequest *first =
      wyrebox_daemon_fact_batch_import_request_get_entry
      (decoded.fact_batch_import, 0);
  const WyreboxDaemonFactMutationRequest *second =
      wyrebox_daemon_fact_batch_import_request_get_entry
      (decoded.fact_batch_import, 1);
  g_assert_cmpint (first->mutation, ==, WYREBOX_DAEMON_FACT_MUTATION_INSERT);
  g_assert_cmpstr (first->predicate_id, ==, "project_mention");
  g_assert_cmpstr (first->scope_id, ==, "account-1");
  g_assert_cmpstr (first->arguments[0], ==, "mail-1");
  g_assert_cmpstr (first->arguments[1], ==, "project-a");
  g_assert_cmpint (second->mutation, ==, WYREBOX_DAEMON_FACT_MUTATION_RETRACT);
  g_assert_cmpstr (second->predicate_id, ==, "reference_candidate");
  g_assert_cmpstr (second->scope_id, ==, "account-1");
  g_assert_cmpstr (second->arguments[0], ==, "mail-2");
  g_assert_null (decoded.fact_mutation);

  g_assert_nonnull (decoded_state_clear);
  g_assert_nonnull (decoded_state);
  decoded_state_clear (decoded_state);
}

static void
assert_fact_batch_import_decode_rejects_empty_batch (void)
{
  g_autoptr (GBytes) request =
      build_fact_batch_import_request_bytes ("request-fact-batch-invalid",
      "trusted-tool", "account-1", NULL, 0);
  g_autoptr (GError) error = NULL;
  WyreboxDaemonDecodedRequestFrame decoded = { 0 };
  gpointer decoded_state = NULL;
  GDestroyNotify decoded_state_clear = NULL;

  g_assert_false (wyrebox_daemon_capnp_codec_decode_request_frame (NULL,
          request,
          &decoded, &decoded_state, &decoded_state_clear, NULL, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_null (decoded_state);
  g_assert_null (decoded_state_clear);
}

static void
assert_fact_batch_import_decode_rejects_over_limit_batch (void)
{
  const char *arguments[] = { "mail-1", NULL };
  const guint n_entries =
      WYREBOX_DAEMON_FACT_BATCH_IMPORT_REQUEST_MAX_ENTRIES + 1;
  g_autofree FactBatchImportEntryFixture *entries = NULL;
  g_autoptr (GBytes) request = NULL;
  g_autoptr (GError) error = NULL;
  WyreboxDaemonDecodedRequestFrame decoded = { 0 };
  gpointer decoded_state = NULL;
  GDestroyNotify decoded_state_clear = NULL;

  entries = g_new0 (FactBatchImportEntryFixture, n_entries);
  for (guint i = 0; i < n_entries; i++) {
    entries[i].mutation = FactMutationKind::INSERT;
    entries[i].predicate_id = "project_mention";
    entries[i].scope_id = "account-1";
    entries[i].arguments = arguments;
  }

  request = build_fact_batch_import_request_bytes ("request-fact-batch-invalid",
      "trusted-tool", "account-1", entries, n_entries);

  g_assert_false (wyrebox_daemon_capnp_codec_decode_request_frame (NULL,
          request,
          &decoded, &decoded_state, &decoded_state_clear, NULL, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_null (decoded_state);
  g_assert_null (decoded_state_clear);
}

static void
assert_fact_batch_import_decode_rejects_invalid_nested_mutation (void)
{
  const char *valid_arguments[] = { "mail-1", NULL };
  const char *invalid_arguments[] = { "", NULL };
  const FactBatchImportEntryFixture entries[] = {
    {
          FactMutationKind::INSERT,
          "project_mention",
          "account-1",
          valid_arguments,
        },
    {
          FactMutationKind::RETRACT,
          "project_mention",
          "account-1",
          invalid_arguments,
        },
  };
  g_autoptr (GBytes) request =
      build_fact_batch_import_request_bytes ("request-fact-batch-invalid",
      "trusted-tool", "account-1", entries, G_N_ELEMENTS (entries));
  g_autoptr (GError) error = NULL;
  WyreboxDaemonDecodedRequestFrame decoded = { 0 };
  gpointer decoded_state = NULL;
  GDestroyNotify decoded_state_clear = NULL;

  g_assert_false (wyrebox_daemon_capnp_codec_decode_request_frame (NULL,
          request,
          &decoded, &decoded_state, &decoded_state_clear, NULL, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_null (decoded_state);
  g_assert_null (decoded_state_clear);
}

static void
assert_fact_batch_import_decode_rejects_mixed_scope (void)
{
  const char *arguments[] = { "mail-1", NULL };
  const FactBatchImportEntryFixture entries[] = {
    {
          FactMutationKind::INSERT,
          "project_mention",
          "account-1",
          arguments,
        },
    {
          FactMutationKind::RETRACT,
          "project_mention",
          "account-2",
          arguments,
        },
  };
  g_autoptr (GBytes) request =
      build_fact_batch_import_request_bytes ("request-fact-batch-invalid",
      "trusted-tool", "account-1", entries, G_N_ELEMENTS (entries));
  g_autoptr (GError) error = NULL;
  WyreboxDaemonDecodedRequestFrame decoded = { 0 };
  gpointer decoded_state = NULL;
  GDestroyNotify decoded_state_clear = NULL;

  g_assert_false (wyrebox_daemon_capnp_codec_decode_request_frame (NULL,
          request,
          &decoded, &decoded_state, &decoded_state_clear, NULL, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
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
          WYREBOX_DAEMON_MAILBOX_LIST_CHILD_STATE_HAS_NO_CHILDREN, &error));
  g_assert_no_error (error);

  g_assert_true (wyrebox_daemon_response_frame_init_mailbox_list (&response,
          "request-1", "corr-1", &result, &error));
  g_assert_no_error (error);

  encoded = wyrebox_daemon_capnp_codec_encode_response_frame (&response,
      NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (encoded);

  gsize size = 0;
  const guint8 *data =
      static_cast < const guint8 * >(g_bytes_get_data (encoded, &size));
  auto words = kj::arrayPtr (reinterpret_cast < const capnp::word * >(data),
      size / sizeof (capnp::word));
  capnp::FlatArrayMessageReader reader (words);

  auto response_frame = reader.getRoot < ResponseFrame > ();
  g_assert_true (response_frame.which () == ResponseFrame::MAILBOX_LIST);
  g_assert_cmpstr (response_frame.getMailboxList ().getRequestId ().cStr (), ==,
      "request-1");
  g_assert_cmpuint (response_frame.getMailboxList ().getEntries ().size (), ==,
      1);
  g_assert_cmpstr (response_frame.getMailboxList ().
      getEntries ()[0].getMailboxId ().cStr (), ==, "mailbox-inbox");
  g_assert_cmpstr (response_frame.getMailboxList ().
      getEntries ()[0].getHierarchyDelimiter ().cStr (), ==, "/");
}

static void
assert_response_bytes_decode_mailbox_list_roundtrip (void)
{
  g_auto (WyreboxDaemonMailboxListResult) result = { 0 };
  g_auto (WyreboxDaemonResponseFrame) response = { 0 };
  g_auto (WyreboxDaemonResponseFrame) decoded = { 0 };
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) encoded = NULL;
  const WyreboxDaemonMailboxListEntry *entry = NULL;

  wyrebox_daemon_mailbox_list_result_init_empty (&result);
  g_assert_true (wyrebox_daemon_mailbox_list_result_append_entry (&result,
          WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_ORDINARY,
          "mailbox-inbox",
          "INBOX",
          "/",
          "\\Inbox",
          TRUE,
          WYREBOX_DAEMON_MAILBOX_LIST_CHILD_STATE_HAS_NO_CHILDREN, &error));
  g_assert_no_error (error);
  g_assert_true (wyrebox_daemon_mailbox_list_result_append_entry (&result,
          WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_VIRTUAL,
          "view-projects",
          "Projects",
          "/",
          NULL,
          FALSE,
          WYREBOX_DAEMON_MAILBOX_LIST_CHILD_STATE_HAS_CHILDREN, &error));
  g_assert_no_error (error);

  g_assert_true (wyrebox_daemon_response_frame_init_mailbox_list (&response,
          "request-mailbox-list-decode",
          "corr-mailbox-list-decode", &result, &error));
  g_assert_no_error (error);

  encoded = wyrebox_daemon_capnp_codec_encode_response_frame (&response,
      NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (encoded);

  g_assert_true (wyrebox_daemon_capnp_codec_decode_response_frame (encoded,
          &decoded, &error));
  g_assert_no_error (error);
  g_assert_cmpint (decoded.kind, ==,
      WYREBOX_DAEMON_RESPONSE_FRAME_MAILBOX_LIST);
  g_assert_cmpstr (decoded.request_id, ==, "request-mailbox-list-decode");
  g_assert_cmpstr (decoded.correlation_id, ==, "corr-mailbox-list-decode");
  g_assert_cmpuint (wyrebox_daemon_mailbox_list_result_get_n_entries
      (&decoded.mailbox_list), ==, 2);

  entry = wyrebox_daemon_mailbox_list_result_get_entry
      (&decoded.mailbox_list, 0);
  g_assert_cmpint (entry->kind, ==,
      WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_ORDINARY);
  g_assert_cmpstr (entry->mailbox_id, ==, "mailbox-inbox");
  g_assert_cmpstr (entry->mailbox_name, ==, "INBOX");
  g_assert_cmpstr (entry->hierarchy_delimiter, ==, "/");
  g_assert_cmpstr (entry->special_use, ==, "\\Inbox");
  g_assert_true (entry->is_selectable);
  g_assert_cmpint (entry->child_state, ==,
      WYREBOX_DAEMON_MAILBOX_LIST_CHILD_STATE_HAS_NO_CHILDREN);

  entry = wyrebox_daemon_mailbox_list_result_get_entry
      (&decoded.mailbox_list, 1);
  g_assert_cmpint (entry->kind, ==,
      WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_VIRTUAL);
  g_assert_cmpstr (entry->mailbox_id, ==, "view-projects");
  g_assert_cmpstr (entry->mailbox_name, ==, "Projects");
  g_assert_cmpstr (entry->hierarchy_delimiter, ==, "/");
  g_assert_null (entry->special_use);
  g_assert_false (entry->is_selectable);
  g_assert_cmpint (entry->child_state, ==,
      WYREBOX_DAEMON_MAILBOX_LIST_CHILD_STATE_HAS_CHILDREN);
}

static void
assert_response_bytes_encode_mailbox_select (void)
{
  g_auto (WyreboxDaemonMailboxSelectResult) result = { };
  g_auto (WyreboxDaemonResponseFrame) response = { 0 };
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) encoded = NULL;

  g_assert_true (wyrebox_daemon_mailbox_select_result_init (&result,
          WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_VIRTUAL,
          "view-project-a", "Projects/Project A", 99, 1234, 56, &error));
  g_assert_no_error (error);

  g_assert_true (wyrebox_daemon_response_frame_init_mailbox_select (&response,
          "request-select-response", "corr-select-response", &result, &error));
  g_assert_no_error (error);

  encoded = wyrebox_daemon_capnp_codec_encode_response_frame (&response,
      NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (encoded);

  gsize size = 0;
  const guint8 *data =
      static_cast < const guint8 * >(g_bytes_get_data (encoded, &size));
  auto words = kj::arrayPtr (reinterpret_cast < const capnp::word * >(data),
      size / sizeof (capnp::word));
  capnp::FlatArrayMessageReader reader (words);

  auto response_frame = reader.getRoot < ResponseFrame > ();
  g_assert_true (response_frame.which () == ResponseFrame::MAILBOX_SELECT);
  g_assert_cmpstr (response_frame.getRequestId ().cStr (), ==,
      "request-select-response");
  g_assert_cmpstr (response_frame.getCorrelationId ().cStr (), ==,
      "corr-select-response");
  g_assert_cmpstr (response_frame.getMailboxSelect ().getRequestId ().cStr (),
      ==, "request-select-response");
  g_assert_true (response_frame.getMailboxSelect ().getKind () ==
      MailboxListEntryKind::VIRTUAL);
  g_assert_cmpstr (response_frame.getMailboxSelect ().getMailboxId ().cStr (),
      ==, "view-project-a");
  g_assert_cmpstr (response_frame.getMailboxSelect ().getMailboxName ().cStr (),
      ==, "Projects/Project A");
  g_assert_cmpuint (response_frame.getMailboxSelect ().getUidValidity (), ==,
      99);
  g_assert_cmpuint (response_frame.getMailboxSelect ().getUidNext (), ==, 1234);
  g_assert_cmpuint (response_frame.getMailboxSelect ().getMessageCount (), ==,
      56);
}

static void
assert_response_bytes_decode_mailbox_select_roundtrip (void)
{
  g_auto (WyreboxDaemonMailboxSelectResult) result = { };
  g_auto (WyreboxDaemonResponseFrame) response = { 0 };
  g_auto (WyreboxDaemonResponseFrame) decoded = { 0 };
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) encoded = NULL;

  g_assert_true (wyrebox_daemon_mailbox_select_result_init (&result,
          WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_VIRTUAL,
          "view-project-a", "Projects/Project A", 99, 1234, 56, &error));
  g_assert_no_error (error);

  g_assert_true (wyrebox_daemon_response_frame_init_mailbox_select (&response,
          "request-select-decode", "corr-select-decode", &result, &error));
  g_assert_no_error (error);

  encoded = wyrebox_daemon_capnp_codec_encode_response_frame (&response,
      NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (encoded);

  g_assert_true (wyrebox_daemon_capnp_codec_decode_response_frame (encoded,
          &decoded, &error));
  g_assert_no_error (error);
  g_assert_cmpint (decoded.kind, ==,
      WYREBOX_DAEMON_RESPONSE_FRAME_MAILBOX_SELECT);
  g_assert_cmpstr (decoded.request_id, ==, "request-select-decode");
  g_assert_cmpstr (decoded.correlation_id, ==, "corr-select-decode");
  g_assert_cmpint (decoded.mailbox_select.kind, ==,
      WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_VIRTUAL);
  g_assert_cmpstr (decoded.mailbox_select.mailbox_id, ==, "view-project-a");
  g_assert_cmpstr (decoded.mailbox_select.mailbox_name, ==,
      "Projects/Project A");
  g_assert_cmpuint (decoded.mailbox_select.uid_validity, ==, 99);
  g_assert_cmpuint (decoded.mailbox_select.uid_next, ==, 1234);
  g_assert_cmpuint (decoded.mailbox_select.message_count, ==, 56);
}

static void
assert_response_bytes_rejects_mailbox_select_missing_payload_request_id (void)
{
  g_auto (WyreboxDaemonResponseFrame) decoded = { 0 };
  g_autoptr (GBytes) encoded = NULL;
  g_autoptr (GError) error = NULL;
  capnp::MallocMessageBuilder response_builder;
  auto response_frame = response_builder.initRoot < ResponseFrame > ();
  auto response_select = response_frame.initMailboxSelect ();

  response_frame.setRequestId ("request-mailbox-select-invalid");
  response_frame.setCorrelationId ("corr-invalid");
  response_select.setRequestId ("");
  response_select.setKind (MailboxListEntryKind::ORDINARY);
  response_select.setMailboxId ("view-project-a");
  response_select.setMailboxName ("Projects/Project A");
  response_select.setUidValidity (99);
  response_select.setUidNext (1234);

  auto words = capnp::messageToFlatArray (response_builder);
  auto bytes = words.asBytes ();
  encoded = g_bytes_new (bytes.begin (), bytes.size ());

  g_assert_false (wyrebox_daemon_capnp_codec_decode_response_frame (encoded,
          &decoded, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
}

static void
    assert_response_bytes_rejects_mailbox_select_mismatched_payload_request_id
    (void)
{
  g_auto (WyreboxDaemonResponseFrame) decoded = { 0 };
  g_autoptr (GBytes) encoded = NULL;
  g_autoptr (GError) error = NULL;
  capnp::MallocMessageBuilder response_builder;
  auto response_frame = response_builder.initRoot < ResponseFrame > ();
  auto response_select = response_frame.initMailboxSelect ();

  response_frame.setRequestId ("request-envelope");
  response_frame.setCorrelationId ("corr-invalid");
  response_select.setRequestId ("request-payload");
  response_select.setKind (MailboxListEntryKind::VIRTUAL);
  response_select.setMailboxId ("view-project-a");
  response_select.setMailboxName ("Projects/Project A");
  response_select.setUidValidity (99);
  response_select.setUidNext (1234);

  auto words = capnp::messageToFlatArray (response_builder);
  auto bytes = words.asBytes ();
  encoded = g_bytes_new (bytes.begin (), bytes.size ());

  g_assert_false (wyrebox_daemon_capnp_codec_decode_response_frame (encoded,
          &decoded, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
}

static void
assert_response_bytes_rejects_mailbox_select_invalid_fields (void)
{
  g_auto (WyreboxDaemonResponseFrame) decoded = { 0 };
  g_autoptr (GBytes) encoded = NULL;
  g_autoptr (GError) error = NULL;
  capnp::MallocMessageBuilder response_builder;
  auto response_frame = response_builder.initRoot < ResponseFrame > ();
  auto response_select = response_frame.initMailboxSelect ();

  response_frame.setRequestId ("request-mailbox-select-invalid");
  response_frame.setCorrelationId ("corr-invalid");
  response_select.setRequestId ("request-mailbox-select-invalid");
  response_select.setKind (MailboxListEntryKind::ORDINARY);
  response_select.setMailboxId ("");
  response_select.setMailboxName ("");
  response_select.setUidValidity (99);
  response_select.setUidNext (1234);

  auto words = capnp::messageToFlatArray (response_builder);
  auto bytes = words.asBytes ();
  encoded = g_bytes_new (bytes.begin (), bytes.size ());

  g_assert_false (wyrebox_daemon_capnp_codec_decode_response_frame (encoded,
          &decoded, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
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
          "message-1", NULL, "corr-stream", 42, chunk_bytes, FALSE, &error));
  g_assert_no_error (error);

  g_assert_true (wyrebox_daemon_response_frame_init_stream_chunk (&response,
          &chunk, &error));
  g_assert_no_error (error);

  encoded = wyrebox_daemon_capnp_codec_encode_response_frame (&response,
      NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (encoded);

  gsize size = 0;
  const guint8 *data =
      static_cast < const guint8 * >(g_bytes_get_data (encoded, &size));
  auto words = kj::arrayPtr (reinterpret_cast < const capnp::word * >(data),
      size / sizeof (capnp::word));
  capnp::FlatArrayMessageReader reader (words);

  auto response_frame = reader.getRoot < ResponseFrame > ();
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
      ==, "corr-stream");
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
      NULL, &error);
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
      NULL, &error);
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
          NULL, "query-1", NULL, 43, NULL, TRUE, &error));
  g_assert_no_error (error);

  g_assert_true (wyrebox_daemon_response_frame_init_stream_chunk (&response,
          &chunk, &error));
  g_assert_no_error (error);

  encoded = wyrebox_daemon_capnp_codec_encode_response_frame (&response,
      NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (encoded);

  gsize size = 0;
  const guint8 *data =
      static_cast < const guint8 * >(g_bytes_get_data (encoded, &size));
  auto words = kj::arrayPtr (reinterpret_cast < const capnp::word * >(data),
      size / sizeof (capnp::word));
  capnp::FlatArrayMessageReader reader (words);

  auto response_frame = reader.getRoot < ResponseFrame > ();
  g_assert_true (response_frame.which () == ResponseFrame::STREAM_CHUNK);
  g_assert_cmpstr (response_frame.getStreamChunk ().getRequestId ().cStr (), ==,
      "request-stream-final");
  g_assert_cmpstr (response_frame.getStreamChunk ().getMessageId ().cStr (), ==,
      "");
  g_assert_cmpstr (response_frame.getStreamChunk ().getQueryId ().cStr (), ==,
      "query-1");
  g_assert_cmpuint (response_frame.getStreamChunk ().getChunkIndex (), ==, 43);
  g_assert_true (response_frame.getStreamChunk ().getEndOfStream ());
  g_assert_cmpuint (response_frame.getStreamChunk ().getBytes ().size (), ==,
      0);
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
          WYREBOX_DAEMON_ERROR_PERMISSION_DENIED, "denied", "retry", &error));
  g_assert_no_error (error);

  g_assert_true (wyrebox_daemon_response_frame_init_error (&response,
          &error_frame, "corr-2", &error));
  g_assert_no_error (error);

  encoded = wyrebox_daemon_capnp_codec_encode_response_frame (&response,
      NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (encoded);

  gsize size = 0;
  const guint8 *data =
      static_cast < const guint8 * >(g_bytes_get_data (encoded, &size));
  auto words = kj::arrayPtr (reinterpret_cast < const capnp::word * >(data),
      size / sizeof (capnp::word));
  capnp::FlatArrayMessageReader reader (words);

  auto response_frame = reader.getRoot < ResponseFrame > ();
  g_assert_true (response_frame.which () == ResponseFrame::ERROR);
  g_assert_cmpstr (response_frame.getError ().getRequestId ().cStr (), ==,
      "request-2");
  g_assert_true (response_frame.getError ().getErrorClass () ==
      ErrorClass::PERMISSION_DENIED);
  g_assert_cmpstr (response_frame.getError ().getMessage ().cStr (), ==,
      "denied");
}

static void
assert_response_bytes_encode_success (void)
{
  const char *arguments[] = { "mail-1", NULL };
  g_auto (WyreboxDaemonFactMutationRequest) request = { };
  g_auto (WyreboxDaemonResponseFrame) response = { 0 };
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) encoded = NULL;

  g_assert_true (wyrebox_daemon_fact_mutation_request_init (&request,
          WYREBOX_DAEMON_FACT_MUTATION_INSERT,
          "project_mention", "account-1", arguments, &error));
  g_assert_no_error (error);

  g_assert_true (wyrebox_daemon_response_frame_init_fact_mutation_success
      (&response, "request-fact-success", "corr-fact", &request, 4096, 7,
          &error));
  g_assert_no_error (error);

  encoded = wyrebox_daemon_capnp_codec_encode_response_frame (&response,
      NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (encoded);

  gsize size = 0;
  const guint8 *data =
      static_cast < const guint8 * >(g_bytes_get_data (encoded, &size));
  auto words = kj::arrayPtr (reinterpret_cast < const capnp::word * >(data),
      size / sizeof (capnp::word));
  capnp::FlatArrayMessageReader reader (words);

  auto response_frame = reader.getRoot < ResponseFrame > ();
  g_assert_true (response_frame.which () == ResponseFrame::SUCCESS);
  g_assert_cmpstr (response_frame.getRequestId ().cStr (), ==,
      "request-fact-success");
  g_assert_cmpstr (response_frame.getCorrelationId ().cStr (), ==, "corr-fact");
  g_assert_cmpstr (response_frame.getSuccess ().getRequestId ().cStr (), ==,
      "request-fact-success");
  g_assert_cmpstr (response_frame.getSuccess ().getDurableMarker ().cStr (), ==,
      "journal:4096:7");
  g_assert_cmpuint (response_frame.getSuccess ().getJournalOffset (), ==, 4096);
  g_assert_cmpuint (response_frame.getSuccess ().getJournalSequence (), ==, 7);
  g_assert_cmpstr (response_frame.getSuccess ().getSummary ().cStr (), ==,
      "fact_mutation mutation=insert predicate_id=project_mention "
      "scope_id=account-1 argument_count=1");
}

static void
assert_response_bytes_decode_success_roundtrip (void)
{
  const char *arguments[] = { "mail-1", NULL };
  g_auto (WyreboxDaemonFactMutationRequest) request = { };
  g_auto (WyreboxDaemonResponseFrame) response = { 0 };
  g_auto (WyreboxDaemonResponseFrame) decoded = { 0 };
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) encoded = NULL;

  g_assert_true (wyrebox_daemon_fact_mutation_request_init (&request,
          WYREBOX_DAEMON_FACT_MUTATION_INSERT,
          "project_mention", "account-1", arguments, &error));
  g_assert_no_error (error);

  g_assert_true (wyrebox_daemon_response_frame_init_fact_mutation_success
      (&response, "request-fact-success-decode", "corr-fact-decode", &request,
          8192, 11, &error));
  g_assert_no_error (error);

  encoded = wyrebox_daemon_capnp_codec_encode_response_frame (&response,
      NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (encoded);

  g_assert_true (wyrebox_daemon_capnp_codec_decode_response_frame (encoded,
          &decoded, &error));
  g_assert_no_error (error);
  g_assert_cmpint (decoded.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_SUCCESS);
  g_assert_cmpstr (decoded.request_id, ==, "request-fact-success-decode");
  g_assert_cmpstr (decoded.correlation_id, ==, "corr-fact-decode");
  g_assert_cmpstr (decoded.success.request_id, ==,
      "request-fact-success-decode");
  g_assert_cmpstr (decoded.success.durable_marker, ==, "journal:8192:11");
  g_assert_cmpuint (decoded.success.journal_offset, ==, 8192);
  g_assert_cmpuint (decoded.success.journal_sequence, ==, 11);
  g_assert_cmpstr (decoded.success.summary, ==,
      "fact_mutation mutation=insert predicate_id=project_mention "
      "scope_id=account-1 argument_count=1");
}

static void
assert_response_bytes_decode_error_roundtrip (void)
{
  g_auto (WyreboxDaemonErrorFrame) error_frame = { 0 };
  g_auto (WyreboxDaemonResponseFrame) response = { 0 };
  g_auto (WyreboxDaemonResponseFrame) decoded = { 0 };
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) encoded = NULL;

  g_assert_true (wyrebox_daemon_error_frame_init (&error_frame,
          "request-error-decode",
          WYREBOX_DAEMON_ERROR_PERMISSION_DENIED, "denied", "retry", &error));
  g_assert_no_error (error);

  g_assert_true (wyrebox_daemon_response_frame_init_error (&response,
          &error_frame, "corr-error-decode", &error));
  g_assert_no_error (error);

  encoded = wyrebox_daemon_capnp_codec_encode_response_frame (&response,
      NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (encoded);

  g_assert_true (wyrebox_daemon_capnp_codec_decode_response_frame (encoded,
          &decoded, &error));
  g_assert_no_error (error);
  g_assert_cmpint (decoded.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_ERROR);
  g_assert_cmpstr (decoded.request_id, ==, "request-error-decode");
  g_assert_cmpstr (decoded.correlation_id, ==, "corr-error-decode");
  g_assert_cmpstr (decoded.error.request_id, ==, "request-error-decode");
  g_assert_cmpint (decoded.error.error_class, ==,
      WYREBOX_DAEMON_ERROR_PERMISSION_DENIED);
  g_assert_cmpstr (decoded.error.message, ==, "denied");
  g_assert_cmpstr (decoded.error.retry_hint, ==, "retry");
}

static void
assert_response_bytes_decode_rejects_malformed (void)
{
  const guint8 payload[] = { 0xff, 0xff, 0xff };
  g_autoptr (GBytes) encoded = g_bytes_new_static (payload, sizeof (payload));
  g_auto (WyreboxDaemonResponseFrame) decoded = { 0 };
  g_autoptr (GError) error = NULL;

  g_assert_false (wyrebox_daemon_capnp_codec_decode_response_frame (encoded,
          &decoded, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_cmpint (decoded.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_NONE);
}

static void
assert_response_bytes_rejects_mailbox_list_mismatched_payload_request_id (void)
{
  g_auto (WyreboxDaemonResponseFrame) decoded = { 0 };
  g_autoptr (GBytes) encoded = NULL;
  g_autoptr (GError) error = NULL;
  capnp::MallocMessageBuilder response_builder;
  auto response_frame = response_builder.initRoot < ResponseFrame > ();
  response_frame.setRequestId ("request-envelope");
  response_frame.setCorrelationId ("corr-mailbox-list-decode");
  auto response_list = response_frame.initMailboxList ();
  response_list.setRequestId ("request-payload");
  auto entries = response_list.initEntries (1);
  entries[0].setKind (MailboxListEntryKind::ORDINARY);
  entries[0].setMailboxId ("mailbox-inbox");
  entries[0].setMailboxName ("INBOX");
  entries[0].setHierarchyDelimiter ("/");
  entries[0].setSpecialUse ("\\Inbox");
  entries[0].setSelectable (true);
  entries[0].setChildState (MailboxListChildState::HAS_NO_CHILDREN);
  auto words = capnp::messageToFlatArray (response_builder);
  auto bytes = words.asBytes ();
  encoded = g_bytes_new (bytes.begin (), bytes.size ());

  g_assert_false (wyrebox_daemon_capnp_codec_decode_response_frame (encoded,
          &decoded, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_cmpint (decoded.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_NONE);
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
      service_state, NULL);
  g_assert_nonnull (service);

  adapter = wyrebox_daemon_request_adapter_new (NULL, NULL,
      service,
      NULL,
      NULL,
      NULL,
      NULL,
      NULL,
      wyrebox_daemon_capnp_codec_decode_request_frame,
      NULL, NULL, wyrebox_daemon_capnp_codec_encode_response_frame, NULL, NULL);
  g_assert_nonnull (adapter);

  request = build_mailbox_list_request_bytes ("", "request-1");
  const WyreboxDaemonPeerCredentials peer_credentials = {
    .uid = 100,
    .gid = 101,
    .pid = 102,
  };

  response_bytes =
      wyrebox_daemon_request_adapter_handle_payload (&peer_credentials, request,
      adapter, &error);
  g_assert_no_error (error);
  g_assert_nonnull (response_bytes);
  g_assert_true (service_state->was_called);

  gsize size = 0;
  const guint8 *data =
      static_cast < const guint8 * >(g_bytes_get_data (response_bytes, &size));
  auto words = kj::arrayPtr (reinterpret_cast < const capnp::word * >(data),
      size / sizeof (capnp::word));
  capnp::FlatArrayMessageReader reader (words);
  auto response_frame = reader.getRoot < ResponseFrame > ();
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
      service_state, NULL);
  g_assert_nonnull (service);

  adapter = wyrebox_daemon_request_adapter_new (NULL, NULL,
      NULL,
      service,
      NULL,
      NULL,
      NULL,
      NULL,
      wyrebox_daemon_capnp_codec_decode_request_frame,
      NULL, NULL, wyrebox_daemon_capnp_codec_encode_response_frame, NULL, NULL);
  g_assert_nonnull (adapter);

  request = build_mailbox_select_request_bytes ("mailbox-inbox",
      "", "request-select-route");
  const WyreboxDaemonPeerCredentials peer_credentials = {
    .uid = 100,
    .gid = 101,
    .pid = 102,
  };

  response_bytes =
      wyrebox_daemon_request_adapter_handle_payload (&peer_credentials, request,
      adapter, &error);
  g_assert_no_error (error);
  g_assert_nonnull (response_bytes);
  g_assert_true (service_state->was_called);

  gsize size = 0;
  const guint8 *data =
      static_cast < const guint8 * >(g_bytes_get_data (response_bytes, &size));
  auto words = kj::arrayPtr (reinterpret_cast < const capnp::word * >(data),
      size / sizeof (capnp::word));
  capnp::FlatArrayMessageReader reader (words);
  auto response_frame = reader.getRoot < ResponseFrame > ();
  g_assert_true (response_frame.which () == ResponseFrame::MAILBOX_SELECT);
  g_assert_cmpstr (response_frame.getMailboxSelect ().getRequestId ().cStr (),
      ==, "request-select-route");
  g_assert_cmpstr (response_frame.getMailboxSelect ().getMailboxId ().cStr (),
      ==, "mailbox-inbox");
  g_assert_cmpstr (response_frame.getMailboxSelect ().getMailboxName ().cStr (),
      ==, "INBOX");
  g_assert_cmpuint (response_frame.getMailboxSelect ().getUidValidity (), ==,
      77);
  g_assert_cmpuint (response_frame.getMailboxSelect ().getUidNext (), ==, 42);
  g_assert_cmpuint (response_frame.getMailboxSelect ().getMessageCount (), ==,
      123);
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

  adapter = wyrebox_daemon_request_adapter_new (NULL, service,
      NULL,
      NULL,
      NULL,
      NULL,
      NULL,
      NULL,
      wyrebox_daemon_capnp_codec_decode_request_frame,
      NULL, NULL, wyrebox_daemon_capnp_codec_encode_response_frame, NULL, NULL);
  g_assert_nonnull (adapter);

  request = build_fact_mutation_request_bytes (FactMutationKind::INSERT,
      "request-fact-route",
      "trusted-tool", "account-1", "project_mention", "account-1", arguments);
  const WyreboxDaemonPeerCredentials peer_credentials = {
    .uid = 100,
    .gid = 101,
    .pid = 102,
  };

  response_bytes =
      wyrebox_daemon_request_adapter_handle_payload (&peer_credentials, request,
      adapter, &error);
  g_assert_no_error (error);
  g_assert_nonnull (response_bytes);

  gsize size = 0;
  const guint8 *data =
      static_cast < const guint8 * >(g_bytes_get_data (response_bytes, &size));
  auto words = kj::arrayPtr (reinterpret_cast < const capnp::word * >(data),
      size / sizeof (capnp::word));
  capnp::FlatArrayMessageReader reader (words);
  auto response_frame = reader.getRoot < ResponseFrame > ();
  g_assert_true (response_frame.which () == ResponseFrame::SUCCESS);
  g_assert_cmpstr (response_frame.getSuccess ().getRequestId ().cStr (), ==,
      "request-fact-route");
  g_assert_cmpstr (response_frame.getSuccess ().getDurableMarker ().cStr (), ==,
      "journal:0:1");
  g_assert_cmpuint (response_frame.getSuccess ().getJournalSequence (), ==, 1);

  remove_tree (root);
}

static void
read_next_journal_fact_mutation (WyreboxJournalReader *reader,
    WyreboxJournalEventType expected_event_type,
    WyreboxDaemonFactMutationRequest *out_request, GError **error)
{
  g_auto (WyreboxJournalRecord) record = { 0 };
  gboolean eof = FALSE;

  g_assert_true (wyrebox_journal_reader_read_next (reader,
          &record, &eof, error));
  g_assert_false (eof);
  g_assert_cmpint (record.event_type, ==, expected_event_type);
  g_assert_true (wyrebox_daemon_fact_mutation_request_decode (record.payload,
          out_request, error));
}

static void
assert_request_adapter_routes_fact_batch_import (void)
{
  const char *insert_arguments[] = { "mail-1", "project-a", NULL };
  const char *retract_arguments[] = { "mail-2", NULL };
  const FactBatchImportEntryFixture entries[] = {
    {
          FactMutationKind::INSERT,
          "project_mention",
          "account-1",
          insert_arguments,
        },
    {
          FactMutationKind::RETRACT,
          "reference_candidate",
          "account-1",
          retract_arguments,
        },
  };
  g_autofree char *root =
      g_dir_make_tmp ("wyrebox-capnp-fact-batch-XXXXXX", NULL);
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) request = NULL;
  g_autoptr (GBytes) response_bytes = NULL;
  g_autoptr (WyreboxJournalWriter) writer = NULL;
  g_autoptr (WyreboxJournalReader) reader = NULL;
  g_autoptr (WyreboxDaemonFactMutationService) service = NULL;
  g_autoptr (WyreboxDaemonRequestAdapter) adapter = NULL;
  g_auto (WyreboxDaemonFactMutationRequest) first = { };
  g_auto (WyreboxDaemonFactMutationRequest) second = { };
  g_auto (WyreboxJournalRecord) extra = { 0 };
  g_auto (WyreboxDaemonAuditPayload) audit = { };
  gboolean eof = FALSE;

  g_assert_nonnull (root);

  writer = wyrebox_journal_writer_new (root, &error);
  g_assert_no_error (error);
  g_assert_nonnull (writer);

  service = wyrebox_daemon_fact_mutation_service_new (writer);
  g_assert_nonnull (service);

  adapter = wyrebox_daemon_request_adapter_new (NULL, service,
      NULL,
      NULL,
      NULL,
      NULL,
      NULL,
      NULL,
      wyrebox_daemon_capnp_codec_decode_request_frame,
      NULL, NULL, wyrebox_daemon_capnp_codec_encode_response_frame, NULL, NULL);
  g_assert_nonnull (adapter);

  request = build_fact_batch_import_request_bytes ("request-fact-batch-route",
      "trusted-tool", "account-1", entries, G_N_ELEMENTS (entries));
  const WyreboxDaemonPeerCredentials peer_credentials = {
    .uid = 100,
    .gid = 101,
    .pid = 102,
  };

  response_bytes =
      wyrebox_daemon_request_adapter_handle_payload (&peer_credentials, request,
      adapter, &error);
  g_assert_no_error (error);
  g_assert_nonnull (response_bytes);

  gsize size = 0;
  const guint8 *data =
      static_cast < const guint8 * >(g_bytes_get_data (response_bytes, &size));
  auto words = kj::arrayPtr (reinterpret_cast < const capnp::word * >(data),
      size / sizeof (capnp::word));
  capnp::FlatArrayMessageReader response_reader (words);
  auto response_frame = response_reader.getRoot < ResponseFrame > ();
  g_assert_true (response_frame.which () == ResponseFrame::SUCCESS);
  g_assert_cmpstr (response_frame.getSuccess ().getRequestId ().cStr (), ==,
      "request-fact-batch-route");
  g_assert_cmpuint (response_frame.getSuccess ().getJournalSequence (), ==, 2);
  g_assert_cmpstr (response_frame.getSuccess ().getSummary ().cStr (), ==,
      "fact_batch_import count=2 scope_id=account-1");

  reader = wyrebox_journal_reader_new (root, &error);
  g_assert_no_error (error);
  g_assert_nonnull (reader);

  read_next_journal_fact_mutation (reader,
      WYREBOX_JOURNAL_EVENT_FACT_INSERTED, &first, &error);
  g_assert_no_error (error);
  g_assert_cmpint (first.mutation, ==, WYREBOX_DAEMON_FACT_MUTATION_INSERT);
  g_assert_cmpstr (first.predicate_id, ==, "project_mention");
  g_assert_cmpstr (first.scope_id, ==, "account-1");
  g_assert_cmpstr (first.arguments[0], ==, "mail-1");
  g_assert_cmpstr (first.arguments[1], ==, "project-a");

  read_next_journal_fact_mutation (reader,
      WYREBOX_JOURNAL_EVENT_FACT_RETRACTED, &second, &error);
  g_assert_no_error (error);
  g_assert_cmpint (second.mutation, ==, WYREBOX_DAEMON_FACT_MUTATION_RETRACT);
  g_assert_cmpstr (second.predicate_id, ==, "reference_candidate");
  g_assert_cmpstr (second.scope_id, ==, "account-1");
  g_assert_cmpstr (second.arguments[0], ==, "mail-2");

  g_assert_true (wyrebox_journal_reader_read_next (reader,
          &extra, &eof, &error));
  g_assert_no_error (error);
  g_assert_false (eof);
  g_assert_cmpint (extra.event_type, ==,
      WYREBOX_JOURNAL_EVENT_DAEMON_AUDIT_RECORDED);
  g_assert_cmpuint (extra.sequence, ==, 3);
  g_assert_true (wyrebox_daemon_audit_payload_decode (extra.payload,
          &audit, &error));
  g_assert_no_error (error);
  g_assert_cmpint (audit.operation, ==,
      WYREBOX_DAEMON_AUDIT_OPERATION_FACT_BATCH_IMPORT);
  g_assert_cmpint (audit.outcome, ==, WYREBOX_DAEMON_AUDIT_OUTCOME_SUCCESS);
  g_assert_cmpstr (audit.request_id, ==, "request-fact-batch-route");
  g_assert_cmpstr (audit.caller_identity, ==, "trusted-tool");
  g_assert_cmpstr (audit.account_identity, ==, "account-1");
  g_assert_cmpstr (audit.scope_id, ==, "account-1");
  g_assert_cmpuint (audit.mutation_count, ==, 2);
  g_assert_cmpuint (audit.final_journal_sequence, ==, 2);

  wyrebox_journal_record_clear (&extra);
  g_assert_false (wyrebox_journal_reader_read_next (reader,
          &extra, &eof, &error));
  g_assert_no_error (error);
  g_assert_true (eof);

  remove_tree (root);
}

static void
assert_request_adapter_rejects_unauthorized_fact_batch_import (void)
{
  const char *arguments[] = { "mail-1", NULL };
  const FactBatchImportEntryFixture entries[] = {
    {
          FactMutationKind::INSERT,
          "project_mention",
          "account-1",
          arguments,
        },
  };
  g_autofree char *root =
      g_dir_make_tmp ("wyrebox-capnp-fact-batch-denied-XXXXXX", NULL);
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) request = NULL;
  g_autoptr (GBytes) response_bytes = NULL;
  g_autoptr (WyreboxJournalWriter) writer = NULL;
  g_autoptr (WyreboxJournalReader) reader = NULL;
  g_autoptr (WyreboxDaemonFactMutationService) service = NULL;
  g_autoptr (WyreboxDaemonRequestAdapter) adapter = NULL;
  g_auto (WyreboxJournalRecord) record = { 0 };
  gboolean eof = FALSE;

  g_assert_nonnull (root);

  writer = wyrebox_journal_writer_new (root, &error);
  g_assert_no_error (error);
  service = wyrebox_daemon_fact_mutation_service_new (writer);
  g_assert_nonnull (service);

  adapter = wyrebox_daemon_request_adapter_new (NULL, service,
      NULL,
      NULL,
      NULL,
      NULL,
      NULL,
      NULL,
      wyrebox_daemon_capnp_codec_decode_request_frame,
      NULL, NULL, wyrebox_daemon_capnp_codec_encode_response_frame, NULL, NULL);
  g_assert_nonnull (adapter);

  request = build_fact_batch_import_request_bytes ("request-fact-batch-denied",
      "admin-cli", "account-1", entries, G_N_ELEMENTS (entries));
  const WyreboxDaemonPeerCredentials peer_credentials = {
    .uid = 100,
    .gid = 101,
    .pid = 102,
  };

  response_bytes =
      wyrebox_daemon_request_adapter_handle_payload (&peer_credentials, request,
      adapter, &error);
  g_assert_no_error (error);
  g_assert_nonnull (response_bytes);

  gsize size = 0;
  const guint8 *data =
      static_cast < const guint8 * >(g_bytes_get_data (response_bytes, &size));
  auto words = kj::arrayPtr (reinterpret_cast < const capnp::word * >(data),
      size / sizeof (capnp::word));
  capnp::FlatArrayMessageReader response_reader (words);
  auto response_frame = response_reader.getRoot < ResponseFrame > ();
  g_assert_true (response_frame.which () == ResponseFrame::ERROR);
  g_assert_true (response_frame.getError ().getErrorClass () ==
      ErrorClass::PERMISSION_DENIED);

  reader = wyrebox_journal_reader_new (root, &error);
  g_assert_no_error (error);
  g_assert_false (wyrebox_journal_reader_read_next (reader,
          &record, &eof, &error));
  g_assert_no_error (error);
  g_assert_true (eof);

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
      service_state, NULL);
  g_assert_nonnull (service);

  adapter =
      wyrebox_daemon_request_adapter_new (NULL, NULL,
      NULL,
      NULL,
      service,
      NULL,
      NULL,
      NULL,
      wyrebox_daemon_capnp_codec_decode_request_frame,
      NULL, NULL, wyrebox_daemon_capnp_codec_encode_response_frame, NULL, NULL);
  g_assert_nonnull (adapter);

  request = build_message_fetch_request_bytes ("request-fetch-route",
      "account-1", "account-1", "mailbox-inbox", 77, 42);
  const WyreboxDaemonPeerCredentials peer_credentials = {
    .uid = 100,
    .gid = 101,
    .pid = 102,
  };

  response_bytes =
      wyrebox_daemon_request_adapter_handle_payload (&peer_credentials, request,
      adapter, &error);
  g_assert_no_error (error);
  g_assert_nonnull (response_bytes);
  g_assert_true (service_state->was_called);

  gsize size = 0;
  const guint8 *data =
      static_cast < const guint8 * >(g_bytes_get_data (response_bytes, &size));
  auto words = kj::arrayPtr (reinterpret_cast < const capnp::word * >(data),
      size / sizeof (capnp::word));
  capnp::FlatArrayMessageReader reader (words);
  auto response_frame = reader.getRoot < ResponseFrame > ();

  g_assert_true (response_frame.which () == ResponseFrame::STREAM_CHUNK);
  g_assert_cmpstr (response_frame.getStreamChunk ().getRequestId ().cStr (), ==,
      "request-fetch-route");
  g_assert_cmpstr (response_frame.getStreamChunk ().getMessageId ().cStr (), ==,
      "message-1");
  g_assert_cmpstr (response_frame.getStreamChunk ().getQueryId ().cStr (), ==,
      "");
  g_assert_cmpstr (response_frame.getStreamChunk ().getCorrelationId ().cStr (),
      ==, "corr-fetch");
  g_assert_cmpuint (response_frame.getStreamChunk ().getChunkIndex (), ==, 0);
  g_assert_true (response_frame.getStreamChunk ().getEndOfStream ());
}

static void
assert_request_adapter_routes_message_search (void)
{
  g_autoptr (GError) error = NULL;
  g_autofree FixtureState *service_state = g_new0 (FixtureState, 1);
  g_autoptr (GBytes) request = NULL;
  g_autoptr (GBytes) response_bytes = NULL;
  g_autoptr (WyreboxDaemonMessageSearchService) service = NULL;
  g_autoptr (WyreboxDaemonRequestAdapter) adapter = NULL;
  const WyreboxDaemonPeerCredentials peer_credentials = {
    .uid = 100,
    .gid = 101,
    .pid = 102,
  };

  service = wyrebox_daemon_message_search_service_new (search_message_fixture,
      service_state, NULL);
  g_assert_nonnull (service);

  adapter = wyrebox_daemon_request_adapter_new (NULL, NULL,
      NULL,
      NULL,
      NULL,
      service,
      NULL,
      NULL,
      wyrebox_daemon_capnp_codec_decode_request_frame,
      NULL, NULL, wyrebox_daemon_capnp_codec_encode_response_frame, NULL, NULL);
  g_assert_nonnull (adapter);

  request = build_message_search_request_bytes ("request-search-route",
      "account-1", "account-1", "mailbox-inbox", 77, "unseen");

  response_bytes =
      wyrebox_daemon_request_adapter_handle_payload (&peer_credentials, request,
      adapter, &error);
  g_assert_no_error (error);
  g_assert_nonnull (response_bytes);
  g_assert_true (service_state->was_called);

  gsize size = 0;
  const guint8 *data =
      static_cast < const guint8 * >(g_bytes_get_data (response_bytes, &size));
  auto words = kj::arrayPtr (reinterpret_cast < const capnp::word * >(data),
      size / sizeof (capnp::word));
  capnp::FlatArrayMessageReader reader (words);
  auto response_frame = reader.getRoot < ResponseFrame > ();

  g_assert_true (response_frame.which () == ResponseFrame::STREAM_CHUNK);
  g_assert_cmpstr (response_frame.getStreamChunk ().getRequestId ().cStr (), ==,
      "request-search-route");
  g_assert_cmpstr (response_frame.getStreamChunk ().getMessageId ().cStr (), ==,
      "");
  g_assert_cmpstr (response_frame.getStreamChunk ().getQueryId ().cStr (), ==,
      "query-search");
  g_assert_cmpstr (response_frame.getStreamChunk ().getCorrelationId ().cStr (),
      ==, "corr-search");
  g_assert_cmpuint (response_frame.getStreamChunk ().getChunkIndex (), ==, 0);
  g_assert_true (response_frame.getStreamChunk ().getEndOfStream ());
}

static void
assert_request_adapter_routes_wirelog_predicate_query (void)
{
  const char *bindings[] = { NULL };
  g_autoptr (GError) error = NULL;
  g_autofree FixtureState *service_state = g_new0 (FixtureState, 1);
  g_autoptr (GBytes) request = NULL;
  g_autoptr (GBytes) response_bytes = NULL;
  g_autoptr (WyreboxDaemonWirelogPredicateQueryService) service = NULL;
  g_autoptr (WyreboxDaemonRequestAdapter) adapter = NULL;
  const WyreboxDaemonPeerCredentials peer_credentials = {
    .uid = 100,
    .gid = 101,
    .pid = 102,
  };

  service =
      wyrebox_daemon_wirelog_predicate_query_service_new
      (query_wirelog_predicate_fixture, service_state, NULL);
  g_assert_nonnull (service);

  adapter = wyrebox_daemon_request_adapter_new (NULL, NULL,
      NULL,
      NULL,
      NULL,
      NULL,
      service,
      NULL,
      wyrebox_daemon_capnp_codec_decode_request_frame,
      NULL, NULL, wyrebox_daemon_capnp_codec_encode_response_frame, NULL, NULL);
  g_assert_nonnull (adapter);

  request =
      build_wirelog_predicate_query_request_bytes ("request-wirelog-route",
      "trusted-tool", "account-1", "query-wirelog", "show_in_virtual_folder.v1",
      "account-1", bindings);

  response_bytes =
      wyrebox_daemon_request_adapter_handle_payload (&peer_credentials, request,
      adapter, &error);
  g_assert_no_error (error);
  g_assert_nonnull (response_bytes);
  g_assert_true (service_state->was_called);

  gsize size = 0;
  const guint8 *data =
      static_cast < const guint8 * >(g_bytes_get_data (response_bytes, &size));
  auto words = kj::arrayPtr (reinterpret_cast < const capnp::word * >(data),
      size / sizeof (capnp::word));
  capnp::FlatArrayMessageReader reader (words);
  auto response_frame = reader.getRoot < ResponseFrame > ();

  g_assert_true (response_frame.which () == ResponseFrame::STREAM_CHUNK);
  g_assert_cmpstr (response_frame.getStreamChunk ().getRequestId ().cStr (), ==,
      "request-wirelog-route");
  g_assert_cmpstr (response_frame.getStreamChunk ().getMessageId ().cStr (), ==,
      "");
  g_assert_cmpstr (response_frame.getStreamChunk ().getQueryId ().cStr (), ==,
      "query-wirelog");
  g_assert_cmpstr (response_frame.getStreamChunk ().getCorrelationId ().cStr (),
      ==, "corr-wirelog");
  g_assert_cmpuint (response_frame.getStreamChunk ().getChunkIndex (), ==, 0);
  g_assert_true (response_frame.getStreamChunk ().getEndOfStream ());
}

static void
assert_request_adapter_routes_duckdb_query_template (void)
{
  const char *parameters[] = { "mailbox-inbox", NULL };
  g_autoptr (GError) error = NULL;
  g_autofree FixtureState *service_state = g_new0 (FixtureState, 1);
  g_autoptr (GBytes) request = NULL;
  g_autoptr (GBytes) response_bytes = NULL;
  g_autoptr (WyreboxDaemonDuckDBQueryTemplateService) service = NULL;
  g_autoptr (WyreboxDaemonRequestAdapter) adapter = NULL;
  const WyreboxDaemonPeerCredentials peer_credentials = {
    .uid = 100,
    .gid = 101,
    .pid = 102,
  };

  service =
      wyrebox_daemon_duckdb_query_template_service_new
      (query_duckdb_template_fixture, service_state, NULL);
  g_assert_nonnull (service);

  adapter = wyrebox_daemon_request_adapter_new (NULL, NULL,
      NULL,
      NULL,
      NULL,
      NULL,
      NULL,
      NULL,
      wyrebox_daemon_capnp_codec_decode_request_frame,
      NULL, NULL, wyrebox_daemon_capnp_codec_encode_response_frame, NULL, NULL);
  g_assert_nonnull (adapter);
  wyrebox_daemon_request_adapter_set_duckdb_query_template_service (adapter,
      service);

  request = build_duckdb_query_template_request_bytes ("request-duckdb-route",
      "admin-cli",
      "account-1",
      "query-duckdb", "mailbox.uid_map.v1", "account-1", parameters);

  response_bytes =
      wyrebox_daemon_request_adapter_handle_payload (&peer_credentials, request,
      adapter, &error);
  g_assert_no_error (error);
  g_assert_nonnull (response_bytes);
  g_assert_true (service_state->was_called);

  gsize size = 0;
  const guint8 *data =
      static_cast < const guint8 * >(g_bytes_get_data (response_bytes, &size));
  auto words = kj::arrayPtr (reinterpret_cast < const capnp::word * >(data),
      size / sizeof (capnp::word));
  capnp::FlatArrayMessageReader reader (words);
  auto response_frame = reader.getRoot < ResponseFrame > ();

  g_assert_true (response_frame.which () == ResponseFrame::STREAM_CHUNK);
  g_assert_cmpstr (response_frame.getStreamChunk ().getRequestId ().cStr (), ==,
      "request-duckdb-route");
  g_assert_cmpstr (response_frame.getStreamChunk ().getMessageId ().cStr (), ==,
      "");
  g_assert_cmpstr (response_frame.getStreamChunk ().getQueryId ().cStr (), ==,
      "query-duckdb");
  g_assert_cmpstr (response_frame.getStreamChunk ().getCorrelationId ().cStr (),
      ==, "corr-duckdb");
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

  service =
      wyrebox_daemon_flag_keyword_update_service_new
      (update_flag_keyword_fixture, &was_called, NULL);
  g_assert_nonnull (service);

  adapter = wyrebox_daemon_request_adapter_new (NULL, NULL,
      NULL,
      NULL,
      NULL,
      NULL,
      NULL,
      service,
      wyrebox_daemon_capnp_codec_decode_request_frame,
      NULL, NULL, wyrebox_daemon_capnp_codec_encode_response_frame, NULL, NULL);
  g_assert_nonnull (adapter);

  request = build_flag_keyword_update_request_bytes ("request-flag-keyword",
      "account-1",
      "account-1",
      "mailbox-inbox",
      77, 42, FlagKeywordUpdateMode::SET, system_flags, user_keywords);

  response_bytes =
      wyrebox_daemon_request_adapter_handle_payload (&peer_credentials, request,
      adapter, &error);
  g_assert_no_error (error);
  g_assert_nonnull (response_bytes);
  g_assert_true (was_called);

  gsize size = 0;
  const guint8 *data =
      static_cast < const guint8 * >(g_bytes_get_data (response_bytes, &size));
  auto words = kj::arrayPtr (reinterpret_cast < const capnp::word * >(data),
      size / sizeof (capnp::word));
  capnp::FlatArrayMessageReader reader (words);
  auto response_frame = reader.getRoot < ResponseFrame > ();
  g_assert_true (response_frame.which () == ResponseFrame::SUCCESS);
  g_assert_cmpstr (response_frame.getSuccess ().getRequestId ().cStr (), ==,
      "request-flag-keyword");
  g_assert_cmpstr (response_frame.getSuccess ().getDurableMarker ().cStr (), ==,
      "journal:123:456");
  g_assert_cmpuint (response_frame.getSuccess ().getJournalSequence (), ==,
      456);
}

static void
assert_request_adapter_routes_delivery_ingestion (void)
{
  const char *recipients[] = { "alice@example.com", NULL };
  const char *payload = "message-bytes";
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) request = NULL;
  g_autoptr (GBytes) response_bytes = NULL;
  g_autoptr (WyreboxDaemonDeliveryIngestionService) service = NULL;
  g_autoptr (WyreboxDaemonRequestAdapter) adapter = NULL;
  gboolean was_called = FALSE;
  const WyreboxDaemonPeerCredentials peer_credentials = {
    .uid = 10,
    .gid = 20,
    .pid = 30,
  };

  service =
      wyrebox_daemon_delivery_ingestion_service_new (ingest_delivery_fixture,
      &was_called, NULL);
  g_assert_nonnull (service);

  adapter =
      wyrebox_daemon_request_adapter_new (service, NULL,
      NULL,
      NULL,
      NULL,
      NULL,
      NULL,
      NULL,
      wyrebox_daemon_capnp_codec_decode_request_frame,
      NULL, NULL, wyrebox_daemon_capnp_codec_encode_response_frame, NULL, NULL);
  g_assert_nonnull (adapter);

  request = build_delivery_ingestion_request_bytes ("request-delivery-route",
      "account-1",
      "postfix",
      "delivery-123",
      "QID",
      NULL,
      recipients,
      reinterpret_cast < const guint8 * >(payload),
      strlen (payload), "corr-delivery");

  response_bytes =
      wyrebox_daemon_request_adapter_handle_payload (&peer_credentials, request,
      adapter, &error);
  g_assert_no_error (error);
  g_assert_nonnull (response_bytes);
  g_assert_true (was_called);

  gsize size = 0;
  const guint8 *data =
      static_cast < const guint8 * >(g_bytes_get_data (response_bytes, &size));
  auto words = kj::arrayPtr (reinterpret_cast < const capnp::word * >(data),
      size / sizeof (capnp::word));
  capnp::FlatArrayMessageReader reader (words);
  auto response_frame = reader.getRoot < ResponseFrame > ();

  g_assert_true (response_frame.which () == ResponseFrame::SUCCESS);
  g_assert_cmpstr (response_frame.getSuccess ().getRequestId ().cStr (), ==,
      "request-delivery-route");
  g_assert_cmpstr (response_frame.getSuccess ().getDurableMarker ().cStr (), ==,
      "journal:4096:7");
  g_assert_cmpstr (response_frame.getSuccess ().getSummary ().cStr (), ==,
      "delivery_ingestion object_key=sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef size_bytes=12");
  g_assert_cmpuint (response_frame.getSuccess ().getJournalSequence (), ==, 7);
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
  g_test_add_func
      ("/daemon-api/capnp/codec/reject-mailbox-select-missing-selector",
      assert_mailbox_select_decode_rejects_missing_selector);
  g_test_add_func
      ("/daemon-api/capnp/codec/reject-mailbox-select-both-selectors",
      assert_mailbox_select_decode_rejects_both_selectors);
  g_test_add_func ("/daemon-api/capnp/codec/decode-fact-mutation-insert",
      assert_request_bytes_decode_fact_mutation_insert);
  g_test_add_func ("/daemon-api/capnp/codec/decode-fact-mutation-retract",
      assert_request_bytes_decode_fact_mutation_retract);
  g_test_add_func ("/daemon-api/capnp/codec/decode-fact-batch-import",
      assert_request_bytes_decode_fact_batch_import);
  g_test_add_func ("/daemon-api/capnp/codec/decode-message-fetch-valid",
      assert_request_bytes_decode_message_fetch);
  g_test_add_func ("/daemon-api/capnp/codec/decode-message-search-valid",
      assert_request_bytes_decode_message_search);
  g_test_add_func
      ("/daemon-api/capnp/codec/reject-message-search-missing-account",
      assert_request_bytes_rejects_message_search_missing_account);
  g_test_add_func
      ("/daemon-api/capnp/codec/reject-message-search-missing-mailbox",
      assert_request_bytes_rejects_message_search_missing_mailbox);
  g_test_add_func
      ("/daemon-api/capnp/codec/reject-message-search-zero-uid-validity",
      assert_request_bytes_rejects_message_search_zero_uid_validity);
  g_test_add_func
      ("/daemon-api/capnp/codec/reject-message-search-empty-criteria",
      assert_request_bytes_rejects_message_search_empty_criteria);
  g_test_add_func
      ("/daemon-api/capnp/codec/reject-message-search-invalid-criteria-backslash",
      assert_request_bytes_rejects_message_search_invalid_criteria_backslash);
  g_test_add_func
      ("/daemon-api/capnp/codec/reject-message-search-invalid-criteria-sql",
      assert_request_bytes_rejects_message_search_invalid_criteria_sql_like);
  g_test_add_func
      ("/daemon-api/capnp/codec/decode-wirelog-predicate-query-bindings",
      assert_request_bytes_decode_wirelog_predicate_query_with_bindings);
  g_test_add_func
      ("/daemon-api/capnp/codec/decode-wirelog-predicate-query-empty-bindings",
      assert_request_bytes_decode_wirelog_predicate_query_empty_bindings);
  g_test_add_func
      ("/daemon-api/capnp/codec/reject-wirelog-predicate-query-invalid-query",
      assert_request_bytes_rejects_wirelog_predicate_query_invalid_query_id);
  g_test_add_func
      ("/daemon-api/capnp/codec/reject-wirelog-predicate-query-invalid-predicate",
      assert_request_bytes_rejects_wirelog_predicate_query_invalid_predicate_id);
  g_test_add_func
      ("/daemon-api/capnp/codec/reject-wirelog-predicate-query-invalid-scope",
      assert_request_bytes_rejects_wirelog_predicate_query_invalid_scope_id);
  g_test_add_func
      ("/daemon-api/capnp/codec/reject-wirelog-predicate-query-invalid-binding",
      assert_request_bytes_rejects_wirelog_predicate_query_invalid_binding);
  g_test_add_func
      ("/daemon-api/capnp/codec/decode-duckdb-query-template-parameters",
      assert_request_bytes_decode_duckdb_query_template_with_parameters);
  g_test_add_func
      ("/daemon-api/capnp/codec/decode-duckdb-query-template-empty-parameters",
      assert_request_bytes_decode_duckdb_query_template_empty_parameters);
  g_test_add_func
      ("/daemon-api/capnp/codec/reject-duckdb-query-template-invalid-query",
      assert_request_bytes_rejects_duckdb_query_template_invalid_query_id);
  g_test_add_func
      ("/daemon-api/capnp/codec/reject-duckdb-query-template-invalid-template",
      assert_request_bytes_rejects_duckdb_query_template_invalid_template_id);
  g_test_add_func
      ("/daemon-api/capnp/codec/reject-duckdb-query-template-invalid-scope",
      assert_request_bytes_rejects_duckdb_query_template_invalid_scope_id);
  g_test_add_func
      ("/daemon-api/capnp/codec/reject-duckdb-query-template-invalid-parameter",
      assert_request_bytes_rejects_duckdb_query_template_invalid_parameter);
  g_test_add_func ("/daemon-api/capnp/codec/decode-delivery-ingestion-valid",
      assert_request_bytes_decode_delivery_ingestion);
  g_test_add_func ("/daemon-api/capnp/codec/encode-delivery-ingestion-valid",
      assert_delivery_ingestion_request_encoder_decodes_valid_request);
  g_test_add_func
      ("/daemon-api/capnp/codec/encode-delivery-ingestion-missing-optionals",
      assert_delivery_ingestion_request_encoder_decodes_missing_optionals);
  g_test_add_func
      ("/daemon-api/capnp/codec/reject-delivery-ingestion-encode-invalid-input",
      assert_delivery_ingestion_request_encoder_rejects_invalid_input);
  g_test_add_func
      ("/daemon-api/capnp/codec/reject-delivery-ingestion-missing-delivery-id",
      assert_request_bytes_rejects_delivery_ingestion_missing_delivery_id);
  g_test_add_func
      ("/daemon-api/capnp/codec/reject-delivery-ingestion-control-character",
      assert_request_bytes_rejects_delivery_ingestion_control_character);
  g_test_add_func
      ("/daemon-api/capnp/codec/reject-delivery-ingestion-missing-recipients",
      assert_request_bytes_rejects_delivery_ingestion_missing_recipients);
  g_test_add_func
      ("/daemon-api/capnp/codec/reject-delivery-ingestion-empty-message-bytes",
      assert_request_bytes_rejects_delivery_ingestion_empty_message_bytes);
  g_test_add_func ("/daemon-api/capnp/codec/encode-mailbox-select-valid-id",
      assert_mailbox_select_request_encoder_round_trip_by_id);
  g_test_add_func ("/daemon-api/capnp/codec/encode-mailbox-select-valid-name",
      assert_mailbox_select_request_encoder_round_trip_by_name);
  g_test_add_func
      ("/daemon-api/capnp/codec/reject-mailbox-select-encode-invalid-input",
      assert_mailbox_select_request_encoder_rejects_invalid_input);
  g_test_add_func ("/daemon-api/capnp/codec/encode-mailbox-list-request",
      assert_mailbox_list_request_encoder_round_trip);
  g_test_add_func
      ("/daemon-api/capnp/codec/reject-mailbox-list-encode-invalid-input",
      assert_mailbox_list_request_encoder_rejects_invalid_input);
  g_test_add_func ("/daemon-api/capnp/codec/encode-message-fetch-valid",
      assert_message_fetch_request_encoder_round_trip);
  g_test_add_func
      ("/daemon-api/capnp/codec/reject-message-fetch-encode-invalid-input",
      assert_message_fetch_request_encoder_rejects_invalid_input);
  g_test_add_func ("/daemon-api/capnp/codec/decode-flag-keyword-update-set",
      assert_request_bytes_decode_flag_keyword_update_set);
  g_test_add_func
      ("/daemon-api/capnp/codec/decode-flag-keyword-update-mode-mapping",
      assert_request_bytes_decode_flag_keyword_update_mode_mapping);
  g_test_add_func
      ("/daemon-api/capnp/codec/decode-flag-keyword-update-replace-empty",
      assert_request_bytes_decode_flag_keyword_update_replace_empty_payload);
  g_test_add_func
      ("/daemon-api/capnp/codec/reject-flag-keyword-update-invalid-mode",
      assert_request_bytes_rejects_flag_keyword_update_invalid_mode);
  g_test_add_func
      ("/daemon-api/capnp/codec/reject-flag-keyword-update-missing-payload",
      assert_request_bytes_rejects_flag_keyword_update_missing_payload_for_set);
  g_test_add_func
      ("/daemon-api/capnp/codec/reject-flag-keyword-update-missing-account",
      assert_request_bytes_rejects_flag_keyword_update_missing_account);
  g_test_add_func
      ("/daemon-api/capnp/codec/reject-flag-keyword-update-missing-mailbox",
      assert_request_bytes_rejects_flag_keyword_update_missing_mailbox);
  g_test_add_func
      ("/daemon-api/capnp/codec/reject-flag-keyword-update-zero-uid-validity",
      assert_request_bytes_rejects_flag_keyword_update_zero_uid_validity);
  g_test_add_func
      ("/daemon-api/capnp/codec/reject-flag-keyword-update-zero-mailbox-uid",
      assert_request_bytes_rejects_flag_keyword_update_zero_mailbox_uid);
  g_test_add_func
      ("/daemon-api/capnp/codec/reject-flag-keyword-update-unknown-system-flag",
      assert_request_bytes_rejects_flag_keyword_update_unknown_system_flag);
  g_test_add_func
      ("/daemon-api/capnp/codec/reject-flag-keyword-update-recent-system-flag",
      assert_request_bytes_rejects_flag_keyword_update_recent_system_flag);
  g_test_add_func
      ("/daemon-api/capnp/codec/reject-flag-keyword-update-system-flag-keyword",
      assert_request_bytes_rejects_flag_keyword_update_system_flag_keyword);
  g_test_add_func
      ("/daemon-api/capnp/codec/reject-flag-keyword-update-invalid-keyword",
      assert_request_bytes_rejects_flag_keyword_update_invalid_keyword);
  g_test_add_func
      ("/daemon-api/capnp/codec/reject-flag-keyword-update-duplicate-flag",
      assert_request_bytes_rejects_flag_keyword_update_duplicate_flag);
  g_test_add_func
      ("/daemon-api/capnp/codec/reject-flag-keyword-update-duplicate-keyword",
      assert_request_bytes_rejects_flag_keyword_update_duplicate_keyword);
  g_test_add_func ("/daemon-api/capnp/codec/reject-message-fetch-empty-account",
      assert_request_bytes_rejects_message_fetch_missing_account_identity);
  g_test_add_func ("/daemon-api/capnp/codec/reject-message-fetch-empty-mailbox",
      assert_request_bytes_rejects_message_fetch_missing_mailbox_id);
  g_test_add_func
      ("/daemon-api/capnp/codec/reject-message-fetch-zero-uid-validity",
      assert_request_bytes_rejects_message_fetch_zero_uid_validity);
  g_test_add_func
      ("/daemon-api/capnp/codec/reject-message-fetch-zero-mailbox-uid",
      assert_request_bytes_rejects_message_fetch_zero_mailbox_uid);
  g_test_add_func ("/daemon-api/capnp/codec/reject-fact-mutation-predicate",
      assert_fact_mutation_decode_rejects_missing_predicate);
  g_test_add_func ("/daemon-api/capnp/codec/reject-fact-mutation-scope",
      assert_fact_mutation_decode_rejects_missing_scope);
  g_test_add_func ("/daemon-api/capnp/codec/reject-fact-mutation-argument",
      assert_fact_mutation_decode_rejects_empty_argument);
  g_test_add_func ("/daemon-api/capnp/codec/reject-fact-batch-import-empty",
      assert_fact_batch_import_decode_rejects_empty_batch);
  g_test_add_func
      ("/daemon-api/capnp/codec/reject-fact-batch-import-over-limit",
      assert_fact_batch_import_decode_rejects_over_limit_batch);
  g_test_add_func
      ("/daemon-api/capnp/codec/reject-fact-batch-import-invalid-nested",
      assert_fact_batch_import_decode_rejects_invalid_nested_mutation);
  g_test_add_func
      ("/daemon-api/capnp/codec/reject-fact-batch-import-mixed-scope",
      assert_fact_batch_import_decode_rejects_mixed_scope);
  g_test_add_func ("/daemon-api/capnp/codec/encode-mailbox-list",
      assert_response_bytes_encode_mailbox_list);
  g_test_add_func ("/daemon-api/capnp/codec/encode-mailbox-select",
      assert_response_bytes_encode_mailbox_select);
  g_test_add_func ("/daemon-api/capnp/codec/decode-mailbox-select-response",
      assert_response_bytes_decode_mailbox_select_roundtrip);
  g_test_add_func ("/daemon-api/capnp/codec/decode-mailbox-list-response",
      assert_response_bytes_decode_mailbox_list_roundtrip);
  g_test_add_func
      ("/daemon-api/capnp/codec/reject-mailbox-select-response-missing-payload-request-id",
      assert_response_bytes_rejects_mailbox_select_missing_payload_request_id);
  g_test_add_func
      ("/daemon-api/capnp/codec/reject-mailbox-select-response-mismatched-request-id",
      assert_response_bytes_rejects_mailbox_select_mismatched_payload_request_id);
  g_test_add_func
      ("/daemon-api/capnp/codec/reject-mailbox-select-response-invalid-fields",
      assert_response_bytes_rejects_mailbox_select_invalid_fields);
  g_test_add_func ("/daemon-api/capnp/codec/encode-stream-chunk",
      assert_response_bytes_encode_stream_chunk);
  g_test_add_func ("/daemon-api/capnp/codec/reject-ambiguous-stream-chunk",
      assert_response_bytes_reject_ambiguous_stream_chunk);
  g_test_add_func
      ("/daemon-api/capnp/codec/reject-mismatched-stream-chunk-request",
      assert_response_bytes_reject_mismatched_stream_chunk_request);
  g_test_add_func ("/daemon-api/capnp/codec/encode-final-empty-stream-chunk",
      assert_response_bytes_encode_final_empty_stream_chunk);
  g_test_add_func ("/daemon-api/capnp/codec/encode-error",
      assert_response_bytes_encode_error);
  g_test_add_func ("/daemon-api/capnp/codec/encode-success",
      assert_response_bytes_encode_success);
  g_test_add_func ("/daemon-api/capnp/codec/decode-success-response",
      assert_response_bytes_decode_success_roundtrip);
  g_test_add_func ("/daemon-api/capnp/codec/decode-error-response",
      assert_response_bytes_decode_error_roundtrip);
  g_test_add_func ("/daemon-api/capnp/codec/reject-malformed-response",
      assert_response_bytes_decode_rejects_malformed);
  g_test_add_func
      ("/daemon-api/capnp/codec/reject-mailbox-list-response-mismatched-request-id",
      assert_response_bytes_rejects_mailbox_list_mismatched_payload_request_id);
  g_test_add_func ("/daemon-api/capnp/codec/request-adapter-compatible",
      assert_request_adapter_callbacks_are_usable);
  g_test_add_func ("/daemon-api/capnp/codec/request-adapter-mailbox-select",
      assert_request_adapter_routes_mailbox_select);
  g_test_add_func ("/daemon-api/capnp/codec/request-adapter-fact-mutation",
      assert_request_adapter_routes_fact_mutation);
  g_test_add_func ("/daemon-api/capnp/codec/request-adapter-fact-batch-import",
      assert_request_adapter_routes_fact_batch_import);
  g_test_add_func
      ("/daemon-api/capnp/codec/request-adapter-fact-batch-import-unauthorized",
      assert_request_adapter_rejects_unauthorized_fact_batch_import);
  g_test_add_func ("/daemon-api/capnp/codec/request-adapter-message-fetch",
      assert_request_adapter_routes_message_fetch);
  g_test_add_func ("/daemon-api/capnp/codec/request-adapter-message-search",
      assert_request_adapter_routes_message_search);
  g_test_add_func
      ("/daemon-api/capnp/codec/request-adapter-wirelog-predicate-query",
      assert_request_adapter_routes_wirelog_predicate_query);
  g_test_add_func
      ("/daemon-api/capnp/codec/request-adapter-duckdb-query-template",
      assert_request_adapter_routes_duckdb_query_template);
  g_test_add_func ("/daemon-api/capnp/codec/request-adapter-delivery-ingestion",
      assert_request_adapter_routes_delivery_ingestion);
  g_test_add_func
      ("/daemon-api/capnp/codec/request-adapter-flag-keyword-update",
      assert_request_adapter_routes_flag_keyword_update);

  return g_test_run ();
}
