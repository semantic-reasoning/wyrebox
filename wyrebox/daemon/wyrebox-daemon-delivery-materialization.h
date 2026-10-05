#pragma once

#include "wyrebox-journal-writer.h"
#include "wyrebox-local-object-store.h"

#include <glib-object.h>

/* *INDENT-OFF* */
G_BEGIN_DECLS

#define WYREBOX_TYPE_DAEMON_DELIVERY_MATERIALIZATION \
  (wyrebox_daemon_delivery_materialization_get_type())

G_DECLARE_FINAL_TYPE (WyreboxDaemonDeliveryMaterialization,
    wyrebox_daemon_delivery_materialization,
    WYREBOX,
    DAEMON_DELIVERY_MATERIALIZATION,
    GObject)

/*
 * Materializes journaled deliveries into the DuckDB catalog inside wyreboxd.
 *
 * Each catch-up replays only the journal prefix that @journal_writer reports as
 * durable, so records still being appended by concurrent deliveries are never
 * read. Deliveries land in the INBOX of the account recorded in their payload,
 * as resolved by wyrebox_delivery_materializer_apply_to_inbox(). Catch-ups are
 * serialized by an internal lock.
 *
 * Retries are scheduled on the thread-default main context of the thread that
 * calls this constructor. A pending retry holds a reference to the service;
 * call wyrebox_daemon_delivery_materialization_stop() on that thread to cancel
 * it before dropping the last caller reference.
 *
 * @catalog_path: DuckDB catalog, already prepared to the current schema.
 * @journal_root_dir: root of the journal @journal_writer appends to.
 * @journal_writer: (transfer none): live journal writer; a reference is kept.
 * @object_store: (transfer none): object store; a reference is kept.
 *
 * Returns: (transfer full): new service, or NULL with @error set.
 */
WyreboxDaemonDeliveryMaterialization *
wyrebox_daemon_delivery_materialization_new (
    const char *catalog_path,
    const char *journal_root_dir,
    WyreboxJournalWriter *journal_writer,
    WyreboxLocalObjectStore *object_store,
    GError **error);

/*
 * Materializes every durable delivery past the persisted checkpoint.
 *
 * Fails with G_IO_ERROR_INVALID_DATA when a pending delivery has no account
 * identity, or when the account's existing INBOX mailbox does not match the
 * materialized state WyreBox requires (for example it is not selectable, or
 * its UIDVALIDITY differs). Runs committed before a failure stay committed and
 * the checkpoint never moves past uncommitted work, so calling again after the
 * cause is fixed resumes where it stopped.
 *
 * Must not be called while holding the journal writer's append lock, for
 * example from a WyreboxJournalWriterGuardedAppendFunc.
 */
gboolean wyrebox_daemon_delivery_materialization_catch_up (
    WyreboxDaemonDeliveryMaterialization *self,
    GError **error);

/*
 * Runs wyrebox_daemon_delivery_materialization_catch_up(). On failure, logs a
 * warning and, unless one is already pending, schedules a retry that repeats
 * every retry interval until a catch-up succeeds. Every failure is treated as
 * temporary here; deliveries stay durable in the journal regardless.
 *
 * Safe to call from any thread, with the same append-lock restriction as
 * wyrebox_daemon_delivery_materialization_catch_up(). Callers on other threads
 * and the scheduled retry contend for the same internal lock, so a call can
 * block for the duration of an in-progress catch-up.
 *
 * After wyrebox_daemon_delivery_materialization_stop(), failures are logged
 * but no retry is scheduled.
 */
void wyrebox_daemon_delivery_materialization_catch_up_or_schedule_retry (
    WyreboxDaemonDeliveryMaterialization *self);

/*
 * Cancels any pending retry, releasing the reference it holds, and prevents
 * new retries from being scheduled. Call on the thread that created the
 * service, typically once its main loop has quit.
 */
void wyrebox_daemon_delivery_materialization_stop (
    WyreboxDaemonDeliveryMaterialization *self);

gboolean wyrebox_daemon_delivery_materialization_is_retry_pending (
    WyreboxDaemonDeliveryMaterialization *self);

/*
 * Sets the retry interval in milliseconds used for retries scheduled after
 * this call. Defaults to 5000.
 */
void wyrebox_daemon_delivery_materialization_set_retry_interval (
    WyreboxDaemonDeliveryMaterialization *self,
    guint interval_ms);

G_END_DECLS
/* *INDENT-ON* */
