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
  WYREBOX_DELIVERY_REPLAY_VALIDATOR_ERROR_OBJECT_UNREADABLE,
} WyreboxDeliveryReplayValidatorError;

/*
 * Raw object failures found by
 * wyrebox_delivery_replay_validator_validate_all_report().
 *
 * @object_failure_count: MessageDelivered records whose raw object is
 *   missing or does not match the journaled size or SHA-256 key.
 * @first_object_failure_offset, @first_object_failure_sequence: the journal
 *   position of the first such record; zero when @object_failure_count is 0.
 * @first_object_failure_code: MISSING_OBJECT, SIZE_MISMATCH, or HASH_MISMATCH
 *   for the first such record; INVALID_RECORD when @object_failure_count is 0.
 */
typedef struct {
  guint64 object_failure_count;
  guint64 first_object_failure_offset;
  guint64 first_object_failure_sequence;
  WyreboxDeliveryReplayValidatorError first_object_failure_code;
} WyreboxDeliveryReplayValidatorReport;

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
 *
 * Fails on the first invalid record. A payload that does not decode fails
 * with G_IO_ERROR_INVALID_DATA. A raw object that is missing or does not match
 * the journaled size or SHA-256 key fails with MISSING_OBJECT, SIZE_MISMATCH,
 * or HASH_MISMATCH, an invalid object key with INVALID_RECORD, and any other
 * object read error with OBJECT_UNREADABLE. Journal read errors keep their
 * own domain and code.
 */
gboolean wyrebox_delivery_replay_validator_validate_all (
    WyreboxDeliveryReplayValidator *self,
    GError **error);

/*
 * Like wyrebox_delivery_replay_validator_validate_all(), but a raw object
 * that is missing or does not match the journaled size or SHA-256 key is
 * counted in @out_report and validation continues. Every other failure,
 * including OBJECT_UNREADABLE, still fails.
 *
 * @out_report: (out caller-allocates): reset before validation starts;
 *   valid only when the function returns TRUE.
 */
gboolean wyrebox_delivery_replay_validator_validate_all_report (
    WyreboxDeliveryReplayValidator *self,
    WyreboxDeliveryReplayValidatorReport *out_report,
    GError **error);

G_END_DECLS
/* *INDENT-ON* */
