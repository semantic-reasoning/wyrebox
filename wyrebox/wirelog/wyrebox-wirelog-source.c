#include "wyrebox-wirelog-source.h"

#include <gio/gio.h>

char *
wyrebox_wirelog_source_build (const char *rules_source,
    GPtrArray *facts, GError **error)
{
  g_autofree char *fact_source = NULL;
  g_autoptr (GString) source = NULL;

  g_return_val_if_fail (error == NULL || *error == NULL, NULL);

  if (facts == NULL) {
    g_set_error (error,
        G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "fact array is required");
    return NULL;
  }

  fact_source = wyrebox_fact_record_array_to_wirelog_facts (facts, error);
  if (fact_source == NULL)
    return NULL;

  source = g_string_new (NULL);
  if (rules_source != NULL && rules_source[0] != '\0') {
    g_string_append (source, rules_source);
    if (source->len > 0 && source->str[source->len - 1] != '\n')
      g_string_append_c (source, '\n');
  }
  g_string_append (source, fact_source);

  return g_string_free (g_steal_pointer (&source), FALSE);
}

WyreboxWirelogProgram *
wyrebox_wirelog_program_new_from_rules_and_facts (const char *rules_source,
    GPtrArray *facts, GError **error)
{
  g_autofree char *source = NULL;

  g_return_val_if_fail (error == NULL || *error == NULL, NULL);

  source = wyrebox_wirelog_source_build (rules_source, facts, error);
  if (source == NULL)
    return NULL;

  return wyrebox_wirelog_program_new_from_source (source, error);
}
