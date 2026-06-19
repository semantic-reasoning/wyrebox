#pragma once

#include <glib-object.h>

/* *INDENT-OFF* */
G_BEGIN_DECLS

typedef struct
{
  /*
   * Strings are owned by the request and are released by clear().
   * Exactly one mailbox selector must be present: mailbox_id or mailbox_name.
   */
  char *account_identity;
  char *mailbox_id;
  char *mailbox_name;
} WyreboxDaemonMailboxStatusRequest;

void wyrebox_daemon_mailbox_status_request_clear (
    WyreboxDaemonMailboxStatusRequest *request);

G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (WyreboxDaemonMailboxStatusRequest,
    wyrebox_daemon_mailbox_status_request_clear)

gboolean wyrebox_daemon_mailbox_status_request_init (
    WyreboxDaemonMailboxStatusRequest *request,
    const char *account_identity,
    const char *mailbox_id,
    const char *mailbox_name,
    GError **error);

G_END_DECLS
/* *INDENT-ON* */
