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
  char *mailbox_id;
  guint64 uid_validity;
  char *criteria_token;
} WyreboxDaemonMessageSearchRequest;

void wyrebox_daemon_message_search_request_clear (
    WyreboxDaemonMessageSearchRequest *request);

G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (WyreboxDaemonMessageSearchRequest,
    wyrebox_daemon_message_search_request_clear)

gboolean wyrebox_daemon_message_search_request_init (
    WyreboxDaemonMessageSearchRequest *request,
    const char *account_identity,
    const char *mailbox_id,
    guint64 uid_validity,
    const char *criteria_token,
    GError **error);

G_END_DECLS
/* *INDENT-ON* */
