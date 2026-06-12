#pragma once

#include "wyrebox-daemon-flag-keyword-update-request.h"
#include "wyrebox-daemon-request-identity.h"
#include "wyrebox-daemon-response-frame.h"

#include <glib-object.h>

/* *INDENT-OFF* */
G_BEGIN_DECLS

#define WYREBOX_TYPE_DAEMON_FLAG_KEYWORD_UPDATE_SERVICE \
    (wyrebox_daemon_flag_keyword_update_service_get_type())

G_DECLARE_FINAL_TYPE (WyreboxDaemonFlagKeywordUpdateService,
    wyrebox_daemon_flag_keyword_update_service,
    WYREBOX,
    DAEMON_FLAG_KEYWORD_UPDATE_SERVICE,
    GObject)

typedef gboolean (*WyreboxDaemonFlagKeywordUpdateServiceFunc) (
    const WyreboxDaemonRequestIdentity *identity,
    const WyreboxDaemonFlagKeywordUpdateRequest *request,
    WyreboxDaemonSuccessReceipt *out_receipt,
    gpointer user_data,
    GError **error);

WyreboxDaemonFlagKeywordUpdateService *
wyrebox_daemon_flag_keyword_update_service_new (WyreboxDaemonFlagKeywordUpdateServiceFunc
    func,
    gpointer user_data,
    GDestroyNotify user_data_destroy);

gboolean wyrebox_daemon_flag_keyword_update_service_handle_identity (
    WyreboxDaemonFlagKeywordUpdateService *self,
    const WyreboxDaemonRequestIdentity *identity,
    const WyreboxDaemonFlagKeywordUpdateRequest *request,
    WyreboxDaemonResponseFrame *out_frame,
    GError **error);

G_END_DECLS
/* *INDENT-ON* */
