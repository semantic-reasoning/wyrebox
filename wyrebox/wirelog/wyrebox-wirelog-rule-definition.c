#include "wyrebox-wirelog-rule-definition.h"

#include "wyrebox-wirelog-program.h"
#include "wyrebox-wirelog-rule-version.h"

#include <string.h>

#define WYREBOX_WIRELOG_DEFAULT_RELATION_NAME "show_in_virtual_folder"

void
wyrebox_wirelog_rule_definition_clear (WyreboxWirelogRuleDefinition *definition)
{
  if (definition == NULL)
    return;

  g_clear_pointer (&definition->definition_ref, g_free);
  g_clear_pointer (&definition->rules_source, g_free);
  g_clear_pointer (&definition->rule_version_hash, g_free);
  g_clear_pointer (&definition->relation_name, g_free);
}

void
wyrebox_wirelog_rule_definition_free (WyreboxWirelogRuleDefinition *definition)
{
  if (definition == NULL)
    return;

  wyrebox_wirelog_rule_definition_clear (definition);
  g_free (definition);
}

static char *
build_definition_ref (GFile *file)
{
  g_autofree char *path = NULL;
  g_autofree char *uri = NULL;

  path = g_file_get_path (file);
  if (path != NULL)
    return g_strdup_printf ("file:%s", path);

  uri = g_file_get_uri (file);
  return g_strdup_printf ("file:%s", uri);
}

static gboolean
validate_rules_source_bytes (const char *contents, gsize size, GError **error)
{
  if (size == 0) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT, "Wirelog rule definition file is empty");
    return FALSE;
  }

  if (memchr (contents, '\0', size) != NULL) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA,
        "Wirelog rule definition contains embedded NUL");
    return FALSE;
  }

  return TRUE;
}

WyreboxWirelogRuleDefinition *
wyrebox_wirelog_rule_definition_load_file (GFile *file, GError **error)
{
  g_autofree char *contents = NULL;
  gsize size = 0;
  g_autoptr (WyreboxWirelogProgram) program = NULL;
  g_autoptr (WyreboxWirelogRuleDefinition) definition = NULL;

  g_return_val_if_fail (error == NULL || *error == NULL, NULL);

  if (!G_IS_FILE (file)) {
    g_set_error (error,
        G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
        "Wirelog rule file is required");
    return NULL;
  }

  if (!g_file_load_contents (file, NULL, &contents, &size, NULL, error))
    return NULL;

  if (!validate_rules_source_bytes (contents, size, error))
    return NULL;

  definition = g_new0 (WyreboxWirelogRuleDefinition, 1);
  definition->definition_ref = build_definition_ref (file);
  definition->rules_source = g_strdup (contents);
  definition->rule_version_hash =
      wyrebox_wirelog_rule_version_hash (definition->rules_source, error);
  if (definition->rule_version_hash == NULL)
    return NULL;

  program = wyrebox_wirelog_program_new_from_source (definition->rules_source,
      error);
  if (program == NULL)
    return NULL;

  definition->relation_name = g_strdup (WYREBOX_WIRELOG_DEFAULT_RELATION_NAME);

  return g_steal_pointer (&definition);
}
