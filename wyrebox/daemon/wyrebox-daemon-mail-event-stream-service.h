#pragma once

#include "wyrebox-daemon-mail-event-stream-request.h"
#include "wyrebox-daemon-request-identity.h"
#include "wyrebox-daemon-response-frame.h"

#include <glib-object.h>

/* *INDENT-OFF* */
G_BEGIN_DECLS

#define WYREBOX_TYPE_DAEMON_MAIL_EVENT_STREAM_SERVICE \
  (wyrebox_daemon_mail_event_stream_service_get_type())

G_DECLARE_FINAL_TYPE (WyreboxDaemonMailEventStreamService,
    wyrebox_daemon_mail_event_stream_service,
    WYREBOX,
    DAEMON_MAIL_EVENT_STREAM_SERVICE,
    GObject)

typedef gboolean (*WyreboxDaemonMailEventStreamServiceFunc) (
    const WyreboxDaemonRequestIdentity *identity,
    const WyreboxDaemonMailEventStreamRequest *request,
    WyreboxDaemonStreamChunkFrame *out_chunk,
    gpointer user_data,
    GError **error);

WyreboxDaemonMailEventStreamService *
wyrebox_daemon_mail_event_stream_service_new (
    WyreboxDaemonMailEventStreamServiceFunc func,
    gpointer user_data,
    GDestroyNotify user_data_destroy);

WyreboxDaemonMailEventStreamService *
wyrebox_daemon_mail_event_stream_service_new_from_journal_root (
    const char *journal_root_dir,
    GError **error);

gboolean wyrebox_daemon_mail_event_stream_service_handle_identity (
    WyreboxDaemonMailEventStreamService *self,
    const WyreboxDaemonRequestIdentity *identity,
    const WyreboxDaemonMailEventStreamRequest *request,
    WyreboxDaemonResponseFrame *out_frame,
    GError **error);

G_END_DECLS
/* *INDENT-ON* */
