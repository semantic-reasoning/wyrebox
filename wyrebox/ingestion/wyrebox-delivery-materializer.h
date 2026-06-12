#pragma once

#include "wyrebox-delivery-projection.h"

#include <glib-object.h>

/* *INDENT-OFF* */
G_BEGIN_DECLS

#define WYREBOX_TYPE_DELIVERY_MATERIALIZER (wyrebox_delivery_materializer_get_type())

G_DECLARE_FINAL_TYPE (WyreboxDeliveryMaterializer,
    wyrebox_delivery_materializer,
    WYREBOX,
    DELIVERY_MATERIALIZER,
    GObject)

/*
 * Construct a DuckDB-backed delivery materializer for @path.
 *
 * The target database must already contain the bootstrap catalog created by the
 * schema migration layer. The materializer does not expose arbitrary SQL or own
 * schema orchestration.
 *
 * Returns: (transfer full): a non-floating GObject reference owned by the
 * caller, or NULL with @error set.
 */
WyreboxDeliveryMaterializer *wyrebox_delivery_materializer_new_duckdb (
    const gchar *path,
    GError **error);

/*
 * Apply @projection into one ordinary mailbox.
 *
 * The operation is transactional and idempotent for previously materialized
 * journal records. Previously materialized mailbox memberships keep their
 * assigned UIDs. New mailbox memberships receive monotonically increasing UIDs
 * from the current mailbox namespace uidnext in projection order.
 */
gboolean wyrebox_delivery_materializer_apply_to_mailbox (
    WyreboxDeliveryMaterializer *self,
    const gchar *account_id,
    const gchar *mailbox_id,
    const gchar *imap_name,
    const WyreboxDeliveryProjectionList *projection,
    GError **error);

G_END_DECLS
/* *INDENT-ON* */
