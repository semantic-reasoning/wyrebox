#pragma once

#include "wyrebox-daemon-fact-mutation-service.h"
#include "wyrebox-daemon-mailbox-list-service.h"
#include "wyrebox-daemon-message-fetch-service.h"
#include "wyrebox-daemon-flag-keyword-update-service.h"
#include "wyrebox-daemon-message-search-service.h"
#include "wyrebox-daemon-peer-credentials.h"
#include "wyrebox-daemon-request-router.h"
#include "wyrebox-daemon-response-frame.h"

#include <glib-object.h>

/* *INDENT-OFF* */
G_BEGIN_DECLS

#define WYREBOX_TYPE_DAEMON_REQUEST_ADAPTER \
    (wyrebox_daemon_request_adapter_get_type())

G_DECLARE_FINAL_TYPE (WyreboxDaemonRequestAdapter,
    wyrebox_daemon_request_adapter,
    WYREBOX, DAEMON_REQUEST_ADAPTER, GObject)

typedef gboolean (*WyreboxDaemonRequestAdapterDecodeRequestFrameCallback) (
    const WyreboxDaemonPeerCredentials *peer_credentials,
    GBytes *request,
    WyreboxDaemonDecodedRequestFrame *out_request_frame,
    gpointer *out_decoded_state,
    GDestroyNotify *out_decoded_state_clear,
    gpointer user_data,
    GError **error);

typedef GBytes *(*WyreboxDaemonRequestAdapterEncodeResponseFrameCallback) (
    const WyreboxDaemonResponseFrame *response_frame,
    gpointer user_data,
    GError **error);

WyreboxDaemonRequestAdapter *wyrebox_daemon_request_adapter_new (
    WyreboxDaemonFactMutationService *fact_mutation_service,
    WyreboxDaemonMailboxListService *mailbox_list_service,
    WyreboxDaemonMailboxSelectService *mailbox_select_service,
    WyreboxDaemonMessageFetchService *message_fetch_service,
    WyreboxDaemonMessageSearchService *message_search_service,
    WyreboxDaemonFlagKeywordUpdateService *flag_keyword_update_service,
    WyreboxDaemonRequestAdapterDecodeRequestFrameCallback
    decode_request_frame_callback,
    gpointer decode_request_frame_user_data,
    GDestroyNotify decode_request_frame_user_data_destroy,
    WyreboxDaemonRequestAdapterEncodeResponseFrameCallback
    encode_response_frame_callback,
    gpointer encode_response_frame_user_data,
    GDestroyNotify encode_response_frame_user_data_destroy);

GBytes *wyrebox_daemon_request_adapter_handle_payload (
    const WyreboxDaemonPeerCredentials *peer_credentials,
    GBytes *request,
    gpointer user_data,
    GError **error);

G_END_DECLS
/* *INDENT-ON* */
