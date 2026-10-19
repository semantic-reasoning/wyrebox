#pragma once

#include "wyrebox-daemon-mailbox-list-result.h"

#include <glib-object.h>

/*
 * Maximum source UID count for one COPY or MOVE request. Keeps the Cap'n Proto
 * frame and journaled payload within practical bounds.
 */
#define WYREBOX_DAEMON_MEMBERSHIPS_COPY_MOVE_MAX_UIDS 65535

/* *INDENT-OFF* */
G_BEGIN_DECLS

typedef struct
{
  /*
   * Strings and @source_uids are owned by the request and released by clear().
   */
  char *account_identity;
  char *source_mailbox_id;
  WyreboxDaemonMailboxListEntryKind source_namespace_kind;
  guint64 source_uid_validity;
  char *dest_mailbox_id;
  guint64 dest_uid_validity;
  guint64 *source_uids;
  gsize n_source_uids;
} WyreboxDaemonMembershipsCopyMoveRequest;

void wyrebox_daemon_memberships_copy_move_request_clear (
    WyreboxDaemonMembershipsCopyMoveRequest *request);

G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (WyreboxDaemonMembershipsCopyMoveRequest,
    wyrebox_daemon_memberships_copy_move_request_clear)

gboolean wyrebox_daemon_memberships_copy_move_request_init (
    WyreboxDaemonMembershipsCopyMoveRequest *request,
    const char *account_identity,
    const char *source_mailbox_id,
    WyreboxDaemonMailboxListEntryKind source_namespace_kind,
    guint64 source_uid_validity,
    const char *dest_mailbox_id,
    guint64 dest_uid_validity,
    const guint64 *source_uids,
    gsize n_source_uids,
    GError **error);

G_END_DECLS
/* *INDENT-ON* */
