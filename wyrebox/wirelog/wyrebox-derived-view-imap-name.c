#include "wyrebox-derived-view-imap-name.h"

#include <gio/gio.h>
#include <string.h>

gboolean
wyrebox_derived_view_imap_name_validate_stored (const gchar *imap_name,
    GError **error)
{
  gsize len = 0;

  if (imap_name == NULL || imap_name[0] == '\0') {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "derived view imap_name must not be empty");
    return FALSE;
  }

  if (!g_utf8_validate (imap_name, -1, NULL)) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "derived view imap_name must be valid UTF-8");
    return FALSE;
  }

  len = strlen (imap_name);
  if (imap_name[0] == '/' || imap_name[len - 1] == '/') {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "derived view imap_name must not start or end with '/'");
    return FALSE;
  }

  for (const gchar * cursor = imap_name; *cursor != '\0';
      cursor = g_utf8_next_char (cursor)) {
    if (g_unichar_iscntrl (g_utf8_get_char (cursor))) {
      g_set_error (error,
          G_IO_ERROR,
          G_IO_ERROR_INVALID_ARGUMENT,
          "derived view imap_name must not contain control characters");
      return FALSE;
    }
  }

  for (gsize i = 0; i < len; i++) {
    const guchar c = (guchar) imap_name[i];

    if (c == '*' || c == '%') {
      g_set_error (error,
          G_IO_ERROR,
          G_IO_ERROR_INVALID_ARGUMENT,
          "derived view imap_name must not contain '*' or '%%'");
      return FALSE;
    }

    if (c == '/' && imap_name[i + 1] == '/') {
      g_set_error (error,
          G_IO_ERROR,
          G_IO_ERROR_INVALID_ARGUMENT,
          "derived view imap_name must not contain empty path segments");
      return FALSE;
    }
  }

  return TRUE;
}
