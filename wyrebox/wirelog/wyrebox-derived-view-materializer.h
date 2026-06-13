#pragma once

#include "wyrebox-wirelog-derived-membership.h"

#include <glib-object.h>

/* *INDENT-OFF* */
G_BEGIN_DECLS

#define WYREBOX_TYPE_DERIVED_VIEW_MATERIALIZER (wyrebox_derived_view_materializer_get_type())

G_DECLARE_FINAL_TYPE (WyreboxDerivedViewMaterializer,
    wyrebox_derived_view_materializer,
    WYREBOX,
    DERIVED_VIEW_MATERIALIZER,
    GObject)

WyreboxDerivedViewMaterializer *wyrebox_derived_view_materializer_new_duckdb (
    const gchar *path,
    GError **error);

gboolean wyrebox_derived_view_materializer_apply_memberships (
    WyreboxDerivedViewMaterializer *self,
    const gchar *account_id,
    const gchar *view_id,
    const gchar *imap_name,
    const gchar *definition_ref,
    const gchar *rule_version_hash,
    guint64 materialized_at_unix_us,
    GPtrArray *memberships,
    GError **error);

gboolean wyrebox_derived_view_materializer_refresh_memberships (
    WyreboxDerivedViewMaterializer *self,
    const gchar *account_id,
    const gchar *view_id,
    const gchar *imap_name,
    const gchar *definition_ref,
    const gchar *rule_version_hash,
    guint64 materialized_at_unix_us,
    GPtrArray *memberships,
    GError **error);

G_END_DECLS
/* *INDENT-ON* */
