#pragma once

#include "wyrebox-daemon-mailbox-list-result.h"

#include <glib-object.h>

/* *INDENT-OFF* */
G_BEGIN_DECLS

typedef struct
{
  /*
   * Strings are owned by the result and are released by clear().
   * For virtual mailboxes, mailbox_id carries the stable WyreBox view_id.
   */
  WyreboxDaemonMailboxListEntryKind kind;
  char *mailbox_id;
  char *mailbox_name;
  guint32 uid_validity;
  guint32 uid_next;
} WyreboxDaemonMailboxSelectResult;

void wyrebox_daemon_mailbox_select_result_clear (
    WyreboxDaemonMailboxSelectResult *result);

G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (WyreboxDaemonMailboxSelectResult,
    wyrebox_daemon_mailbox_select_result_clear)

gboolean wyrebox_daemon_mailbox_select_result_init (
    WyreboxDaemonMailboxSelectResult *result,
    WyreboxDaemonMailboxListEntryKind kind,
    const char *mailbox_id,
    const char *mailbox_name,
    guint32 uid_validity,
    guint32 uid_next,
    GError **error);

G_END_DECLS
/* *INDENT-ON* */
