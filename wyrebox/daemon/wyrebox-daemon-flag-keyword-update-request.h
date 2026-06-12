#pragma once

#include <glib-object.h>

/* *INDENT-OFF* */
G_BEGIN_DECLS

typedef enum {
  WYREBOX_DAEMON_FLAG_KEYWORD_UPDATE_MODE_SET,
  WYREBOX_DAEMON_FLAG_KEYWORD_UPDATE_MODE_CLEAR,
  WYREBOX_DAEMON_FLAG_KEYWORD_UPDATE_MODE_REPLACE,
} WyreboxDaemonFlagKeywordUpdateMode;

typedef struct
{
  /*
   * Strings are owned by the request and released by clear().
   */
  char *account_identity;
  char *mailbox_id;
  guint64 uid_validity;
  guint64 mailbox_uid;
  WyreboxDaemonFlagKeywordUpdateMode mode;
  gchar **system_flags;
  gchar **user_keywords;
} WyreboxDaemonFlagKeywordUpdateRequest;

void wyrebox_daemon_flag_keyword_update_request_clear (
    WyreboxDaemonFlagKeywordUpdateRequest *request);

G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (WyreboxDaemonFlagKeywordUpdateRequest,
    wyrebox_daemon_flag_keyword_update_request_clear)

gboolean wyrebox_daemon_flag_keyword_update_request_init (
    WyreboxDaemonFlagKeywordUpdateRequest *request,
    const char *account_identity,
    const char *mailbox_id,
    guint64 uid_validity,
    guint64 mailbox_uid,
    WyreboxDaemonFlagKeywordUpdateMode mode,
    const char * const *system_flags,
    const char * const *user_keywords,
    GError **error);

G_END_DECLS
/* *INDENT-ON* */
