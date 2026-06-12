#pragma once

#include <glib-object.h>

/* *INDENT-OFF* */
G_BEGIN_DECLS

typedef struct
{
  char *account_identity;
  char *namespace_prefix;
} WyreboxDaemonMailboxListRequest;

void wyrebox_daemon_mailbox_list_request_clear (
    WyreboxDaemonMailboxListRequest *request);

G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (WyreboxDaemonMailboxListRequest,
    wyrebox_daemon_mailbox_list_request_clear)

gboolean wyrebox_daemon_mailbox_list_request_init (
    WyreboxDaemonMailboxListRequest *request,
    const char *account_identity,
    const char *namespace_prefix,
    GError **error);

G_END_DECLS
/* *INDENT-ON* */
