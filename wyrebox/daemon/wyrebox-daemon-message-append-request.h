#pragma once

#include "wyrebox-daemon-frame-io.h"

#include <glib-object.h>

/*
 * Cap'n Proto frame budget minus room for identity fields and request
 * envelope outside messageBytes.
 */
#define WYREBOX_DAEMON_MESSAGE_APPEND_FRAME_RESERVE_BYTES (256 * 1024)

#define WYREBOX_DAEMON_MESSAGE_APPEND_MAX_MESSAGE_BYTES \
        (WYREBOX_DAEMON_MAX_FRAME_SIZE_BYTES - \
        WYREBOX_DAEMON_MESSAGE_APPEND_FRAME_RESERVE_BYTES)

/* *INDENT-OFF* */
G_BEGIN_DECLS

typedef struct
{
  /*
   * Strings, @message_bytes, and flag vectors are owned by the request and
   * released by clear().
   */
  char *account_identity;
  char *mailbox_id;
  guint64 uid_validity;
  GBytes *message_bytes;
  guint64 internal_date_unix_us;
  GStrv system_flags;
  GStrv user_keywords;
} WyreboxDaemonMessageAppendRequest;

void wyrebox_daemon_message_append_request_clear (
    WyreboxDaemonMessageAppendRequest *request);

G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (WyreboxDaemonMessageAppendRequest,
    wyrebox_daemon_message_append_request_clear)

gboolean wyrebox_daemon_message_append_request_init (
    WyreboxDaemonMessageAppendRequest *request,
    const char *account_identity,
    const char *mailbox_id,
    guint64 uid_validity,
    GBytes *message_bytes,
    guint64 internal_date_unix_us,
    const char *const *system_flags,
    const char *const *user_keywords,
    GError **error);

G_END_DECLS
/* *INDENT-ON* */
