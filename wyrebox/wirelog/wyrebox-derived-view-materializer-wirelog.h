#pragma once

#include "wyrebox-derived-view-materializer.h"
#include "wyrebox-fact-record.h"

#include <glib-object.h>

/* *INDENT-OFF* */
G_BEGIN_DECLS

gboolean
wyrebox_derived_view_materializer_refresh_from_rules_and_facts_with_changes (
    WyreboxDerivedViewMaterializer *self,
    const gchar *account_id,
    const gchar *view_id,
    const gchar *imap_name,
    const gchar *definition_ref,
    guint64 materialized_at_unix_us,
    const gchar *rules_source,
    GPtrArray *facts,
    const gchar *relation_name,
    GPtrArray **out_changes,
    GError **error);

gboolean
wyrebox_derived_view_materializer_refresh_from_rules_and_fact_journal_with_changes (
    WyreboxDerivedViewMaterializer *self,
    const gchar *account_id,
    const gchar *view_id,
    const gchar *imap_name,
    const gchar *definition_ref,
    guint64 materialized_at_unix_us,
    const gchar *rules_source,
    const gchar *journal_root_dir,
    const gchar *relation_name,
    GPtrArray **out_changes,
    GError **error);

G_END_DECLS
/* *INDENT-ON* */
