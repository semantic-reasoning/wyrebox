#pragma once

#include "wyrebox-journal-writer.h"
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

typedef struct
{
  gchar *account_id;
  gchar *view_id;
  gchar *message_id;
  gchar *membership_id;
  gchar *rule_version_hash;
  guint64 uid;
  guint64 uidvalidity;
  gboolean is_visible;
  guint64 materialized_at_unix_us;
} WyreboxDerivedViewMembershipChange;

void wyrebox_derived_view_membership_change_clear (
    WyreboxDerivedViewMembershipChange *change);

void wyrebox_derived_view_membership_change_free (
    WyreboxDerivedViewMembershipChange *change);

/*
 * Appends already-committed derived view membership changes to @writer in
 * @changes order.
 *
 * This helper is intentionally outside the materializer's DuckDB transaction:
 * callers should invoke it only after the projection commit succeeds. Journal
 * append or payload encode failure is reported as FALSE with @error set, and no
 * DuckDB rollback or materializer repair is attempted. These records are an
 * audit/change stream for committed projection changes; they are not the sole
 * canonical rebuild source for derived view state.
 */
gboolean wyrebox_derived_view_membership_changes_append_journal (
    GPtrArray *changes,
    WyreboxJournalWriter *writer,
    GError **error);

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

gboolean wyrebox_derived_view_materializer_apply_memberships_with_changes (
    WyreboxDerivedViewMaterializer *self,
    const gchar *account_id,
    const gchar *view_id,
    const gchar *imap_name,
    const gchar *definition_ref,
    const gchar *rule_version_hash,
    guint64 materialized_at_unix_us,
    GPtrArray *memberships,
    GPtrArray **out_changes,
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

gboolean wyrebox_derived_view_materializer_refresh_memberships_with_changes (
    WyreboxDerivedViewMaterializer *self,
    const gchar *account_id,
    const gchar *view_id,
    const gchar *imap_name,
    const gchar *definition_ref,
    const gchar *rule_version_hash,
    guint64 materialized_at_unix_us,
    GPtrArray *memberships,
    GPtrArray **out_changes,
    GError **error);

gboolean wyrebox_derived_view_materializer_refresh_current_rule_version_with_changes (
    WyreboxDerivedViewMaterializer *self,
    const gchar *account_id,
    const gchar *view_id,
    const gchar *imap_name,
    const gchar *definition_ref,
    const gchar *rule_version_hash,
    guint64 materialized_at_unix_us,
    GPtrArray *memberships,
    GPtrArray **out_changes,
    GError **error);

G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (WyreboxDerivedViewMembershipChange,
    wyrebox_derived_view_membership_change_clear)

G_END_DECLS
/* *INDENT-ON* */
