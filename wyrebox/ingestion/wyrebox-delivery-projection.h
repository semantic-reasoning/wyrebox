#pragma once

#include "wyrebox-daemon-fact-mutation-request.h"
#include "wyrebox-flag-changed-payload.h"
#include "wyrebox-journal-reader.h"
#include "wyrebox-local-object-store.h"

#include <glib-object.h>

/* *INDENT-OFF* */
G_BEGIN_DECLS

#define WYREBOX_TYPE_DELIVERY_PROJECTION (wyrebox_delivery_projection_get_type())

G_DECLARE_FINAL_TYPE (WyreboxDeliveryProjection,
    wyrebox_delivery_projection,
    WYREBOX,
    DELIVERY_PROJECTION,
    GObject)

typedef struct
{
  /*
   * Journal location of the MessageDelivered, FlagChanged, FactInserted, or
   * FactRetracted record.
   */
  guint64 journal_offset;
  guint64 journal_sequence;

  /*
   * Immutable raw object reference for the delivered RFC 5322 bytes.
   *
   * Ownership: owned by this record and cleared by
   * wyrebox_delivery_projection_record_clear(). When accessed through
   * WyreboxDeliveryProjectionList, this pointer is borrowed and valid until the
   * list is cleared or the record is removed from the list.
   */
  char *object_key;

  /*
   * Account the delivery was routed to, or NULL for payloads written before
   * account identity was journaled.
   *
   * Ownership: owned by this record and cleared by
   * wyrebox_delivery_projection_record_clear(). When accessed through
   * WyreboxDeliveryProjectionList, this pointer is borrowed and valid until the
   * list is cleared or the record is removed from the list.
   */
  char *account_identity;

  /*
   * Stored metadata from the MessageDelivered payload.
   */
  guint64 size_bytes;
  guint64 internal_date_unix_us;
  guint duplicate_message_id_count;
  char *rfc_message_id;
  char *subject;
  char *from;
  char *to;
  char *cc;
  char *bcc;
  char *date_raw;
  gboolean message_id_span_valid;
  guint64 message_id_span_start;
  guint64 message_id_span_end;
  gboolean subject_span_valid;
  guint64 subject_span_start;
  guint64 subject_span_end;

  /*
   * Decoded payload when the entry is a FlagChanged record, or NULL for a
   * MessageDelivered record. A flag change has no object key or delivery
   * metadata; @account_identity is its payload's account.
   *
   * Ownership: owned by this record and cleared by
   * wyrebox_delivery_projection_record_clear().
   */
  WyreboxFlagChangedPayload *flag_change;

  /*
   * Decoded payload when the entry is a FactInserted or FactRetracted record,
   * or NULL otherwise. A fact mutation has no object key or delivery
   * metadata; @account_identity is its scope.
   *
   * Ownership: owned by this record and cleared by
   * wyrebox_delivery_projection_record_clear().
   */
  WyreboxDaemonFactMutationRequest *fact_mutation;
} WyreboxDeliveryProjectionRecord;

/*
 * A mutable ordered list of projected delivered messages.
 *
 * Ownership: the list owns @records and every contained
 * WyreboxDeliveryProjectionRecord. Entries returned from @records are borrowed
 * while they remain in the list. Clear the list with
 * wyrebox_delivery_projection_list_clear() or g_auto().
 */
typedef struct
{
  GPtrArray *records;
} WyreboxDeliveryProjectionList;

void wyrebox_delivery_projection_record_clear (
    WyreboxDeliveryProjectionRecord *record);

/*
 * Clears owned list storage and all contained records.
 */
void wyrebox_delivery_projection_list_clear (
    WyreboxDeliveryProjectionList *projection);

G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (WyreboxDeliveryProjectionRecord,
    wyrebox_delivery_projection_record_clear)

G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (WyreboxDeliveryProjectionList,
    wyrebox_delivery_projection_list_clear)

/*
 * @journal_reader: (transfer none): replay reader from which MessageDelivered
 *   records are consumed through EOF.
 * @object_store: (transfer none): immutable object store used to verify that
 *   referenced objects exist.
 *
 * Returns: (transfer full): projection component holding references to both
 *   dependencies.
 */
WyreboxDeliveryProjection *wyrebox_delivery_projection_new (
    WyreboxJournalReader *journal_reader,
    WyreboxLocalObjectStore *object_store);

/*
 * Replays MessageDelivered records from the reader's current position through
 * EOF into an owned result list and checks every referenced raw object with
 * wyrebox_delivery_projection_check_record_object().
 *
 * @out_projection must be zero-initialized or already managed by
 * wyrebox_delivery_projection_list_clear(); any previous contents are cleared
 * before replay starts. On failure, partial projection contents are cleared and
 * @out_projection records is set to NULL.
 */
gboolean wyrebox_delivery_projection_replay_all (
    WyreboxDeliveryProjection *self,
    WyreboxDeliveryProjectionList *out_projection,
    GError **error);

/*
 * Like wyrebox_delivery_projection_replay_all() but reads and decodes the
 * records without reading their raw objects, so callers can check objects
 * per record with wyrebox_delivery_projection_check_record_object().
 */
gboolean wyrebox_delivery_projection_replay_records (
    WyreboxDeliveryProjection *self,
    WyreboxDeliveryProjectionList *out_projection,
    GError **error);

/*
 * Like wyrebox_delivery_projection_replay_records() but also projects, in
 * journal order with the deliveries, FlagChanged records as entries whose
 * @flag_change is set and FactInserted/FactRetracted records as entries whose
 * @fact_mutation is set. A payload that fails to decode, or a fact payload
 * whose mutation does not match its event type, fails the replay with
 * G_IO_ERROR_INVALID_DATA.
 */
gboolean wyrebox_delivery_projection_replay_records_with_mutations (
    WyreboxDeliveryProjection *self,
    WyreboxDeliveryProjectionList *out_projection,
    GError **error);

/*
 * Returns TRUE when @record projects a MessageDelivered record, which has a
 * raw object, rather than a flag change or fact mutation.
 */
gboolean wyrebox_delivery_projection_record_is_delivery (
    const WyreboxDeliveryProjectionRecord *record);

/*
 * Checks the raw object @record references.
 *
 * A missing object, a size or SHA-256 mismatch, or an invalid key does not
 * change on retry and fails with G_IO_ERROR_INVALID_DATA. Any other read
 * failure keeps the object store's domain and code, so callers can treat it
 * as transient.
 *
 * @object_store: (transfer none): store holding the raw object.
 * @record: (transfer none): projected delivery whose object is checked.
 */
gboolean wyrebox_delivery_projection_check_record_object (
    WyreboxLocalObjectStore *object_store,
    const WyreboxDeliveryProjectionRecord *record,
    GError **error);

G_END_DECLS
/* *INDENT-ON* */
