#pragma once

#include "wyrebox-eml-ingestor.h"

#include <glib-object.h>

/* *INDENT-OFF* */
G_BEGIN_DECLS

typedef struct
{
  /*
   * Request identity copied into the daemon success frame.
   *
   * Ownership: caller owns and must clear with
   * wyrebox_daemon_success_receipt_clear().
   */
  char *request_id;

  /*
   * Stable operation-specific durable marker. Delivery ingestion uses
   * journal:<offset>:<sequence>.
   */
  char *durable_marker;

  /*
   * Canonical mutation journal receipt for operations backed by the journal.
   */
  guint64 journal_offset;
  guint64 journal_sequence;

  /*
   * Human-readable operational summary for logs and diagnostics.
   */
  char *summary;
} WyreboxDaemonSuccessReceipt;

/*
 * Clears owned fields in @receipt and leaves it reusable as an empty receipt.
 */
void wyrebox_daemon_success_receipt_clear (
    WyreboxDaemonSuccessReceipt *receipt);

G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (WyreboxDaemonSuccessReceipt,
    wyrebox_daemon_success_receipt_clear)

/*
 * Builds the durable success receipt for delivery ingestion.
 *
 * Delivery ingestion is valid only after a durable raw object commit and a
 * durable MessageDelivered journal append. Therefore @ingest_result must carry
 * an object key and non-zero journal sequence. A zero journal offset is valid
 * for the first record in a segment.
 */
gboolean wyrebox_daemon_success_receipt_init_delivery_ingestion (
    WyreboxDaemonSuccessReceipt *receipt,
    const char *request_id,
    const WyreboxEmlIngestResult *ingest_result,
    GError **error);

G_END_DECLS
/* *INDENT-ON* */
