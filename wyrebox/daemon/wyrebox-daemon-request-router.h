#pragma once

#include "wyrebox-daemon-fact-mutation-dispatcher.h"
#include "wyrebox-daemon-mailbox-list-dispatcher.h"

#include <glib-object.h>

/* *INDENT-OFF* */
G_BEGIN_DECLS

typedef enum {
  WYREBOX_DAEMON_REQUEST_FRAME_OPERATION_NONE,
  WYREBOX_DAEMON_REQUEST_FRAME_OPERATION_MAILBOX_LIST,
  WYREBOX_DAEMON_REQUEST_FRAME_OPERATION_FACT_MUTATION,
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
  const WyreboxDaemonFactMutationRequest *fact_mutation;
} WyreboxDaemonDecodedRequestFrame;

gboolean wyrebox_daemon_request_router_route (
    WyreboxDaemonFactMutationService *fact_mutation_service,
    WyreboxDaemonMailboxListService *mailbox_list_service,
    const WyreboxDaemonDecodedRequestFrame *request_frame,
    WyreboxDaemonResponseFrame *out_frame,
    GError **error);

G_END_DECLS
/* *INDENT-ON* */
