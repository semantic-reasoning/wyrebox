#pragma once

#include "wyrebox-journal-reader.h"
#include "wyrebox-local-object-store.h"

#include <glib-object.h>

/* *INDENT-OFF* */
G_BEGIN_DECLS

#define WYREBOX_TYPE_DELIVERY_REPLAY_VALIDATOR \
  (wyrebox_delivery_replay_validator_get_type())

G_DECLARE_FINAL_TYPE (WyreboxDeliveryReplayValidator,
    wyrebox_delivery_replay_validator,
    WYREBOX,
    DELIVERY_REPLAY_VALIDATOR,
    GObject)

typedef enum {
  WYREBOX_DELIVERY_REPLAY_VALIDATOR_ERROR_INVALID_RECORD,
  WYREBOX_DELIVERY_REPLAY_VALIDATOR_ERROR_MISSING_OBJECT,
  WYREBOX_DELIVERY_REPLAY_VALIDATOR_ERROR_SIZE_MISMATCH,
  WYREBOX_DELIVERY_REPLAY_VALIDATOR_ERROR_HASH_MISMATCH,
} WyreboxDeliveryReplayValidatorError;

#define WYREBOX_DELIVERY_REPLAY_VALIDATOR_ERROR \
  (wyrebox_delivery_replay_validator_error_quark ())

GQuark wyrebox_delivery_replay_validator_error_quark (void);

/*
 * @journal_reader: (transfer none): replay reader to consume from its current
 *   position through EOF.
 * @object_store: (transfer none): immutable raw object store used to verify
 *   MessageDelivered object references.
 *
 * Returns: (transfer full): a replay validator holding references to both
 * dependencies.
 */
WyreboxDeliveryReplayValidator *wyrebox_delivery_replay_validator_new (
    WyreboxJournalReader *journal_reader,
    WyreboxLocalObjectStore *object_store);

/*
 * Reads records to EOF. For this replay validation phase, only
 * MessageDelivered records are decoded and checked; all other event types are
 * skipped.
 */
gboolean wyrebox_delivery_replay_validator_validate_all (
    WyreboxDeliveryReplayValidator *self,
    GError **error);

G_END_DECLS
/* *INDENT-ON* */
