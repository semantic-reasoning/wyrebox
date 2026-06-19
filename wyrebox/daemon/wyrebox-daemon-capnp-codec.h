#pragma once

#include "wyrebox-build-config.h"
#include "wyrebox-daemon-delivery-ingestion-request.h"
#include "wyrebox-daemon-duckdb-query-template-request.h"
#include "wyrebox-daemon-mailbox-list-request.h"
#include "wyrebox-daemon-mailbox-select-request.h"
#include "wyrebox-daemon-mailbox-status-request.h"
#include "wyrebox-daemon-peer-credentials.h"
#include "wyrebox-daemon-request-identity.h"
#include "wyrebox-daemon-response-frame.h"
#include "wyrebox-daemon-request-router.h"

#include <gio/gio.h>
#include <glib-object.h>

/* *INDENT-OFF* */
G_BEGIN_DECLS

#if defined(WYREBOX_HAVE_CAPNP_SERIALIZATION) && WYREBOX_HAVE_CAPNP_SERIALIZATION

gboolean wyrebox_daemon_capnp_codec_decode_request_frame (
    const WyreboxDaemonPeerCredentials *peer_credentials,
    GBytes *request,
    WyreboxDaemonDecodedRequestFrame *out_request_frame,
    gpointer *out_decoded_state,
    GDestroyNotify *out_decoded_state_clear,
    gpointer user_data,
    GError **error);

GBytes *wyrebox_daemon_capnp_codec_encode_response_frame (
    const WyreboxDaemonResponseFrame *response_frame,
    gpointer user_data,
    GError **error);

gboolean wyrebox_daemon_capnp_codec_decode_response_frame (
    GBytes *response,
    WyreboxDaemonResponseFrame *out_response_frame,
    GError **error);

GBytes *wyrebox_daemon_capnp_codec_encode_delivery_ingestion_request (
    const WyreboxDaemonRequestIdentity *identity,
    const WyreboxDaemonDeliveryIngestionRequest *request,
    gpointer user_data,
    GError **error);

GBytes *wyrebox_daemon_capnp_codec_encode_message_fetch_request (
    const WyreboxDaemonRequestIdentity *identity,
    const WyreboxDaemonMessageFetchRequest *request,
    gpointer user_data,
    GError **error);

GBytes *wyrebox_daemon_capnp_codec_encode_duckdb_query_template_request (
    const WyreboxDaemonRequestIdentity *identity,
    const WyreboxDaemonDuckDBQueryTemplateRequest *request,
    gpointer user_data,
    GError **error);

GBytes *wyrebox_daemon_capnp_codec_encode_mailbox_list_request (
    const WyreboxDaemonRequestIdentity *identity,
    const WyreboxDaemonMailboxListRequest *request,
    gpointer user_data,
    GError **error);

GBytes *wyrebox_daemon_capnp_codec_encode_mailbox_select_request (
    const WyreboxDaemonRequestIdentity *identity,
    const WyreboxDaemonMailboxSelectRequest *request,
    gpointer user_data,
    GError **error);

GBytes *wyrebox_daemon_capnp_codec_encode_mailbox_status_request (
    const WyreboxDaemonRequestIdentity *identity,
    const WyreboxDaemonMailboxStatusRequest *request,
    gpointer user_data,
    GError **error);

void wyrebox_daemon_capnp_codec_decoded_state_clear (gpointer decoded_state);

#endif

G_END_DECLS
/* *INDENT-ON* */
