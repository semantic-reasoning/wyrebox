#pragma once

#include "wyrebox-delivery-materializer.h"
#include "wyrebox-journal-reader.h"
#include "wyrebox-local-object-store.h"
#include "wyrebox-schema-metadata-store.h"

#include <glib-object.h>

/* *INDENT-OFF* */
G_BEGIN_DECLS

/*
 * Replay journaled MessageDelivered records that are not covered by the
 * persisted materialization checkpoint and materialize them into the fixed
 * ordinary INBOX mailbox.
 *
 * @metadata_store: (transfer none): metadata source for the persisted
 *   materialization checkpoint.
 * @journal_reader: (transfer none): reader positioned at the beginning of the
 *   journal; this function advances it through replay.
 * @object_store: (transfer none): object store used by projection replay to
 *   verify immutable raw message objects.
 * @materializer: (transfer none): delivery materializer receiving the INBOX
 *   projection.
 * @account_id: account owning the fixed INBOX mailbox.
 */
gboolean wyrebox_delivery_catchup_materialize_inbox (
    WyreboxSchemaMetadataStore *metadata_store,
    WyreboxJournalReader *journal_reader,
    WyreboxLocalObjectStore *object_store,
    WyreboxDeliveryMaterializer *materializer,
    const gchar *account_id,
    GError **error);

G_END_DECLS
/* *INDENT-ON* */
