#include "wyrebox-daemon-capnp-codec.h"

#include "wyrebox-daemon-error-frame.h"
#include "wyrebox-daemon-delivery-ingestion-request.h"
#include "wyrebox-daemon-fact-mutation-request.h"
#include "wyrebox-daemon-flag-keyword-update-request.h"
#include "wyrebox-daemon-mailbox-list-request.h"
#include "wyrebox-daemon-mailbox-list-result.h"
#include "wyrebox-daemon-mailbox-select-request.h"
#include "wyrebox-daemon-mailbox-select-result.h"
#include "wyrebox-daemon-message-fetch-request.h"
#include "wyrebox-daemon-message-search-request.h"

#include <capnp/message.h>
#include <capnp/serialize.h>

#include "wyrebox-daemon-api.capnp.h"

#include <glib.h>

#include <exception>

typedef struct
{
  char *request_id;
  char *caller_identity;
  char *account_identity;
  char *tool_identity;
  char *correlation_id;

  WyreboxDaemonMailboxListRequest mailbox_list;
  WyreboxDaemonMailboxSelectRequest mailbox_select;
  WyreboxDaemonFactMutationRequest fact_mutation;
  WyreboxDaemonMessageFetchRequest message_fetch;
  WyreboxDaemonMessageSearchRequest message_search;
  WyreboxDaemonDeliveryIngestionRequest delivery_ingestion;
  WyreboxDaemonFlagKeywordUpdateRequest flag_keyword_update;
} WyreboxDaemonCapnpDecodedRequestState;

static gboolean
set_invalid_argument (GError **error, const char *message)
{
  g_set_error (error,
      G_IO_ERROR,
      G_IO_ERROR_INVALID_ARGUMENT,
      "%s",
      message);
  return FALSE;
}

static gboolean
set_not_supported (GError **error, const char *message)
{
  g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "%s", message);
  return FALSE;
}

void
wyrebox_daemon_capnp_codec_decoded_state_clear (gpointer decoded_state)
{
  WyreboxDaemonCapnpDecodedRequestState *state =
      static_cast<WyreboxDaemonCapnpDecodedRequestState *> (decoded_state);

  if (state == NULL)
    return;

  g_clear_pointer (&state->request_id, g_free);
  g_clear_pointer (&state->caller_identity, g_free);
  g_clear_pointer (&state->account_identity, g_free);
  g_clear_pointer (&state->tool_identity, g_free);
  g_clear_pointer (&state->correlation_id, g_free);
  wyrebox_daemon_mailbox_list_request_clear (&state->mailbox_list);
  wyrebox_daemon_mailbox_select_request_clear (&state->mailbox_select);
  wyrebox_daemon_fact_mutation_request_clear (&state->fact_mutation);
  wyrebox_daemon_message_fetch_request_clear (&state->message_fetch);
  wyrebox_daemon_message_search_request_clear (&state->message_search);
  wyrebox_daemon_delivery_ingestion_request_clear (&state->delivery_ingestion);
  wyrebox_daemon_flag_keyword_update_request_clear (&state->flag_keyword_update);

  g_free (state);
}

static gboolean
map_fact_mutation_kind (FactMutationKind in,
    WyreboxDaemonFactMutationKind *out)
{
  switch (in) {
    case FactMutationKind::INSERT:
      *out = WYREBOX_DAEMON_FACT_MUTATION_INSERT;
      return TRUE;
    case FactMutationKind::RETRACT:
      *out = WYREBOX_DAEMON_FACT_MUTATION_RETRACT;
      return TRUE;
    default:
      return FALSE;
  }
}

static gboolean
map_mailbox_list_child_state (WyreboxDaemonMailboxListChildState in,
    MailboxListChildState *out)
{
  switch (in) {
    case WYREBOX_DAEMON_MAILBOX_LIST_CHILD_STATE_UNKNOWN:
      *out = MailboxListChildState::UNKNOWN;
      return TRUE;
    case WYREBOX_DAEMON_MAILBOX_LIST_CHILD_STATE_HAS_CHILDREN:
      *out = MailboxListChildState::HAS_CHILDREN;
      return TRUE;
    case WYREBOX_DAEMON_MAILBOX_LIST_CHILD_STATE_HAS_NO_CHILDREN:
      *out = MailboxListChildState::HAS_NO_CHILDREN;
      return TRUE;
    default:
      return FALSE;
  }
}

static gboolean
map_mailbox_list_entry_kind (WyreboxDaemonMailboxListEntryKind in,
    MailboxListEntryKind *out)
{
  switch (in) {
    case WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_ORDINARY:
      *out = MailboxListEntryKind::ORDINARY;
      return TRUE;
    case WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_VIRTUAL:
      *out = MailboxListEntryKind::VIRTUAL;
      return TRUE;
    default:
      return FALSE;
  }
}

static gboolean
map_error_class (WyreboxDaemonErrorClass in, ErrorClass *out)
{
  switch (in) {
    case WYREBOX_DAEMON_ERROR_TEMPORARY_FAILURE:
      *out = ErrorClass::TEMPORARY_FAILURE;
      return TRUE;
    case WYREBOX_DAEMON_ERROR_PERMANENT_FAILURE:
      *out = ErrorClass::PERMANENT_FAILURE;
      return TRUE;
    case WYREBOX_DAEMON_ERROR_PERMISSION_DENIED:
      *out = ErrorClass::PERMISSION_DENIED;
      return TRUE;
    case WYREBOX_DAEMON_ERROR_NOT_FOUND:
      *out = ErrorClass::NOT_FOUND;
      return TRUE;
    case WYREBOX_DAEMON_ERROR_CONFLICT:
      *out = ErrorClass::CONFLICT;
      return TRUE;
    case WYREBOX_DAEMON_ERROR_INTERNAL_ERROR:
      *out = ErrorClass::INTERNAL_ERROR;
      return TRUE;
    default:
      return FALSE;
  }
}

static gboolean
map_flag_keyword_update_mode (FlagKeywordUpdateMode in,
    WyreboxDaemonFlagKeywordUpdateMode *out)
{
  switch (in) {
    case FlagKeywordUpdateMode::SET:
      *out = WYREBOX_DAEMON_FLAG_KEYWORD_UPDATE_MODE_SET;
      return TRUE;
    case FlagKeywordUpdateMode::CLEAR:
      *out = WYREBOX_DAEMON_FLAG_KEYWORD_UPDATE_MODE_CLEAR;
      return TRUE;
    case FlagKeywordUpdateMode::REPLACE:
      *out = WYREBOX_DAEMON_FLAG_KEYWORD_UPDATE_MODE_REPLACE;
      return TRUE;
    default:
      return FALSE;
  }
}

static gboolean
decode_strv (const capnp::List<capnp::Text>::Reader &value_list,
    gchar ***out_values)
{
  g_auto (GStrv) copied = NULL;
  gsize value_count = value_list.size ();

  g_return_val_if_fail (out_values != NULL, FALSE);

  *out_values = NULL;

  if (value_count == 0)
    return TRUE;

  copied = g_new0 (gchar *, value_count + 1);
  for (gsize i = 0; i < value_count; i++)
    copied[i] = g_strdup (value_list[i].cStr ());

  *out_values = g_steal_pointer (&copied);
  return TRUE;
}

static gboolean
decode_request_identity (const RequestFrame::Reader &request_frame,
    WyreboxDaemonCapnpDecodedRequestState *state,
    GError **error)
{
  auto identity = request_frame.getIdentity ();
  const char *request_id = identity.getRequestId ().cStr ();

  if (request_id == NULL || *request_id == '\0')
    return set_invalid_argument (error,
        "request frame identity.request_id is required");

  state->request_id = g_strdup (request_id);
  state->caller_identity = g_strdup (identity.getCallerIdentity ().cStr ());
  state->account_identity = g_strdup (identity.getAccountIdentity ().cStr ());
  state->tool_identity = g_strdup (identity.getToolIdentity ().cStr ());
  state->correlation_id = g_strdup (identity.getCorrelationId ().cStr ());

  return TRUE;
}

static gboolean
decode_mailbox_list_request (const RequestFrame::Reader &request_frame,
    WyreboxDaemonCapnpDecodedRequestState *state,
    WyreboxDaemonDecodedRequestFrame *out_request_frame,
    GError **error)
{
  auto mailbox_list = request_frame.getMailboxList ();

  if (!decode_request_identity (request_frame, state, error))
    return FALSE;

  if (state->account_identity == NULL || *state->account_identity == '\0')
    return set_invalid_argument (error,
        "request frame mailboxList.accountIdentity is required");

  if (!wyrebox_daemon_mailbox_list_request_init (&state->mailbox_list,
          mailbox_list.getAccountIdentity ().cStr (),
          mailbox_list.getNamespacePrefix ().cStr (), error))
    return FALSE;

  out_request_frame->request_id = state->request_id;
  out_request_frame->caller_identity = state->caller_identity;
  out_request_frame->account_identity = state->account_identity;
  out_request_frame->tool_identity = state->tool_identity;
  out_request_frame->correlation_id = state->correlation_id;
  out_request_frame->operation = WYREBOX_DAEMON_REQUEST_FRAME_OPERATION_MAILBOX_LIST;
  out_request_frame->mailbox_list = &state->mailbox_list;
  out_request_frame->mailbox_select = NULL;
  out_request_frame->fact_mutation = NULL;
  out_request_frame->message_fetch = NULL;
  out_request_frame->message_search = NULL;
  out_request_frame->wirelog_predicate_query = NULL;
  out_request_frame->flag_keyword_update = NULL;

  return TRUE;
}

static gboolean
decode_mailbox_select_request (const RequestFrame::Reader &request_frame,
    WyreboxDaemonCapnpDecodedRequestState *state,
    WyreboxDaemonDecodedRequestFrame *out_request_frame,
    GError **error)
{
  auto mailbox_select = request_frame.getMailboxSelect ();

  if (!decode_request_identity (request_frame, state, error))
    return FALSE;

  if (!wyrebox_daemon_mailbox_select_request_init (&state->mailbox_select,
          mailbox_select.getAccountIdentity ().cStr (),
          mailbox_select.getMailboxId ().cStr (),
          mailbox_select.getMailboxName ().cStr (),
          error))
    return FALSE;

  out_request_frame->request_id = state->request_id;
  out_request_frame->caller_identity = state->caller_identity;
  out_request_frame->account_identity = state->account_identity;
  out_request_frame->tool_identity = state->tool_identity;
  out_request_frame->correlation_id = state->correlation_id;
  out_request_frame->operation =
      WYREBOX_DAEMON_REQUEST_FRAME_OPERATION_MAILBOX_SELECT;
  out_request_frame->mailbox_list = NULL;
  out_request_frame->mailbox_select = &state->mailbox_select;
  out_request_frame->fact_mutation = NULL;
  out_request_frame->message_fetch = NULL;
  out_request_frame->message_search = NULL;
  out_request_frame->wirelog_predicate_query = NULL;
  out_request_frame->flag_keyword_update = NULL;

  return TRUE;
}

static gboolean
decode_fact_mutation_request (const RequestFrame::Reader &request_frame,
    WyreboxDaemonCapnpDecodedRequestState *state,
    WyreboxDaemonDecodedRequestFrame *out_request_frame,
    GError **error)
{
  auto fact_mutation = request_frame.getFactMutation ();
  auto arguments = fact_mutation.getArguments ();
  WyreboxDaemonFactMutationKind mutation =
      WYREBOX_DAEMON_FACT_MUTATION_INSERT;
  g_auto (GStrv) argument_vector = NULL;

  if (!decode_request_identity (request_frame, state, error))
    return FALSE;

  if (!map_fact_mutation_kind (fact_mutation.getMutation (), &mutation))
    return set_invalid_argument (error, "unsupported fact mutation kind");

  argument_vector = g_new0 (char *, arguments.size () + 1);
  for (guint i = 0; i < arguments.size (); i++)
    argument_vector[i] = g_strdup (arguments[i].cStr ());

  if (!wyrebox_daemon_fact_mutation_request_init (&state->fact_mutation,
          mutation,
          fact_mutation.getPredicateId ().cStr (),
          fact_mutation.getScopeId ().cStr (),
          (const char * const *) argument_vector,
          error))
    return FALSE;

  out_request_frame->request_id = state->request_id;
  out_request_frame->caller_identity = state->caller_identity;
  out_request_frame->account_identity = state->account_identity;
  out_request_frame->tool_identity = state->tool_identity;
  out_request_frame->correlation_id = state->correlation_id;
  out_request_frame->operation =
      WYREBOX_DAEMON_REQUEST_FRAME_OPERATION_FACT_MUTATION;
  out_request_frame->mailbox_list = NULL;
  out_request_frame->mailbox_select = NULL;
  out_request_frame->fact_mutation = &state->fact_mutation;
  out_request_frame->message_fetch = NULL;
  out_request_frame->message_search = NULL;
  out_request_frame->wirelog_predicate_query = NULL;
  out_request_frame->flag_keyword_update = NULL;

  return TRUE;
}

static gboolean
decode_message_fetch_request (const RequestFrame::Reader &request_frame,
    WyreboxDaemonCapnpDecodedRequestState *state,
    WyreboxDaemonDecodedRequestFrame *out_request_frame,
    GError **error)
{
  auto message_fetch = request_frame.getMessageFetch ();

  if (!decode_request_identity (request_frame, state, error))
    return FALSE;

  if (!wyrebox_daemon_message_fetch_request_init (&state->message_fetch,
          message_fetch.getAccountIdentity ().cStr (),
          message_fetch.getMailboxId ().cStr (),
          message_fetch.getUidValidity (),
          message_fetch.getMailboxUid (),
          error))
    return FALSE;

  out_request_frame->request_id = state->request_id;
  out_request_frame->caller_identity = state->caller_identity;
  out_request_frame->account_identity = state->account_identity;
  out_request_frame->tool_identity = state->tool_identity;
  out_request_frame->correlation_id = state->correlation_id;
  out_request_frame->operation =
      WYREBOX_DAEMON_REQUEST_FRAME_OPERATION_MESSAGE_FETCH;
  out_request_frame->mailbox_list = NULL;
  out_request_frame->mailbox_select = NULL;
  out_request_frame->fact_mutation = NULL;
  out_request_frame->message_fetch = &state->message_fetch;
  out_request_frame->message_search = NULL;
  out_request_frame->wirelog_predicate_query = NULL;
  out_request_frame->flag_keyword_update = NULL;

  return TRUE;
}

static gboolean
decode_message_search_request (const RequestFrame::Reader &request_frame,
    WyreboxDaemonCapnpDecodedRequestState *state,
    WyreboxDaemonDecodedRequestFrame *out_request_frame,
    GError **error)
{
  auto message_search = request_frame.getMessageSearch ();

  if (!decode_request_identity (request_frame, state, error))
    return FALSE;

  if (!wyrebox_daemon_message_search_request_init (&state->message_search,
          message_search.getAccountIdentity ().cStr (),
          message_search.getMailboxId ().cStr (),
          message_search.getUidValidity (),
          message_search.getCriteriaToken ().cStr (),
          error))
    return FALSE;

  out_request_frame->request_id = state->request_id;
  out_request_frame->caller_identity = state->caller_identity;
  out_request_frame->account_identity = state->account_identity;
  out_request_frame->tool_identity = state->tool_identity;
  out_request_frame->correlation_id = state->correlation_id;
  out_request_frame->operation =
      WYREBOX_DAEMON_REQUEST_FRAME_OPERATION_MESSAGE_SEARCH;
  out_request_frame->mailbox_list = NULL;
  out_request_frame->mailbox_select = NULL;
  out_request_frame->fact_mutation = NULL;
  out_request_frame->message_fetch = NULL;
  out_request_frame->message_search = &state->message_search;
  out_request_frame->wirelog_predicate_query = NULL;
  out_request_frame->flag_keyword_update = NULL;

  return TRUE;
}

static gboolean
decode_delivery_ingestion_request (const RequestFrame::Reader &request_frame,
    WyreboxDaemonCapnpDecodedRequestState *state,
    WyreboxDaemonDecodedRequestFrame *out_request_frame,
    GError **error)
{
  auto delivery_ingestion = request_frame.getDeliveryIngestion ();
  auto message_bytes = delivery_ingestion.getMessageBytes ();
  g_auto (GStrv) recipients = NULL;
  g_autoptr (GBytes) decoded_message_bytes = NULL;

  if (!decode_request_identity (request_frame, state, error))
    return FALSE;

  if (!decode_strv (delivery_ingestion.getRecipients (), &recipients))
    return FALSE;

  decoded_message_bytes =
      g_bytes_new (message_bytes.begin (), message_bytes.size ());

  if (!wyrebox_daemon_delivery_ingestion_request_init (&state->delivery_ingestion,
          delivery_ingestion.getDeliveryId ().cStr (),
          delivery_ingestion.getQueueId ().cStr (),
          delivery_ingestion.getEnvelopeSender ().cStr (),
          (const gchar * const *) recipients,
          decoded_message_bytes,
          error))
    return FALSE;

  out_request_frame->request_id = state->request_id;
  out_request_frame->caller_identity = state->caller_identity;
  out_request_frame->account_identity = state->account_identity;
  out_request_frame->tool_identity = state->tool_identity;
  out_request_frame->correlation_id = state->correlation_id;
  out_request_frame->operation =
      WYREBOX_DAEMON_REQUEST_FRAME_OPERATION_DELIVERY_INGESTION;
  out_request_frame->mailbox_list = NULL;
  out_request_frame->mailbox_select = NULL;
  out_request_frame->fact_mutation = NULL;
  out_request_frame->message_fetch = NULL;
  out_request_frame->message_search = NULL;
  out_request_frame->wirelog_predicate_query = NULL;
  out_request_frame->delivery_ingestion = &state->delivery_ingestion;
  out_request_frame->flag_keyword_update = NULL;

  return TRUE;
}

static gboolean
decode_flag_keyword_update_request (
    const RequestFrame::Reader &request_frame,
    WyreboxDaemonCapnpDecodedRequestState *state,
    WyreboxDaemonDecodedRequestFrame *out_request_frame,
    GError **error)
{
  auto flag_keyword_update = request_frame.getFlagKeywordUpdate ();
  WyreboxDaemonFlagKeywordUpdateMode c_mode =
      WYREBOX_DAEMON_FLAG_KEYWORD_UPDATE_MODE_SET;
  g_auto (GStrv) system_flags = NULL;
  g_auto (GStrv) user_keywords = NULL;

  if (!decode_request_identity (request_frame, state, error))
    return FALSE;

  if (!map_flag_keyword_update_mode (flag_keyword_update.getMode (), &c_mode))
    return set_invalid_argument (error,
        "unsupported flag keyword update mode");

  if (!decode_strv (flag_keyword_update.getSystemFlags (), &system_flags))
    return FALSE;

  if (!decode_strv (flag_keyword_update.getUserKeywords (), &user_keywords))
    return FALSE;

  if (!wyrebox_daemon_flag_keyword_update_request_init (&state->flag_keyword_update,
          flag_keyword_update.getAccountIdentity ().cStr (),
          flag_keyword_update.getMailboxId ().cStr (),
          flag_keyword_update.getUidValidity (),
          flag_keyword_update.getMailboxUid (),
          c_mode,
          (const char * const *) system_flags,
          (const char * const *) user_keywords,
          error))
    return FALSE;

  out_request_frame->request_id = state->request_id;
  out_request_frame->caller_identity = state->caller_identity;
  out_request_frame->account_identity = state->account_identity;
  out_request_frame->tool_identity = state->tool_identity;
  out_request_frame->correlation_id = state->correlation_id;
  out_request_frame->operation =
      WYREBOX_DAEMON_REQUEST_FRAME_OPERATION_FLAG_KEYWORD_UPDATE;
  out_request_frame->mailbox_list = NULL;
  out_request_frame->mailbox_select = NULL;
  out_request_frame->fact_mutation = NULL;
  out_request_frame->message_fetch = NULL;
  out_request_frame->message_search = NULL;
  out_request_frame->wirelog_predicate_query = NULL;
  out_request_frame->flag_keyword_update = &state->flag_keyword_update;

  return TRUE;
}

static gboolean
decode_request_frame (const capnp::word *words,
    gsize word_count,
    WyreboxDaemonCapnpDecodedRequestState *state,
    WyreboxDaemonDecodedRequestFrame *out_request_frame,
    GError **error)
{
  try {
    capnp::FlatArrayMessageReader request_reader (
        kj::ArrayPtr<const capnp::word> (words, word_count));
    auto request_frame = request_reader.getRoot<RequestFrame> ();

    switch (request_frame.which ()) {
      case RequestFrame::MAILBOX_LIST:
        return decode_mailbox_list_request (request_frame,
            state,
            out_request_frame,
            error);
      case RequestFrame::DELIVERY_INGESTION:
        return decode_delivery_ingestion_request (request_frame,
            state,
            out_request_frame,
            error);
      case RequestFrame::MAILBOX_SELECT:
        return decode_mailbox_select_request (request_frame,
            state,
            out_request_frame,
            error);
      case RequestFrame::MESSAGE_FETCH:
        return decode_message_fetch_request (request_frame,
            state,
            out_request_frame,
            error);
      case RequestFrame::MESSAGE_SEARCH:
        return decode_message_search_request (request_frame,
            state,
            out_request_frame,
            error);
      case RequestFrame::FLAG_KEYWORD_UPDATE:
        return decode_flag_keyword_update_request (request_frame,
            state,
            out_request_frame,
            error);
      case RequestFrame::FACT_MUTATION:
        return decode_fact_mutation_request (request_frame,
            state,
            out_request_frame,
            error);
      case RequestFrame::WIRELOG_PREDICATE_QUERY:
        return set_not_supported (error,
            "unsupported request frame: WirelogPredicateQuery");
      case RequestFrame::DUCK_D_B_QUERY_TEMPLATE:
        return set_not_supported (error,
            "unsupported request frame: DuckDBQueryTemplate");
      default:
        return set_not_supported (error, "unsupported request frame union arm");
    }
  } catch (const std::exception &e) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA,
        "request frame decode failed: %s",
        e.what ());
  }

  return FALSE;
}

static gboolean
encode_success_response (const WyreboxDaemonResponseFrame *response_frame,
    GBytes **out_bytes,
    GError **error)
{
  try {
    if (response_frame->request_id == NULL || *response_frame->request_id == '\0')
      return set_invalid_argument (error, "response frame request_id is required");

    if (response_frame->success.durable_marker == NULL ||
        *response_frame->success.durable_marker == '\0')
      return set_invalid_argument (error,
          "success response durable marker is required");

    if (response_frame->success.summary == NULL ||
        *response_frame->success.summary == '\0')
      return set_invalid_argument (error,
          "success response summary is required");

    capnp::MallocMessageBuilder response_builder;
    auto response_frame_message = response_builder.initRoot<ResponseFrame> ();
    auto response_success = response_frame_message.initSuccess ();

    response_frame_message.setRequestId (response_frame->request_id);
    response_frame_message.setCorrelationId (
        response_frame->correlation_id != NULL ? response_frame->correlation_id : "");

    response_success.setRequestId (
        response_frame->success.request_id != NULL &&
            *response_frame->success.request_id != '\0'
            ? response_frame->success.request_id
            : response_frame->request_id);
    response_success.setDurableMarker (response_frame->success.durable_marker);
    response_success.setJournalOffset (response_frame->success.journal_offset);
    response_success.setSummary (response_frame->success.summary);
    response_success.setJournalSequence (response_frame->success.journal_sequence);

    auto words = capnp::messageToFlatArray (response_builder);
    auto bytes = words.asBytes ();
    *out_bytes = g_bytes_new (bytes.begin (), bytes.size ());

    return TRUE;
  } catch (const std::exception &e) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA,
        "success response encode failed: %s",
        e.what ());
  }

  return FALSE;
}

static gboolean
encode_mailbox_list_response (const WyreboxDaemonResponseFrame *response_frame,
    GBytes **out_bytes,
    GError **error)
{
  try {
    auto *result = response_frame->mailbox_list.entries;

    if (response_frame->request_id == NULL || *response_frame->request_id == '\0')
      return set_invalid_argument (error, "response frame request_id is required");

    if (result == NULL)
      return set_invalid_argument (error,
          "mailbox LIST response requires result entries");

    capnp::MallocMessageBuilder response_builder;
    auto response_frame_message = response_builder.initRoot<ResponseFrame> ();
    auto response_list = response_frame_message.initMailboxList ();

    response_frame_message.setRequestId (response_frame->request_id);
    response_frame_message.setCorrelationId (
        response_frame->correlation_id != NULL ? response_frame->correlation_id : "");

    response_list.setRequestId (response_frame->request_id);
    auto entries = response_list.initEntries (result->len);

    for (guint i = 0; i < result->len; i++) {
      const WyreboxDaemonMailboxListEntry *entry =
          static_cast<const WyreboxDaemonMailboxListEntry *> (result->pdata[i]);
      auto encoded_entry = entries[i];
      MailboxListEntryKind encoded_kind = MailboxListEntryKind::ORDINARY;
      MailboxListChildState encoded_child_state = MailboxListChildState::UNKNOWN;

      if (!map_mailbox_list_entry_kind (entry->kind, &encoded_kind)
          || !map_mailbox_list_child_state (entry->child_state, &encoded_child_state))
        return set_invalid_argument (error,
            "unsupported mailbox list entry enum value");

      encoded_entry.setKind (encoded_kind);
      encoded_entry.setMailboxId (entry->mailbox_id != NULL ? entry->mailbox_id : "");
      encoded_entry.setMailboxName (entry->mailbox_name != NULL
          ? entry->mailbox_name
          : "");
      encoded_entry.setHierarchyDelimiter (entry->hierarchy_delimiter != NULL
          ? entry->hierarchy_delimiter
          : "");
      encoded_entry.setSpecialUse (entry->special_use != NULL
          ? entry->special_use
          : "");
      encoded_entry.setSelectable (entry->is_selectable);
      encoded_entry.setChildState (encoded_child_state);
    }

    auto words = capnp::messageToFlatArray (response_builder);
    auto bytes = words.asBytes ();
    *out_bytes = g_bytes_new (bytes.begin (), bytes.size ());

    return TRUE;
  } catch (const std::exception &e) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA,
        "mailbox LIST response encode failed: %s",
        e.what ());
  }

  return FALSE;
}

static gboolean
encode_stream_chunk_response (const WyreboxDaemonResponseFrame *response_frame,
    GBytes **out_bytes,
    GError **error)
{
  try {
    const WyreboxDaemonStreamChunkFrame *chunk =
        &response_frame->stream_chunk;
    const guint8 *chunk_bytes = NULL;
    gsize chunk_size = 0;

    if (response_frame->request_id == NULL || *response_frame->request_id == '\0')
      return set_invalid_argument (error, "response frame request_id is required");

    if (chunk->request_id == NULL || *chunk->request_id == '\0')
      return set_invalid_argument (error,
          "stream chunk response request_id is required");

    if (g_strcmp0 (response_frame->request_id, chunk->request_id) != 0)
      return set_invalid_argument (error,
          "stream chunk response request_id does not match frame envelope");

    const bool has_message_id =
        chunk->message_id != NULL && *chunk->message_id != '\0';
    const bool has_query_id =
        chunk->query_id != NULL && *chunk->query_id != '\0';

    if (has_message_id == has_query_id)
      return set_invalid_argument (error,
          "stream chunk response requires exactly one of message_id or query_id");

    if (chunk->bytes != NULL)
      chunk_bytes = static_cast<const guint8 *> (
          g_bytes_get_data (chunk->bytes, &chunk_size));

    if (!chunk->end_of_stream && chunk_size == 0)
      return set_invalid_argument (error,
          "non-final stream chunk response requires bytes");

    capnp::MallocMessageBuilder response_builder;
    auto response_frame_message = response_builder.initRoot<ResponseFrame> ();
    auto response_chunk = response_frame_message.initStreamChunk ();

    response_frame_message.setRequestId (response_frame->request_id);
    response_frame_message.setCorrelationId (
        response_frame->correlation_id != NULL ? response_frame->correlation_id : "");

    response_chunk.setRequestId (chunk->request_id);
    response_chunk.setMessageId (
        chunk->message_id != NULL ? chunk->message_id : "");
    response_chunk.setQueryId (chunk->query_id != NULL ? chunk->query_id : "");
    response_chunk.setCorrelationId (
        chunk->correlation_id != NULL ? chunk->correlation_id : "");
    response_chunk.setChunkIndex (chunk->chunk_index);
    response_chunk.setBytes (kj::arrayPtr (
        reinterpret_cast<const capnp::byte *> (chunk_bytes), chunk_size));
    response_chunk.setEndOfStream (chunk->end_of_stream);

    auto words = capnp::messageToFlatArray (response_builder);
    auto bytes = words.asBytes ();
    *out_bytes = g_bytes_new (bytes.begin (), bytes.size ());

    return TRUE;
  } catch (const std::exception &e) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA,
        "stream chunk response encode failed: %s",
        e.what ());
  }

  return FALSE;
}

static gboolean
encode_mailbox_select_response (const WyreboxDaemonResponseFrame *response_frame,
    GBytes **out_bytes,
    GError **error)
{
  try {
    const WyreboxDaemonMailboxSelectResult *result =
        &response_frame->mailbox_select;
    MailboxListEntryKind encoded_kind = MailboxListEntryKind::ORDINARY;

    if (response_frame->request_id == NULL || *response_frame->request_id == '\0')
      return set_invalid_argument (error, "response frame request_id is required");

    if (!map_mailbox_list_entry_kind (result->kind, &encoded_kind))
      return set_invalid_argument (error,
          "unsupported mailbox select result kind");

    if (result->mailbox_id == NULL || *result->mailbox_id == '\0')
      return set_invalid_argument (error,
          "mailbox SELECT response mailbox_id is required");

    if (result->mailbox_name == NULL || *result->mailbox_name == '\0')
      return set_invalid_argument (error,
          "mailbox SELECT response mailbox_name is required");

    if (result->uid_validity == 0)
      return set_invalid_argument (error,
          "mailbox SELECT response uidvalidity is required");

    if (result->uid_next == 0)
      return set_invalid_argument (error,
          "mailbox SELECT response uidnext is required");

    capnp::MallocMessageBuilder response_builder;
    auto response_frame_message = response_builder.initRoot<ResponseFrame> ();
    auto response_select = response_frame_message.initMailboxSelect ();

    response_frame_message.setRequestId (response_frame->request_id);
    response_frame_message.setCorrelationId (
        response_frame->correlation_id != NULL ? response_frame->correlation_id : "");

    response_select.setRequestId (response_frame->request_id);
    response_select.setKind (encoded_kind);
    response_select.setMailboxId (result->mailbox_id);
    response_select.setMailboxName (result->mailbox_name);
    response_select.setUidValidity (result->uid_validity);
    response_select.setUidNext (result->uid_next);

    auto words = capnp::messageToFlatArray (response_builder);
    auto bytes = words.asBytes ();
    *out_bytes = g_bytes_new (bytes.begin (), bytes.size ());

    return TRUE;
  } catch (const std::exception &e) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA,
        "mailbox SELECT response encode failed: %s",
        e.what ());
  }

  return FALSE;
}

static gboolean
encode_error_response (const WyreboxDaemonResponseFrame *response_frame,
    GBytes **out_bytes,
    GError **error)
{
  try {
    ErrorClass encoded_error_class = ErrorClass::INTERNAL_ERROR;

    if (response_frame->request_id == NULL || *response_frame->request_id == '\0')
      return set_invalid_argument (error, "response frame request_id is required");

    if (response_frame->error.message == NULL
        || *response_frame->error.message == '\0')
      return set_invalid_argument (error, "error frame message is required");

    if (!map_error_class (response_frame->error.error_class, &encoded_error_class))
      return set_invalid_argument (error,
          "unsupported daemon error class");

    capnp::MallocMessageBuilder response_builder;
    auto response_frame_message = response_builder.initRoot<ResponseFrame> ();
    auto response_error = response_frame_message.initError ();

    response_frame_message.setRequestId (response_frame->request_id);
    response_frame_message.setCorrelationId (response_frame->correlation_id != NULL
        ? response_frame->correlation_id
        : "");

    response_error.setRequestId (
        response_frame->error.request_id != NULL && *response_frame->error.request_id != '\0'
            ? response_frame->error.request_id
            : response_frame->request_id);
    response_error.setErrorClass (encoded_error_class);
    response_error.setMessage (response_frame->error.message);
    response_error.setRetryHint (response_frame->error.retry_hint != NULL
        ? response_frame->error.retry_hint
        : "");

    auto words = capnp::messageToFlatArray (response_builder);
    auto bytes = words.asBytes ();
    *out_bytes = g_bytes_new (bytes.begin (), bytes.size ());

    return TRUE;
  } catch (const std::exception &e) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA,
        "error response encode failed: %s",
        e.what ());
  }

  return FALSE;
}

gboolean
wyrebox_daemon_capnp_codec_decode_request_frame (
    const WyreboxDaemonPeerCredentials *peer_credentials,
    GBytes *request,
    WyreboxDaemonDecodedRequestFrame *out_request_frame,
    gpointer *out_decoded_state,
    GDestroyNotify *out_decoded_state_clear,
    gpointer user_data,
    GError **error)
{
  (void) peer_credentials;
  (void) user_data;

  g_return_val_if_fail (out_request_frame != NULL, FALSE);
  g_return_val_if_fail (out_decoded_state != NULL, FALSE);
  g_return_val_if_fail (out_decoded_state_clear != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (request == NULL)
    return set_invalid_argument (error, "request payload is null");

  const gsize payload_size = g_bytes_get_size (request);
  if (payload_size == 0)
    return set_invalid_argument (error, "request payload is empty");

  if (payload_size % sizeof (capnp::word) != 0)
    return set_invalid_argument (error,
        "request payload size is not a valid Cap'n Proto byte size");

  g_autofree guint8 *payload_copy = NULL;
  const guint8 *payload_data = static_cast<const guint8 *> (
      g_bytes_get_data (request, NULL));
  payload_copy = static_cast<guint8 *> (g_memdup2 (payload_data, payload_size));
  if (payload_copy == NULL)
    return set_invalid_argument (error, "failed to allocate request payload copy");

  *out_request_frame = (WyreboxDaemonDecodedRequestFrame) { 0 };
  auto state = static_cast<WyreboxDaemonCapnpDecodedRequestState *> (
      g_malloc0 (sizeof (WyreboxDaemonCapnpDecodedRequestState)));

  if (!decode_request_frame (reinterpret_cast<const capnp::word *> (payload_copy),
      payload_size / sizeof (capnp::word),
      state,
      out_request_frame,
      error)) {
    wyrebox_daemon_capnp_codec_decoded_state_clear (state);
    return FALSE;
  }

  *out_decoded_state = state;
  *out_decoded_state_clear = wyrebox_daemon_capnp_codec_decoded_state_clear;

  return TRUE;
}

GBytes *
wyrebox_daemon_capnp_codec_encode_response_frame (
    const WyreboxDaemonResponseFrame *response_frame,
    gpointer user_data,
    GError **error)
{
  g_autoptr (GBytes) out_bytes = NULL;

  (void) user_data;

  g_return_val_if_fail (response_frame != NULL, NULL);
  g_return_val_if_fail (error == NULL || *error == NULL, NULL);

  switch (response_frame->kind) {
    case WYREBOX_DAEMON_RESPONSE_FRAME_SUCCESS:
      if (!encode_success_response (response_frame, &out_bytes, error))
        return NULL;
      return g_steal_pointer (&out_bytes);
    case WYREBOX_DAEMON_RESPONSE_FRAME_MAILBOX_LIST:
      if (!encode_mailbox_list_response (response_frame, &out_bytes, error))
        return NULL;
      return g_steal_pointer (&out_bytes);
    case WYREBOX_DAEMON_RESPONSE_FRAME_STREAM_CHUNK:
      if (!encode_stream_chunk_response (response_frame, &out_bytes, error))
        return NULL;
      return g_steal_pointer (&out_bytes);
    case WYREBOX_DAEMON_RESPONSE_FRAME_MAILBOX_SELECT:
      if (!encode_mailbox_select_response (response_frame, &out_bytes, error))
        return NULL;
      return g_steal_pointer (&out_bytes);
    case WYREBOX_DAEMON_RESPONSE_FRAME_ERROR:
      if (!encode_error_response (response_frame, &out_bytes, error))
        return NULL;
      return g_steal_pointer (&out_bytes);
    default:
      g_set_error (error,
          G_IO_ERROR,
          G_IO_ERROR_NOT_SUPPORTED,
          "unsupported response frame kind");
      return NULL;
  }
}
