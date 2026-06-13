#include "wyrebox-wirelog-rule-version.h"

#include <gio/gio.h>

char *
wyrebox_wirelog_rule_version_hash (const char *rules_source, GError **error)
{
  g_autofree char *digest = NULL;

  g_return_val_if_fail (error == NULL || *error == NULL, NULL);

  if (rules_source == NULL) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT, "Wirelog rule source is required");
    return NULL;
  }

  if (rules_source[0] == '\0') {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT, "Wirelog rule source must not be empty");
    return NULL;
  }

  digest = g_compute_checksum_for_string (G_CHECKSUM_SHA256, rules_source, -1);
  return g_strdup_printf ("sha256:%s", digest);
}
