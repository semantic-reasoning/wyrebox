#pragma once

#include "wyrebox-daemon-fact-mutation-request.h"
#include "wyrebox-delivery-projection.h"
#include "wyrebox-flag-changed-payload.h"

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
 *
 * Errors are classified as described for
 * wyrebox_delivery_materializer_apply_to_inbox_full().
 */
gboolean wyrebox_delivery_materializer_apply_to_mailbox (
    WyreboxDeliveryMaterializer *self,
    const gchar *account_id,
    const gchar *mailbox_id,
    const gchar *imap_name,
    const WyreboxDeliveryProjectionList *projection,
    GError **error);

/*
 * Apply @projection into the INBOX of @account_id, with the same transaction,
 * idempotency and UID rules as wyrebox_delivery_materializer_apply_to_mailbox().
 *
 * The account's existing mailbox named "INBOX" is reused when present;
 * otherwise mailbox "inbox:<account_id>" is created.
 */
gboolean wyrebox_delivery_materializer_apply_to_inbox (
    WyreboxDeliveryMaterializer *self,
    const gchar *account_id,
    const WyreboxDeliveryProjectionList *projection,
    GError **error);

/*
 * Like wyrebox_delivery_materializer_apply_to_inbox(), but the materialization
 * checkpoint is only moved forward to the last record of @projection when
 * @advance_checkpoint is TRUE.
 *
 * Failures caused by the account's mailbox, UID state or record rows,
 * including DuckDB constraint violations, are reported as
 * G_IO_ERROR_INVALID_DATA and are deterministic for @account_id. Other
 * failures, such as DuckDB I/O or lock errors, use other codes. On failure
 * nothing is committed.
 *
 * Re-applying an already materialized record is idempotent only while
 * delivery-created mailbox memberships are never deleted or hidden.
 */
gboolean wyrebox_delivery_materializer_apply_to_inbox_full (
    WyreboxDeliveryMaterializer *self,
    const gchar *account_id,
    const WyreboxDeliveryProjectionList *projection,
    gboolean advance_checkpoint,
    GError **error);

/*
 * Apply the FlagChanged record at @journal_offset/@journal_sequence to the
 * system flags and user keywords of the mailbox membership it targets, in one
 * transaction. The checkpoint is moved forward to the record when
 * @advance_checkpoint is TRUE.
 *
 * A row records the journal position of the change that added its name; a
 * name already present is left untouched. Re-applying any suffix of the
 * journal's flag changes in order therefore reproduces the same rows.
 *
 * A target whose membership or UIDVALIDITY is not materialized, and DuckDB
 * constraint violations, fail with G_IO_ERROR_INVALID_DATA. Other failures,
 * such as DuckDB I/O or lock errors, use other codes. On failure nothing is
 * committed.
 *
 * @payload: (transfer none): decoded FlagChanged payload.
 */
gboolean wyrebox_delivery_materializer_apply_flag_change (
    WyreboxDeliveryMaterializer *self,
    const WyreboxFlagChangedPayload *payload,
    guint64 journal_offset,
    guint64 journal_sequence,
    gboolean advance_checkpoint,
    GError **error);

/*
 * Apply the FactInserted or FactRetracted record at
 * @journal_offset/@journal_sequence to message_facts, in one transaction. The
 * checkpoint is moved forward to the record when @advance_checkpoint is TRUE.
 *
 * A fact belongs to the account named by @mutation's scope and is identified
 * by its source ("fact-mutation:<scope>"), predicate, and arguments; its
 * fact_id is "fact:" followed by the SHA-256 of that identity. The first
 * argument names the message, and object_id is that message's raw object, or
 * empty when the account has no such materialized message.
 *
 * Inserting an active fact and retracting an inactive one change nothing.
 * Retracting sets retracted_at_unix_us to @journal_sequence; inserting a
 * retracted fact reactivates it with created_at_unix_us set to
 * @journal_sequence. Either records the journal position of the change, so
 * re-applying any suffix of the journal's fact mutations in order reproduces
 * the same rows.
 *
 * A mutation without scope or predicate, a zero @journal_sequence, and DuckDB
 * constraint violations fail with G_IO_ERROR_INVALID_DATA. Other failures,
 * such as DuckDB I/O or lock errors, use other codes. On failure nothing is
 * committed.
 *
 * @mutation: (transfer none): decoded fact mutation payload.
 */
gboolean wyrebox_delivery_materializer_apply_fact_mutation (
    WyreboxDeliveryMaterializer *self,
    const WyreboxDaemonFactMutationRequest *mutation,
    guint64 journal_offset,
    guint64 journal_sequence,
    gboolean advance_checkpoint,
    GError **error);

G_END_DECLS
/* *INDENT-ON* */
