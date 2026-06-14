#pragma once

#include "wyrebox-daemon-delivery-ingestion-request.h"
#include "wyrebox-daemon-request-identity.h"

#include <gio/gio.h>

/* *INDENT-OFF* */
G_BEGIN_DECLS

#define WYREBOX_POSTFIX_LMTP_DEFAULT_SOCKET_PATH "/run/wyrebox/wyrebox.sock"

typedef struct
{
  char *socket_path;
} WyreboxPostfixLmtpOptions;

void wyrebox_postfix_lmtp_options_clear (
    WyreboxPostfixLmtpOptions *options);

G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (WyreboxPostfixLmtpOptions,
    wyrebox_postfix_lmtp_options_clear)

gboolean wyrebox_postfix_lmtp_session_build (
    int argc,
    const char * const *argv,
    GInputStream *input,
    gsize max_message_bytes,
    WyreboxPostfixLmtpOptions *options,
    WyreboxDaemonRequestIdentity *identity,
    WyreboxDaemonDeliveryIngestionRequest *request,
    GError **error);

G_END_DECLS
/* *INDENT-ON* */
