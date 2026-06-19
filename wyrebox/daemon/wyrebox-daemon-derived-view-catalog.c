#include "wyrebox-daemon-derived-view-catalog.h"

#include <gio/gio.h>

struct _WyreboxDaemonDerivedViewCatalog
{
  GObject parent_instance;
  GPtrArray *definitions;
};

G_DEFINE_TYPE (WyreboxDaemonDerivedViewCatalog,
    wyrebox_daemon_derived_view_catalog, G_TYPE_OBJECT);

void
wyrebox_daemon_derived_view_definition_clear (WyreboxDaemonDerivedViewDefinition
    *definition)
{
  if (definition == NULL)
    return;

  g_clear_pointer (&definition->view_id, g_free);
  g_clear_pointer (&definition->imap_name, g_free);
  g_clear_pointer (&definition->definition_ref, g_free);
  g_clear_pointer (&definition->rules_source, g_free);
  g_clear_pointer (&definition->relation_name, g_free);
  definition->enabled = FALSE;
}

static void
definition_free (gpointer data)
{
  WyreboxDaemonDerivedViewDefinition *definition = data;

  if (definition == NULL)
    return;

  wyrebox_daemon_derived_view_definition_clear (definition);
  g_free (definition);
}

void
wyrebox_daemon_derived_view_definition_free (WyreboxDaemonDerivedViewDefinition
    *definition)
{
  definition_free (definition);
}

static gboolean
has_control_character (const char *value)
{
  for (const char *cursor = value; *cursor != '\0'; cursor++) {
    if (g_ascii_iscntrl (*cursor))
      return TRUE;
  }

  return FALSE;
}

static gboolean
validate_definition_text (const char *value, const char *field_name,
    gboolean allow_control_characters, GError **error)
{
  if (value == NULL || *value == '\0') {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "derived view definition field '%s' is required", field_name);
    return FALSE;
  }

  if (!allow_control_characters && has_control_character (value)) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "derived view definition field '%s' must not contain control "
        "characters", field_name);
    return FALSE;
  }

  return TRUE;
}

static gboolean
definition_matches_view_id (const WyreboxDaemonDerivedViewDefinition *left,
    const WyreboxDaemonDerivedViewDefinition *right)
{
  return g_strcmp0 (left->view_id, right->view_id) == 0;
}

static gboolean
definition_matches_imap_name (const WyreboxDaemonDerivedViewDefinition *left,
    const WyreboxDaemonDerivedViewDefinition *right)
{
  return g_strcmp0 (left->imap_name, right->imap_name) == 0;
}

static gboolean
definition_matches_view_id_string (const WyreboxDaemonDerivedViewDefinition
    *definition, const char *view_id)
{
  return g_strcmp0 (definition->view_id, view_id) == 0;
}

static void
wyrebox_daemon_derived_view_catalog_finalize (GObject *object)
{
  WyreboxDaemonDerivedViewCatalog *self =
      WYREBOX_DAEMON_DERIVED_VIEW_CATALOG (object);

  g_clear_pointer (&self->definitions, g_ptr_array_unref);

  G_OBJECT_CLASS (wyrebox_daemon_derived_view_catalog_parent_class)->finalize
      (object);
}

static void
    wyrebox_daemon_derived_view_catalog_class_init
    (WyreboxDaemonDerivedViewCatalogClass * klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = wyrebox_daemon_derived_view_catalog_finalize;
}

static void
wyrebox_daemon_derived_view_catalog_init (WyreboxDaemonDerivedViewCatalog *self)
{
  self->definitions = g_ptr_array_new_with_free_func (definition_free);
}

WyreboxDaemonDerivedViewCatalog *
wyrebox_daemon_derived_view_catalog_new (void)
{
  return g_object_new (wyrebox_daemon_derived_view_catalog_get_type (), NULL);
}

void
wyrebox_daemon_derived_view_catalog_free (WyreboxDaemonDerivedViewCatalog
    *catalog)
{
  if (catalog != NULL)
    g_object_unref (catalog);
}

static gboolean
catalog_contains_definition (WyreboxDaemonDerivedViewCatalog *catalog,
    const WyreboxDaemonDerivedViewDefinition *definition)
{
  for (guint i = 0; i < catalog->definitions->len; i++) {
    const WyreboxDaemonDerivedViewDefinition *current =
        g_ptr_array_index (catalog->definitions, i);

    if (definition_matches_view_id (current, definition) ||
        definition_matches_imap_name (current, definition))
      return TRUE;
  }

  return FALSE;
}

gboolean
    wyrebox_daemon_derived_view_catalog_register_definition
    (WyreboxDaemonDerivedViewCatalog * catalog,
    const WyreboxDaemonDerivedViewDefinition * definition, GError ** error)
{
  WyreboxDaemonDerivedViewDefinition *copy = NULL;

  g_return_val_if_fail (WYREBOX_DAEMON_IS_DERIVED_VIEW_CATALOG (catalog),
      FALSE);
  g_return_val_if_fail (definition != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (!validate_definition_text (definition->view_id, "view_id", FALSE,
          error) ||
      !validate_definition_text (definition->imap_name, "imap_name", FALSE,
          error) ||
      !validate_definition_text (definition->definition_ref,
          "definition_ref", FALSE, error) ||
      !validate_definition_text (definition->rules_source, "rules_source",
          TRUE, error) ||
      !validate_definition_text (definition->relation_name, "relation_name",
          FALSE, error))
    return FALSE;

  if (catalog_contains_definition (catalog, definition)) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_EXISTS,
        "derived view definition '%s' is already registered",
        definition->view_id);
    return FALSE;
  }

  copy = g_new0 (WyreboxDaemonDerivedViewDefinition, 1);
  copy->view_id = g_strdup (definition->view_id);
  copy->imap_name = g_strdup (definition->imap_name);
  copy->definition_ref = g_strdup (definition->definition_ref);
  copy->rules_source = g_strdup (definition->rules_source);
  copy->relation_name = g_strdup (definition->relation_name);
  copy->enabled = TRUE;

  g_ptr_array_add (catalog->definitions, copy);
  return TRUE;
}

static WyreboxDaemonDerivedViewDefinition *
lookup_definition_mutable (WyreboxDaemonDerivedViewCatalog *catalog,
    const char *view_id)
{
  for (guint i = 0; i < catalog->definitions->len; i++) {
    WyreboxDaemonDerivedViewDefinition *current =
        g_ptr_array_index (catalog->definitions, i);

    if (definition_matches_view_id_string (current, view_id))
      return current;
  }

  return NULL;
}

gboolean
    wyrebox_daemon_derived_view_catalog_enable_definition
    (WyreboxDaemonDerivedViewCatalog * catalog, const char *view_id,
    GError ** error)
{
  WyreboxDaemonDerivedViewDefinition *definition = NULL;

  g_return_val_if_fail (WYREBOX_DAEMON_IS_DERIVED_VIEW_CATALOG (catalog),
      FALSE);
  g_return_val_if_fail (view_id != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  definition = lookup_definition_mutable (catalog, view_id);
  if (definition == NULL) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_NOT_FOUND,
        "derived view definition '%s' is not registered", view_id);
    return FALSE;
  }

  definition->enabled = TRUE;
  return TRUE;
}

gboolean
    wyrebox_daemon_derived_view_catalog_disable_definition
    (WyreboxDaemonDerivedViewCatalog * catalog, const char *view_id,
    GError ** error)
{
  WyreboxDaemonDerivedViewDefinition *definition = NULL;

  g_return_val_if_fail (WYREBOX_DAEMON_IS_DERIVED_VIEW_CATALOG (catalog),
      FALSE);
  g_return_val_if_fail (view_id != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  definition = lookup_definition_mutable (catalog, view_id);
  if (definition == NULL) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_NOT_FOUND,
        "derived view definition '%s' is not registered", view_id);
    return FALSE;
  }

  definition->enabled = FALSE;
  return TRUE;
}

gboolean
    wyrebox_daemon_derived_view_catalog_remove_definition
    (WyreboxDaemonDerivedViewCatalog * catalog, const char *view_id,
    GError ** error)
{
  guint index = 0;

  g_return_val_if_fail (WYREBOX_DAEMON_IS_DERIVED_VIEW_CATALOG (catalog),
      FALSE);
  g_return_val_if_fail (view_id != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  for (index = 0; index < catalog->definitions->len; index++) {
    WyreboxDaemonDerivedViewDefinition *current =
        g_ptr_array_index (catalog->definitions, index);

    if (definition_matches_view_id_string (current, view_id)) {
      g_ptr_array_remove_index (catalog->definitions, index);
      return TRUE;
    }
  }

  g_set_error (error,
      G_IO_ERROR,
      G_IO_ERROR_NOT_FOUND,
      "derived view definition '%s' is not registered", view_id);
  return FALSE;
}

guint
    wyrebox_daemon_derived_view_catalog_get_n_definitions
    (WyreboxDaemonDerivedViewCatalog * catalog) {
  g_return_val_if_fail (WYREBOX_DAEMON_IS_DERIVED_VIEW_CATALOG (catalog), 0);
  return catalog->definitions->len;
}

const WyreboxDaemonDerivedViewDefinition
    * wyrebox_daemon_derived_view_catalog_get_definition
    (WyreboxDaemonDerivedViewCatalog * catalog, guint index)
{
  g_return_val_if_fail (WYREBOX_DAEMON_IS_DERIVED_VIEW_CATALOG (catalog), NULL);
  g_return_val_if_fail (index < catalog->definitions->len, NULL);
  return g_ptr_array_index (catalog->definitions, index);
}

const WyreboxDaemonDerivedViewDefinition
    * wyrebox_daemon_derived_view_catalog_lookup_definition
    (WyreboxDaemonDerivedViewCatalog * catalog, const char *view_id)
{
  g_return_val_if_fail (WYREBOX_DAEMON_IS_DERIVED_VIEW_CATALOG (catalog), NULL);
  g_return_val_if_fail (view_id != NULL, NULL);

  return lookup_definition_mutable (catalog, view_id);
}
