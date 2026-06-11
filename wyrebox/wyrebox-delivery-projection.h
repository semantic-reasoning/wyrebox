#pragma once

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
   * Journal location of the MessageDelivered record.
   */
  guint64 journal_offset;
  guint64 journal_sequence;

  /*
   * Immutable raw object reference for the delivered RFC 5322 bytes.
   *
   * Ownership: caller owns and must free with g_free() or
   * wyrebox_delivery_projection_record_clear().
   */
  char *object_key;

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
} WyreboxDeliveryProjectionRecord;

/*
 * A mutable ordered list of projected delivered messages.
 *
 * Ownership: caller owns each record entry and must call
 * wyrebox_delivery_projection_record_clear() before free().
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
 * EOF into an owned result list.
 */
gboolean wyrebox_delivery_projection_replay_all (
    WyreboxDeliveryProjection *self,
    WyreboxDeliveryProjectionList *out_projection,
    GError **error);

G_END_DECLS
/* *INDENT-ON* */
