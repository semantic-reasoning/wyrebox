#pragma once

#include <glib-object.h>

/* *INDENT-OFF* */
G_BEGIN_DECLS

typedef struct
{
  /*
   * Strings and @uids are owned by the request and released by clear().
   */
  char *account_identity;
  char *mailbox_id;
  guint64 uid_validity;
  guint64 *uids;
  gsize n_uids;
} WyreboxDaemonMembershipsExpungeRequest;

void wyrebox_daemon_memberships_expunge_request_clear (
    WyreboxDaemonMembershipsExpungeRequest *request);

G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (WyreboxDaemonMembershipsExpungeRequest,
    wyrebox_daemon_memberships_expunge_request_clear)

gboolean wyrebox_daemon_memberships_expunge_request_init (
    WyreboxDaemonMembershipsExpungeRequest *request,
    const char *account_identity,
    const char *mailbox_id,
    guint64 uid_validity,
    const guint64 *uids,
    gsize n_uids,
    GError **error);

G_END_DECLS
/* *INDENT-ON* */
