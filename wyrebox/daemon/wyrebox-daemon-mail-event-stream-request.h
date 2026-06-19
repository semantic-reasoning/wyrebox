#pragma once

#include <glib-object.h>

/* *INDENT-OFF* */
G_BEGIN_DECLS

typedef struct
{
  /*
   * Strings are owned by the request and are released by clear().
   */
  char *account_identity;
  char *mailbox_identity;
  char *message_id;
  char *event_type;
  guint64 after_journal_offset;
  guint64 after_journal_sequence;
  guint64 after_unix_us;
  guint64 before_unix_us;
} WyreboxDaemonMailEventStreamRequest;

void wyrebox_daemon_mail_event_stream_request_clear (
    WyreboxDaemonMailEventStreamRequest *request);

G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (WyreboxDaemonMailEventStreamRequest,
    wyrebox_daemon_mail_event_stream_request_clear)

gboolean wyrebox_daemon_mail_event_stream_request_init (
    WyreboxDaemonMailEventStreamRequest *request,
    const char *account_identity,
    const char *mailbox_identity,
    const char *message_id,
    const char *event_type,
    guint64 after_journal_offset,
    guint64 after_journal_sequence,
    guint64 after_unix_us,
    guint64 before_unix_us,
    GError **error);

G_END_DECLS
/* *INDENT-ON* */
