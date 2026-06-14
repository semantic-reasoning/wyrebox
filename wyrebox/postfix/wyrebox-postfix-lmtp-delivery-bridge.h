#pragma once

#include "wyrebox-daemon-delivery-ingestion-request.h"
#include "wyrebox-daemon-request-identity.h"
#include "wyrebox-postfix-lmtp-reply.h"
#include "wyrebox-postfix-lmtp-session.h"

#include <gio/gio.h>

/* *INDENT-OFF* */
G_BEGIN_DECLS

gboolean wyrebox_postfix_lmtp_delivery_bridge_deliver (
    const WyreboxPostfixLmtpOptions *options,
    const WyreboxDaemonRequestIdentity *identity,
    const WyreboxDaemonDeliveryIngestionRequest *request,
    WyreboxPostfixLmtpReply *reply,
    GError **error);

G_END_DECLS
/* *INDENT-ON* */
