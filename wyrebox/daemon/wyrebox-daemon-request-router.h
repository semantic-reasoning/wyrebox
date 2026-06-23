#pragma once

#include "wyrebox-daemon-fact-mutation-dispatcher.h"
#include "wyrebox-daemon-message-fetch-dispatcher.h"
#include "wyrebox-daemon-mail-event-stream-service.h"
#include "wyrebox-daemon-mailbox-list-dispatcher.h"
#include "wyrebox-daemon-mailbox-select-dispatcher.h"
#include "wyrebox-daemon-mailbox-status-request.h"
#include "wyrebox-daemon-delivery-ingestion-dispatcher.h"
#include "wyrebox-daemon-duckdb-query-template-dispatcher.h"
#include "wyrebox-daemon-wirelog-predicate-query-dispatcher.h"
#include "wyrebox-daemon-flag-keyword-update-dispatcher.h"
#include "wyrebox-daemon-message-search-dispatcher.h"

#include <glib-object.h>

/* *INDENT-OFF* */
G_BEGIN_DECLS

typedef enum {
  WYREBOX_DAEMON_REQUEST_FRAME_OPERATION_NONE,
  WYREBOX_DAEMON_REQUEST_FRAME_OPERATION_MAILBOX_LIST,
  WYREBOX_DAEMON_REQUEST_FRAME_OPERATION_MAILBOX_SELECT,
  WYREBOX_DAEMON_REQUEST_FRAME_OPERATION_MAILBOX_STATUS,
  WYREBOX_DAEMON_REQUEST_FRAME_OPERATION_MESSAGE_FETCH,
  WYREBOX_DAEMON_REQUEST_FRAME_OPERATION_MESSAGE_SEARCH,
  WYREBOX_DAEMON_REQUEST_FRAME_OPERATION_MAIL_EVENT_STREAM,
  WYREBOX_DAEMON_REQUEST_FRAME_OPERATION_DELIVERY_INGESTION,
  WYREBOX_DAEMON_REQUEST_FRAME_OPERATION_FACT_MUTATION,
  WYREBOX_DAEMON_REQUEST_FRAME_OPERATION_FACT_BATCH_IMPORT,
  WYREBOX_DAEMON_REQUEST_FRAME_OPERATION_FLAG_KEYWORD_UPDATE,
  WYREBOX_DAEMON_REQUEST_FRAME_OPERATION_WIRELOG_PREDICATE_QUERY,
  WYREBOX_DAEMON_REQUEST_FRAME_OPERATION_DUCKDB_QUERY_TEMPLATE,
} WyreboxDaemonRequestFrameOperation;

typedef struct
{
  const char *request_id;
  const char *caller_identity;
  const char *account_identity;
  const char *tool_identity;
  const char *correlation_id;

  WyreboxDaemonRequestFrameOperation operation;
  const WyreboxDaemonMailboxListRequest *mailbox_list;
  const WyreboxDaemonMailboxSelectRequest *mailbox_select;
  const WyreboxDaemonMailboxStatusRequest *mailbox_status;
  const WyreboxDaemonFactMutationRequest *fact_mutation;
  const WyreboxDaemonFactBatchImportRequest *fact_batch_import;
  const WyreboxDaemonMessageFetchRequest *message_fetch;
  const WyreboxDaemonMessageSearchRequest *message_search;
  const WyreboxDaemonMailEventStreamRequest *mail_event_stream;
  const WyreboxDaemonDeliveryIngestionRequest *delivery_ingestion;
  const WyreboxDaemonFlagKeywordUpdateRequest *flag_keyword_update;
  const WyreboxDaemonWirelogPredicateQueryRequest *wirelog_predicate_query;
  const WyreboxDaemonDuckDBQueryTemplateRequest *duckdb_query_template;
} WyreboxDaemonDecodedRequestFrame;

gboolean wyrebox_daemon_request_router_route (
    WyreboxDaemonDeliveryIngestionService *delivery_ingestion_service,
    WyreboxDaemonFactMutationService *fact_mutation_service,
    WyreboxDaemonMailboxListService *mailbox_list_service,
    WyreboxDaemonMailboxSelectService *mailbox_select_service,
    WyreboxDaemonMessageFetchService *message_fetch_service,
    WyreboxDaemonMessageSearchService *message_search_service,
    WyreboxDaemonMailEventStreamService *mail_event_stream_service,
    WyreboxDaemonWirelogPredicateQueryService *wirelog_predicate_query_service,
    WyreboxDaemonDuckDBQueryTemplateService *duckdb_query_template_service,
    WyreboxDaemonFlagKeywordUpdateService *flag_keyword_update_service,
    const WyreboxDaemonDecodedRequestFrame *request_frame,
    WyreboxDaemonResponseFrame *out_frame,
    GError **error);

G_END_DECLS
/* *INDENT-ON* */
