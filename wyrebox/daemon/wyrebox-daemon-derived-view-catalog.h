#pragma once

#include <glib-object.h>

/* *INDENT-OFF* */
G_BEGIN_DECLS

typedef struct
{
  gchar *view_id;
  gchar *imap_name;
  gchar *definition_ref;
  gchar *rules_source;
  gchar *relation_name;
} WyreboxDaemonDerivedViewDefinition;

void wyrebox_daemon_derived_view_definition_clear (
    WyreboxDaemonDerivedViewDefinition *definition);

void wyrebox_daemon_derived_view_definition_free (
    WyreboxDaemonDerivedViewDefinition *definition);

G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (WyreboxDaemonDerivedViewDefinition,
    wyrebox_daemon_derived_view_definition_clear)

typedef struct _WyreboxDaemonDerivedViewCatalog
    WyreboxDaemonDerivedViewCatalog;

G_DECLARE_FINAL_TYPE (WyreboxDaemonDerivedViewCatalog,
    wyrebox_daemon_derived_view_catalog,
    WYREBOX_DAEMON,
    DERIVED_VIEW_CATALOG,
    GObject)

WyreboxDaemonDerivedViewCatalog *
wyrebox_daemon_derived_view_catalog_new (void);

void wyrebox_daemon_derived_view_catalog_free (
    WyreboxDaemonDerivedViewCatalog *catalog);

gboolean wyrebox_daemon_derived_view_catalog_register_definition (
    WyreboxDaemonDerivedViewCatalog *catalog,
    const WyreboxDaemonDerivedViewDefinition *definition,
    GError **error);

guint wyrebox_daemon_derived_view_catalog_get_n_definitions (
    WyreboxDaemonDerivedViewCatalog *catalog);

const WyreboxDaemonDerivedViewDefinition *
wyrebox_daemon_derived_view_catalog_get_definition (
    WyreboxDaemonDerivedViewCatalog *catalog,
    guint index);

G_END_DECLS
/* *INDENT-ON* */
