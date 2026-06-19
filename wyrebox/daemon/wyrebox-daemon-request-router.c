#include "wyrebox-daemon-request-router.h"

#include "wyrebox-daemon-error-frame.h"

#include <gio/gio.h>

static gboolean
request_id_is_usable (const char *request_id)
{
  return request_id != NULL && *request_id != '\0';
}

static gboolean
init_error_response (WyreboxDaemonResponseFrame *out_frame,
    const char *request_id, const char *correlation_id, const GError *cause,
    GError **error)
{
  g_auto (WyreboxDaemonErrorFrame) error_frame = { 0 };

  if (!request_id_is_usable (request_id)) {
    g_propagate_error (error, g_error_copy (cause));
    return FALSE;
  }

  if (!wyrebox_daemon_error_frame_init_from_g_error (&error_frame,
          request_id, cause, NULL, error))
    return FALSE;

  return wyrebox_daemon_response_frame_init_error (out_frame,
      &error_frame, correlation_id, error);
}

static gboolean
route_message_fetch (WyreboxDaemonMessageFetchService *service,
    const WyreboxDaemonDecodedRequestFrame *request_frame,
    WyreboxDaemonResponseFrame *out_frame, GError **error)
{
  g_autoptr (GError) local_error = NULL;

  if (!WYREBOX_IS_DAEMON_MESSAGE_FETCH_SERVICE (service)) {
    g_set_error (&local_error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "message FETCH request frame cannot be routed without service");
    return init_error_response (out_frame,
        request_frame->request_id, request_frame->correlation_id, local_error,
        error);
  }

  if (request_frame->message_fetch == NULL) {
    g_set_error (&local_error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "message FETCH request frame is missing payload");
    return init_error_response (out_frame,
        request_frame->request_id, request_frame->correlation_id, local_error,
        error);
  }

  return wyrebox_daemon_message_fetch_dispatch (service,
      request_frame->request_id,
      request_frame->caller_identity,
      request_frame->account_identity,
      request_frame->tool_identity,
      request_frame->correlation_id,
      request_frame->message_fetch, out_frame, error);
}

static gboolean
route_message_search (WyreboxDaemonMessageSearchService *service,
    const WyreboxDaemonDecodedRequestFrame *request_frame,
    WyreboxDaemonResponseFrame *out_frame, GError **error)
{
  g_autoptr (GError) local_error = NULL;

  if (!WYREBOX_IS_DAEMON_MESSAGE_SEARCH_SERVICE (service)) {
    g_set_error (&local_error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "message SEARCH request frame cannot be routed without service");
    return init_error_response (out_frame,
        request_frame->request_id, request_frame->correlation_id, local_error,
        error);
  }

  if (request_frame->message_search == NULL) {
    g_set_error (&local_error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "message SEARCH request frame is missing payload");
    return init_error_response (out_frame,
        request_frame->request_id, request_frame->correlation_id, local_error,
        error);
  }

  return wyrebox_daemon_message_search_dispatch (service,
      request_frame->request_id,
      request_frame->caller_identity,
      request_frame->account_identity,
      request_frame->tool_identity,
      request_frame->correlation_id,
      request_frame->message_search, out_frame, error);
}

static gboolean
route_mail_event_stream (WyreboxDaemonMailEventStreamService *service,
    const WyreboxDaemonDecodedRequestFrame *request_frame,
    WyreboxDaemonResponseFrame *out_frame, GError **error)
{
  g_autoptr (GError) local_error = NULL;

  if (!WYREBOX_IS_DAEMON_MAIL_EVENT_STREAM_SERVICE (service)) {
    g_set_error (&local_error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "mail event stream request frame cannot be routed without service");
    return init_error_response (out_frame,
        request_frame->request_id, request_frame->correlation_id, local_error,
        error);
  }

  if (request_frame->mail_event_stream == NULL) {
    g_set_error (&local_error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "mail event stream request frame is missing payload");
    return init_error_response (out_frame,
        request_frame->request_id, request_frame->correlation_id, local_error,
        error);
  }

  return wyrebox_daemon_mail_event_stream_service_handle_identity (service,
      &(WyreboxDaemonRequestIdentity) {
      .request_id = (char *) request_frame->request_id,.caller_identity =
        (char *) request_frame->caller_identity,.account_identity =
        (char *) request_frame->account_identity,.tool_identity =
        (char *) request_frame->tool_identity,.correlation_id =
        (char *) request_frame->correlation_id,},
      request_frame->mail_event_stream, out_frame, error);
}

static gboolean
route_wirelog_predicate_query (WyreboxDaemonWirelogPredicateQueryService
    *service, const WyreboxDaemonDecodedRequestFrame *request_frame,
    WyreboxDaemonResponseFrame *out_frame, GError **error)
{
  g_autoptr (GError) local_error = NULL;

  if (!WYREBOX_IS_DAEMON_WIRELOG_PREDICATE_QUERY_SERVICE (service)) {
    g_set_error (&local_error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "wirelog predicate query request frame cannot be routed without service");
    return init_error_response (out_frame,
        request_frame->request_id,
        request_frame->correlation_id, local_error, error);
  }

  if (request_frame->wirelog_predicate_query == NULL) {
    g_set_error (&local_error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "wirelog predicate query request frame is missing payload");
    return init_error_response (out_frame,
        request_frame->request_id,
        request_frame->correlation_id, local_error, error);
  }

  return wyrebox_daemon_wirelog_predicate_query_dispatch (service,
      request_frame->request_id,
      request_frame->caller_identity,
      request_frame->account_identity,
      request_frame->tool_identity,
      request_frame->correlation_id,
      request_frame->wirelog_predicate_query, out_frame, error);
}

static gboolean
route_duckdb_query_template (WyreboxDaemonDuckDBQueryTemplateService *service,
    const WyreboxDaemonDecodedRequestFrame *request_frame,
    WyreboxDaemonResponseFrame *out_frame, GError **error)
{
  g_autoptr (GError) local_error = NULL;

  if (!WYREBOX_IS_DAEMON_DUCKDB_QUERY_TEMPLATE_SERVICE (service)) {
    g_set_error (&local_error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "duckdb query template request frame cannot be routed without service");
    return init_error_response (out_frame,
        request_frame->request_id,
        request_frame->correlation_id, local_error, error);
  }

  if (request_frame->duckdb_query_template == NULL) {
    g_set_error (&local_error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "duckdb query template request frame is missing payload");
    return init_error_response (out_frame,
        request_frame->request_id,
        request_frame->correlation_id, local_error, error);
  }

  return wyrebox_daemon_duckdb_query_template_dispatch (service,
      request_frame->request_id,
      request_frame->caller_identity,
      request_frame->account_identity,
      request_frame->tool_identity,
      request_frame->correlation_id,
      request_frame->duckdb_query_template, out_frame, error);
}

static gboolean
route_delivery_ingestion (WyreboxDaemonDeliveryIngestionService *service,
    const WyreboxDaemonDecodedRequestFrame *request_frame,
    WyreboxDaemonResponseFrame *out_frame, GError **error)
{
  g_autoptr (GError) local_error = NULL;

  if (!WYREBOX_IS_DAEMON_DELIVERY_INGESTION_SERVICE (service)) {
    g_set_error (&local_error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "delivery ingestion request frame cannot be routed without service");
    return init_error_response (out_frame,
        request_frame->request_id, request_frame->correlation_id, local_error,
        error);
  }

  if (request_frame->delivery_ingestion == NULL) {
    g_set_error (&local_error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "delivery ingestion request frame is missing payload");
    return init_error_response (out_frame,
        request_frame->request_id, request_frame->correlation_id, local_error,
        error);
  }

  return wyrebox_daemon_delivery_ingestion_dispatch (service,
      request_frame->request_id,
      request_frame->caller_identity,
      request_frame->account_identity,
      request_frame->tool_identity,
      request_frame->correlation_id,
      request_frame->delivery_ingestion, out_frame, error);
}

static gboolean
route_flag_keyword_update (WyreboxDaemonFlagKeywordUpdateService *service,
    const WyreboxDaemonDecodedRequestFrame *request_frame,
    WyreboxDaemonResponseFrame *out_frame, GError **error)
{
  g_autoptr (GError) local_error = NULL;

  if (!WYREBOX_IS_DAEMON_FLAG_KEYWORD_UPDATE_SERVICE (service)) {
    g_set_error (&local_error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "flag keyword update request frame cannot be routed without service");
    return init_error_response (out_frame,
        request_frame->request_id, request_frame->correlation_id, local_error,
        error);
  }

  if (request_frame->flag_keyword_update == NULL) {
    g_set_error (&local_error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "flag keyword update request frame is missing payload");
    return init_error_response (out_frame,
        request_frame->request_id, request_frame->correlation_id, local_error,
        error);
  }

  return wyrebox_daemon_flag_keyword_update_dispatch (service,
      request_frame->request_id,
      request_frame->caller_identity,
      request_frame->account_identity,
      request_frame->tool_identity,
      request_frame->correlation_id,
      request_frame->flag_keyword_update, out_frame, error);
}

static gboolean
route_fact_mutation (WyreboxDaemonFactMutationService *service,
    const WyreboxDaemonDecodedRequestFrame *request_frame,
    WyreboxDaemonResponseFrame *out_frame, GError **error)
{
  g_autoptr (GError) local_error = NULL;

  if (!WYREBOX_IS_DAEMON_FACT_MUTATION_SERVICE (service)) {
    g_set_error (&local_error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "fact mutation request frame cannot be routed without service");
    return init_error_response (out_frame,
        request_frame->request_id, request_frame->correlation_id, local_error,
        error);
  }

  if (request_frame->fact_mutation == NULL) {
    g_set_error (&local_error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "fact mutation request frame is missing payload");
    return init_error_response (out_frame,
        request_frame->request_id, request_frame->correlation_id, local_error,
        error);
  }

  return wyrebox_daemon_fact_mutation_dispatch (service,
      request_frame->request_id,
      request_frame->caller_identity,
      request_frame->account_identity,
      request_frame->tool_identity,
      request_frame->correlation_id,
      request_frame->fact_mutation, out_frame, error);
}

static gboolean
route_fact_batch_import (WyreboxDaemonFactMutationService *service,
    const WyreboxDaemonDecodedRequestFrame *request_frame,
    WyreboxDaemonResponseFrame *out_frame, GError **error)
{
  g_autoptr (GError) local_error = NULL;

  if (!WYREBOX_IS_DAEMON_FACT_MUTATION_SERVICE (service)) {
    g_set_error (&local_error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "fact batch import request frame cannot be routed without service");
    return init_error_response (out_frame,
        request_frame->request_id, request_frame->correlation_id, local_error,
        error);
  }

  if (request_frame->fact_batch_import == NULL) {
    g_set_error (&local_error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "fact batch import request frame is missing payload");
    return init_error_response (out_frame,
        request_frame->request_id, request_frame->correlation_id, local_error,
        error);
  }

  return wyrebox_daemon_fact_batch_import_dispatch (service,
      request_frame->request_id,
      request_frame->caller_identity,
      request_frame->account_identity,
      request_frame->tool_identity,
      request_frame->correlation_id,
      request_frame->fact_batch_import, out_frame, error);
}

static gboolean
route_mailbox_select (WyreboxDaemonMailboxSelectService *service,
    const WyreboxDaemonDecodedRequestFrame *request_frame,
    WyreboxDaemonResponseFrame *out_frame, GError **error)
{
  g_autoptr (GError) local_error = NULL;

  if (!WYREBOX_IS_DAEMON_MAILBOX_SELECT_SERVICE (service)) {
    g_set_error (&local_error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "mailbox SELECT request frame cannot be routed without service");
    return init_error_response (out_frame,
        request_frame->request_id, request_frame->correlation_id, local_error,
        error);
  }

  if (request_frame->mailbox_select == NULL) {
    g_set_error (&local_error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "mailbox SELECT request frame is missing payload");
    return init_error_response (out_frame,
        request_frame->request_id, request_frame->correlation_id, local_error,
        error);
  }

  return wyrebox_daemon_mailbox_select_dispatch (service,
      request_frame->request_id,
      request_frame->caller_identity,
      request_frame->account_identity,
      request_frame->tool_identity,
      request_frame->correlation_id,
      request_frame->mailbox_select, out_frame, error);
}

static gboolean
route_mailbox_status (WyreboxDaemonMailboxSelectService *service,
    const WyreboxDaemonDecodedRequestFrame *request_frame,
    WyreboxDaemonResponseFrame *out_frame, GError **error)
{
  g_auto (WyreboxDaemonMailboxSelectRequest) select_request = { 0 };
  g_autoptr (GError) local_error = NULL;

  if (!WYREBOX_IS_DAEMON_MAILBOX_SELECT_SERVICE (service)) {
    g_set_error (&local_error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "mailbox STATUS request frame cannot be routed without service");
    return init_error_response (out_frame,
        request_frame->request_id, request_frame->correlation_id, local_error,
        error);
  }

  if (request_frame->mailbox_status == NULL) {
    g_set_error (&local_error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "mailbox STATUS request frame is missing payload");
    return init_error_response (out_frame,
        request_frame->request_id, request_frame->correlation_id, local_error,
        error);
  }

  if (!wyrebox_daemon_mailbox_select_request_init (&select_request,
          request_frame->mailbox_status->account_identity,
          request_frame->mailbox_status->mailbox_id,
          request_frame->mailbox_status->mailbox_name, &local_error)) {
    return init_error_response (out_frame,
        request_frame->request_id, request_frame->correlation_id, local_error,
        error);
  }

  return wyrebox_daemon_mailbox_select_dispatch (service,
      request_frame->request_id,
      request_frame->caller_identity,
      request_frame->account_identity,
      request_frame->tool_identity,
      request_frame->correlation_id, &select_request, out_frame, error);
}

static gboolean
route_mailbox_list (WyreboxDaemonMailboxListService *service,
    const WyreboxDaemonDecodedRequestFrame *request_frame,
    WyreboxDaemonResponseFrame *out_frame, GError **error)
{
  g_autoptr (GError) local_error = NULL;

  if (!WYREBOX_IS_DAEMON_MAILBOX_LIST_SERVICE (service)) {
    g_set_error (&local_error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "mailbox LIST request frame cannot be routed without service");
    return init_error_response (out_frame,
        request_frame->request_id, request_frame->correlation_id, local_error,
        error);
  }

  if (request_frame->mailbox_list == NULL) {
    g_set_error (&local_error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "mailbox LIST request frame is missing payload");
    return init_error_response (out_frame,
        request_frame->request_id, request_frame->correlation_id, local_error,
        error);
  }

  return wyrebox_daemon_mailbox_list_dispatch (service,
      request_frame->request_id,
      request_frame->caller_identity,
      request_frame->account_identity,
      request_frame->tool_identity,
      request_frame->correlation_id,
      request_frame->mailbox_list, out_frame, error);
}

gboolean
wyrebox_daemon_request_router_route (WyreboxDaemonDeliveryIngestionService
    *delivery_ingestion_service, WyreboxDaemonFactMutationService
    *fact_mutation_service, WyreboxDaemonMailboxListService
    *mailbox_list_service, WyreboxDaemonMailboxSelectService
    *mailbox_select_service, WyreboxDaemonMessageFetchService
    *message_fetch_service, WyreboxDaemonMessageSearchService
    *message_search_service,
    WyreboxDaemonMailEventStreamService *mail_event_stream_service,
    WyreboxDaemonWirelogPredicateQueryService *wirelog_predicate_query_service,
    WyreboxDaemonDuckDBQueryTemplateService *duckdb_query_template_service,
    WyreboxDaemonFlagKeywordUpdateService *flag_keyword_update_service,
    const WyreboxDaemonDecodedRequestFrame *request_frame,
    WyreboxDaemonResponseFrame *out_frame, GError **error)
{
  g_autoptr (GError) local_error = NULL;

  g_return_val_if_fail (request_frame != NULL, FALSE);
  g_return_val_if_fail (out_frame != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  switch (request_frame->operation) {
    case WYREBOX_DAEMON_REQUEST_FRAME_OPERATION_MAILBOX_LIST:
      return route_mailbox_list (mailbox_list_service, request_frame,
          out_frame, error);
    case WYREBOX_DAEMON_REQUEST_FRAME_OPERATION_MAILBOX_SELECT:
      return route_mailbox_select (mailbox_select_service, request_frame,
          out_frame, error);
    case WYREBOX_DAEMON_REQUEST_FRAME_OPERATION_MAILBOX_STATUS:
      return route_mailbox_status (mailbox_select_service, request_frame,
          out_frame, error);
    case WYREBOX_DAEMON_REQUEST_FRAME_OPERATION_MESSAGE_FETCH:
      return route_message_fetch (message_fetch_service, request_frame,
          out_frame, error);
    case WYREBOX_DAEMON_REQUEST_FRAME_OPERATION_MESSAGE_SEARCH:
      return route_message_search (message_search_service, request_frame,
          out_frame, error);
    case WYREBOX_DAEMON_REQUEST_FRAME_OPERATION_MAIL_EVENT_STREAM:
      return route_mail_event_stream (mail_event_stream_service,
          request_frame, out_frame, error);
    case WYREBOX_DAEMON_REQUEST_FRAME_OPERATION_WIRELOG_PREDICATE_QUERY:
      return route_wirelog_predicate_query (wirelog_predicate_query_service,
          request_frame, out_frame, error);
    case WYREBOX_DAEMON_REQUEST_FRAME_OPERATION_DUCKDB_QUERY_TEMPLATE:
      return route_duckdb_query_template (duckdb_query_template_service,
          request_frame, out_frame, error);
    case WYREBOX_DAEMON_REQUEST_FRAME_OPERATION_DELIVERY_INGESTION:
      return route_delivery_ingestion (delivery_ingestion_service,
          request_frame, out_frame, error);
    case WYREBOX_DAEMON_REQUEST_FRAME_OPERATION_FACT_MUTATION:
      return route_fact_mutation (fact_mutation_service, request_frame,
          out_frame, error);
    case WYREBOX_DAEMON_REQUEST_FRAME_OPERATION_FACT_BATCH_IMPORT:
      return route_fact_batch_import (fact_mutation_service, request_frame,
          out_frame, error);
    case WYREBOX_DAEMON_REQUEST_FRAME_OPERATION_FLAG_KEYWORD_UPDATE:
      return route_flag_keyword_update (flag_keyword_update_service,
          request_frame, out_frame, error);
    default:
      g_set_error (&local_error,
          G_IO_ERROR,
          G_IO_ERROR_NOT_SUPPORTED, "unsupported daemon request operation");
      return init_error_response (out_frame,
          request_frame->request_id, request_frame->correlation_id,
          local_error, error);
  }
}
