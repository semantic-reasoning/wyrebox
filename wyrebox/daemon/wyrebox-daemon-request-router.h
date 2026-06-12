#pragma once

#include "wyrebox-daemon-fact-mutation-dispatcher.h"
#include "wyrebox-daemon-message-fetch-dispatcher.h"
#include "wyrebox-daemon-mailbox-list-dispatcher.h"
#include "wyrebox-daemon-mailbox-select-dispatcher.h"
#include "wyrebox-daemon-flag-keyword-update-dispatcher.h"
#include "wyrebox-daemon-message-search-dispatcher.h"

#include <glib-object.h>

/* *INDENT-OFF* */
G_BEGIN_DECLS

typedef enum {
  WYREBOX_DAEMON_REQUEST_FRAME_OPERATION_NONE,
  WYREBOX_DAEMON_REQUEST_FRAME_OPERATION_MAILBOX_LIST,
  WYREBOX_DAEMON_REQUEST_FRAME_OPERATION_MAILBOX_SELECT,
  WYREBOX_DAEMON_REQUEST_FRAME_OPERATION_MESSAGE_FETCH,
  WYREBOX_DAEMON_REQUEST_FRAME_OPERATION_MESSAGE_SEARCH,
  WYREBOX_DAEMON_REQUEST_FRAME_OPERATION_FACT_MUTATION,
  WYREBOX_DAEMON_REQUEST_FRAME_OPERATION_FLAG_KEYWORD_UPDATE,
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
  const WyreboxDaemonFactMutationRequest *fact_mutation;
  const WyreboxDaemonMessageFetchRequest *message_fetch;
  const WyreboxDaemonMessageSearchRequest *message_search;
  const WyreboxDaemonFlagKeywordUpdateRequest *flag_keyword_update;
} WyreboxDaemonDecodedRequestFrame;

gboolean wyrebox_daemon_request_router_route (
    WyreboxDaemonFactMutationService *fact_mutation_service,
    WyreboxDaemonMailboxListService *mailbox_list_service,
    WyreboxDaemonMailboxSelectService *mailbox_select_service,
    WyreboxDaemonMessageFetchService *message_fetch_service,
    WyreboxDaemonMessageSearchService *message_search_service,
    WyreboxDaemonFlagKeywordUpdateService *flag_keyword_update_service,
    const WyreboxDaemonDecodedRequestFrame *request_frame,
    WyreboxDaemonResponseFrame *out_frame,
    GError **error);

G_END_DECLS
/* *INDENT-ON* */
