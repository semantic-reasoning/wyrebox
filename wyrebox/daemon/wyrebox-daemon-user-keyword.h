#ifndef WYREBOX_DAEMON_USER_KEYWORD_H
#define WYREBOX_DAEMON_USER_KEYWORD_H

#include <glib.h>

G_BEGIN_DECLS

/*
 * RFC 3501 atom characters for IMAP user keywords: reject leading `\`,
 * atom-specials, controls, and non-ASCII. `$` and `+` are allowed.
 */
gboolean wyrebox_daemon_user_keyword_validate (const char *value,
    const char *error_prefix, GError **error);

/* ASCII case-insensitive equality for IMAP keywords (RFC 3501 §2.3.2). */
gboolean wyrebox_daemon_user_keyword_equal (gconstpointer a, gconstpointer b);

guint wyrebox_daemon_user_keyword_hash (gconstpointer key);

G_END_DECLS

#endif /* WYREBOX_DAEMON_USER_KEYWORD_H */
