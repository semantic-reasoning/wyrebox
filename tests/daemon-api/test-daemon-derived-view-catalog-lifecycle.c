#include "wyrebox-daemon-derived-view-catalog.h"

#include <gio/gio.h>
#include <glib.h>

static WyreboxDaemonDerivedViewDefinition
make_definition (const char *view_id, const char *imap_name)
{
  WyreboxDaemonDerivedViewDefinition definition = { 0 };

  definition.view_id = g_strdup (view_id);
  definition.imap_name = g_strdup (imap_name);
  definition.definition_ref = g_strdup ("wirelog:project-view");
  definition.rules_source = g_strdup ("view('project').");
  definition.relation_name = g_strdup ("show_in_virtual_folder");
  definition.enabled = TRUE;
  return definition;
}

static void
test_catalog_lifecycle_enable_disable_remove (void)
{
  g_autoptr (WyreboxDaemonDerivedViewCatalog) catalog = NULL;
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonDerivedViewDefinition) definition = { 0 };
  const WyreboxDaemonDerivedViewDefinition *loaded = NULL;

  catalog = wyrebox_daemon_derived_view_catalog_new ();
  g_assert_nonnull (catalog);

  definition = make_definition ("view-project", "Project");
  g_assert_true (wyrebox_daemon_derived_view_catalog_register_definition
      (catalog, &definition, &error));
  g_assert_no_error (error);
  wyrebox_daemon_derived_view_definition_clear (&definition);

  loaded = wyrebox_daemon_derived_view_catalog_lookup_definition (catalog,
      "view-project");
  g_assert_nonnull (loaded);
  g_assert_true (loaded->enabled);

  g_assert_true (wyrebox_daemon_derived_view_catalog_disable_definition
      (catalog, "view-project", &error));
  g_assert_no_error (error);

  loaded = wyrebox_daemon_derived_view_catalog_lookup_definition (catalog,
      "view-project");
  g_assert_nonnull (loaded);
  g_assert_false (loaded->enabled);

  g_assert_true (wyrebox_daemon_derived_view_catalog_enable_definition
      (catalog, "view-project", &error));
  g_assert_no_error (error);

  loaded = wyrebox_daemon_derived_view_catalog_lookup_definition (catalog,
      "view-project");
  g_assert_nonnull (loaded);
  g_assert_true (loaded->enabled);

  g_assert_true (wyrebox_daemon_derived_view_catalog_remove_definition
      (catalog, "view-project", &error));
  g_assert_no_error (error);
  g_assert_null (wyrebox_daemon_derived_view_catalog_lookup_definition (catalog,
          "view-project"));
  g_assert_cmpuint (wyrebox_daemon_derived_view_catalog_get_n_definitions
      (catalog), ==, 0);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/daemon-api/derived-view-catalog/lifecycle",
      test_catalog_lifecycle_enable_disable_remove);

  return g_test_run ();
}
