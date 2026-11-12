#include "wyrebox-daemon-user-keyword.h"

#include <gio/gio.h>
#include <string.h>

static gboolean
is_atom_char (guchar c)
{
    if (c < 0x20 || c == 0x7f || c >= 0x80)
        return FALSE;

    switch (c) {
    case '(':
    case ')':
    case '{':
    case ' ':
    case '%':
    case '*':
    case '"':
    case '\\':
    case ']':
        return FALSE;
    default:
        return TRUE;
    }
}

gboolean
wyrebox_daemon_user_keyword_validate (const char *value,
    const char *error_prefix, GError **error)
{
    const char *prefix = error_prefix != NULL ? error_prefix : "user_keywords";

    if (value == NULL || value[0] == '\0') {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_INVALID_ARGUMENT, "%s is required", prefix);
        return FALSE;
    }

    if (value[0] == '\\') {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_INVALID_ARGUMENT,
            "%s must not contain system flags", prefix);
        return FALSE;
    }

    for (const char *cursor = value; *cursor != '\0'; cursor++) {
        if (!is_atom_char ((guchar) * cursor)) {
            g_set_error (error,
                G_IO_ERROR,
                G_IO_ERROR_INVALID_ARGUMENT,
                "%s contains invalid characters", prefix);
            return FALSE;
        }
    }

    return TRUE;
}

gboolean
wyrebox_daemon_user_keyword_equal (gconstpointer a, gconstpointer b)
{
    const char *left = a;
    const char *right = b;

    if (left == NULL || right == NULL)
        return left == right;

    return g_ascii_strcasecmp (left, right) == 0;
}

guint
wyrebox_daemon_user_keyword_hash (gconstpointer key)
{
    const char *value = key;
    guint hash = 5381;

    if (value == NULL)
        return 0;

    for (; *value != '\0'; value++)
        hash = (hash * 33) + (guint)g_ascii_tolower (*value);

    return hash;
}
