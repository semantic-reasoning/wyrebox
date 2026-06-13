#pragma once

#include <glib-object.h>

typedef struct
{
  char *account_id;
  char *view_id;
  char *message_id;
  char *membership_id;
  char *rule_version_hash;
  guint64 uid;
  guint64 uidvalidity;
  gboolean is_visible;
  guint64 materialized_at_unix_us;
} WyreboxDerivedViewMembershipChangedPayload;

/* *INDENT-OFF* */
G_BEGIN_DECLS

void wyrebox_derived_view_membership_changed_payload_clear (
    WyreboxDerivedViewMembershipChangedPayload *payload);

GBytes *wyrebox_derived_view_membership_changed_payload_encode (
    const WyreboxDerivedViewMembershipChangedPayload *payload,
    GError **error);

gboolean wyrebox_derived_view_membership_changed_payload_decode (
    GBytes *bytes,
    WyreboxDerivedViewMembershipChangedPayload *out_payload,
    GError **error);

G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (
    WyreboxDerivedViewMembershipChangedPayload,
    wyrebox_derived_view_membership_changed_payload_clear)

G_END_DECLS
/* *INDENT-ON* */
