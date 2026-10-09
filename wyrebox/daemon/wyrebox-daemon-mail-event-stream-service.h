#pragma once

#include "wyrebox-daemon-mail-event-stream-request.h"
#include "wyrebox-daemon-request-identity.h"
#include "wyrebox-daemon-response-frame.h"
#include "wyrebox-journal-writer.h"

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

/*
 * Streams one account's mail events from the journal, one event per request.
 * See wyrebox_daemon_mail_event_project() for which records are included and
 * for the event format.
 *
 * Each chunk carries the event's journal offset as chunk_index and never sets
 * end_of_stream; when no event follows the cursor, the response is a single
 * empty chunk with chunk_index 0 and end_of_stream set. The request cursor
 * (after_journal_offset, after_journal_sequence) is (0, 0) to start at the
 * beginning, or the offset and sequence of the last event received to
 * continue after it.
 *
 * Each request scans the journal from its start, so its cost grows with the
 * journal size.
 *
 * This variant reads the whole journal segment and suits a journal that is
 * not being appended to.
 */
WyreboxDaemonMailEventStreamService *
wyrebox_daemon_mail_event_stream_service_new_from_journal_root (
    const char *journal_root_dir,
    GError **error);

/*
 * Like wyrebox_daemon_mail_event_stream_service_new_from_journal_root(), but
 * reads only up to the durable end of @journal_writer, so records that are
 * still being appended are never streamed. The service keeps a reference to
 * @journal_writer.
 */
WyreboxDaemonMailEventStreamService *
wyrebox_daemon_mail_event_stream_service_new_from_journal_writer (
    const char *journal_root_dir,
    WyreboxJournalWriter *journal_writer,
    GError **error);

gboolean wyrebox_daemon_mail_event_stream_service_handle_identity (
    WyreboxDaemonMailEventStreamService *self,
    const WyreboxDaemonRequestIdentity *identity,
    const WyreboxDaemonMailEventStreamRequest *request,
    WyreboxDaemonResponseFrame *out_frame,
    GError **error);

G_END_DECLS
/* *INDENT-ON* */
