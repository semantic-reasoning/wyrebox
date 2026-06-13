#include "wyrebox-derived-view-materializer-wirelog.h"

#include "wyrebox-wirelog-derived-membership.h"
#include "wyrebox-wirelog-rule-version.h"

#include <gio/gio.h>

gboolean
    wyrebox_derived_view_materializer_refresh_from_rules_and_facts_with_changes
    (WyreboxDerivedViewMaterializer * self, const gchar * account_id,
    const gchar * view_id, const gchar * imap_name,
    const gchar * definition_ref, guint64 materialized_at_unix_us,
    const gchar * rules_source, GPtrArray * facts, const gchar * relation_name,
    GPtrArray ** out_changes, GError ** error)
{
  g_autoptr (GPtrArray) memberships = NULL;
  g_autofree gchar *rule_version_hash = NULL;

  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (out_changes != NULL)
    *out_changes = NULL;

  if (relation_name == NULL || relation_name[0] == '\0') {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "Wirelog derived view relation name is required");
    return FALSE;
  }

  rule_version_hash = wyrebox_wirelog_rule_version_hash (rules_source, error);
  if (rule_version_hash == NULL)
    return FALSE;

  memberships =
      wyrebox_wirelog_derived_membership_snapshot_from_rules_and_facts
      (rules_source, facts, relation_name, error);
  if (memberships == NULL)
    return FALSE;

  return
      wyrebox_derived_view_materializer_refresh_current_rule_version_with_changes
      (self, account_id, view_id, imap_name, definition_ref,
      rule_version_hash, materialized_at_unix_us, memberships, out_changes,
      error);
}
