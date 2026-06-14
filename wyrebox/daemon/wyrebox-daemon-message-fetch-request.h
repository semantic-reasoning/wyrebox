#pragma once

#include "wyrebox-daemon-mailbox-list-result.h"

#include <glib-object.h>

/* *INDENT-OFF* */
G_BEGIN_DECLS

typedef struct
{
  /*
   * Strings are owned by the request and are released by clear().
   */
  char *account_identity;
  char *mailbox_id;
  WyreboxDaemonMailboxListEntryKind namespace_kind;
  guint64 uid_validity;
  guint64 mailbox_uid;
} WyreboxDaemonMessageFetchRequest;

void wyrebox_daemon_message_fetch_request_clear (
    WyreboxDaemonMessageFetchRequest *request);

G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (WyreboxDaemonMessageFetchRequest,
    wyrebox_daemon_message_fetch_request_clear)

gboolean wyrebox_daemon_message_fetch_request_init (
    WyreboxDaemonMessageFetchRequest *request,
    const char *account_identity,
    const char *mailbox_id,
    WyreboxDaemonMailboxListEntryKind namespace_kind,
    guint64 uid_validity,
    guint64 mailbox_uid,
    GError **error);

G_END_DECLS
/* *INDENT-ON* */
