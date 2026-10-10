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

typedef enum
{
  WYREBOX_DAEMON_DELIVERY_MATERIALIZATION_STATE_OK,
  WYREBOX_DAEMON_DELIVERY_MATERIALIZATION_STATE_RETRYING,
  WYREBOX_DAEMON_DELIVERY_MATERIALIZATION_STATE_HELD,
} WyreboxDaemonDeliveryMaterializationState;

/*
 * Snapshot of the materialization failure state.
 *
 * @state: OK after a clean pass, RETRYING after a pass aborted, HELD after a
 *   pass completed with held accounts.
 * @consecutive_failures: failures since the last clean pass, counting the
 *   first failure and each failed retry; 0 when @state is OK.
 * @next_retry_interval_ms: delay of the pending retry, or of the next one to
 *   be scheduled; still reported after
 *   wyrebox_daemon_delivery_materialization_stop(), although no retry is
 *   scheduled then.
 * @held_accounts: (owned): NULL-terminated held accounts, in journal order of
 *   their first unmaterialized delivery; kept after an aborted pass.
 * @last_error: (owned) (nullable): description of the last failure, NULL
 *   when @state is OK.
 */
typedef struct
{
  WyreboxDaemonDeliveryMaterializationState state;
  guint consecutive_failures;
  guint next_retry_interval_ms;
  GStrv held_accounts;
  gchar *last_error;
} WyreboxDaemonDeliveryMaterializationStatus;

void wyrebox_daemon_delivery_materialization_status_clear (
    WyreboxDaemonDeliveryMaterializationStatus *status);

G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (WyreboxDaemonDeliveryMaterializationStatus,
    wyrebox_daemon_delivery_materialization_status_clear)

/*
 * Monotonic clock in microseconds, used to rate-limit failure warnings.
 */
typedef gint64 (*WyreboxDaemonDeliveryMaterializationClockFunc) (
    gpointer user_data);

/*
 * Runs a full pass: materializes every durable delivery past the persisted
 * checkpoint, as described by
 * wyrebox_delivery_catchup_materialize_account_inboxes_isolated(), and
 * replaces the current holds with the pass's holds.
 *
 * An account whose INBOX run fails with G_IO_ERROR_INVALID_DATA, for example
 * because its existing INBOX is not selectable or its UIDVALIDITY differs, or
 * whose run references a raw object that is missing, corrupt, or unreadable,
 * is held: other accounts keep materializing, the hold is logged and a retry is
 * scheduled as described for
 * wyrebox_daemon_delivery_materialization_catch_up_or_schedule_retry(), and
 * the call still returns TRUE.
 *
 * Returns FALSE with @error set when the pass aborts, for example when a
 * pending delivery has no account identity (G_IO_ERROR_INVALID_DATA) or the
 * catalog cannot be written. Nothing is logged or scheduled then, and the
 * status is unchanged. Committed runs stay committed and the checkpoint never
 * moves past uncommitted work, so calling again after the cause is fixed
 * resumes where it stopped.
 *
 * Must not be called while holding the journal writer's append lock, for
 * example from a WyreboxJournalWriterGuardedAppendFunc.
 */
gboolean wyrebox_daemon_delivery_materialization_catch_up (
    WyreboxDaemonDeliveryMaterialization *self,
    GError **error);

/*
 * Runs a catch-up pass after a delivery. While accounts are held, the pass
 * continues after the last delivery the previous pass replayed and skips the
 * held accounts, so they stay held until a full pass. An aborted pass keeps
 * the holds and makes the next pass start at the checkpoint again.
 *
 * When the pass aborts or holds an account, logs a warning and, unless one is
 * already pending, schedules a retry; a pending retry is never postponed.
 * Every abort is treated as temporary here; deliveries stay durable in the
 * journal regardless.
 *
 * Each retry is a one-shot full pass from the checkpoint whose holds replace
 * the current ones, like
 * wyrebox_daemon_delivery_materialization_catch_up(). A failed retry schedules
 * the next one
 * with twice the previous delay, capped at the maximum interval. A clean pass
 * cancels any pending retry, resets the interval, and logs the recovery.
 * Warnings carry each held account with the journal offset and sequence of
 * its first unmaterialized delivery. A warning is logged when the failure
 * changes and otherwise at most once per maximum interval; repeated identical
 * failures are logged at debug level.
 *
 * Safe to call from any thread, with the same append-lock restriction as
 * wyrebox_daemon_delivery_materialization_catch_up(). Callers on other threads
 * and the scheduled retry contend for the same internal lock, so a call can
 * block for the duration of an in-progress catch-up.
 *
 * After wyrebox_daemon_delivery_materialization_stop(), every failure is
 * logged as a warning but no retry is scheduled.
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
 * Fills @out_status with a snapshot of the failure state. Safe to call from
 * any thread.
 *
 * @out_status: (out caller-allocates): zero-initialized status; clear it with
 *   wyrebox_daemon_delivery_materialization_status_clear().
 */
void wyrebox_daemon_delivery_materialization_get_status (
    WyreboxDaemonDeliveryMaterialization *self,
    WyreboxDaemonDeliveryMaterializationStatus *out_status);

/*
 * Sets the first retry delay and the cap in milliseconds; @max_ms is also the
 * warning rate-limit window. Defaults to 5000 and 300000. Call before the
 * first catch-up.
 */
void wyrebox_daemon_delivery_materialization_set_retry_backoff (
    WyreboxDaemonDeliveryMaterialization *self,
    guint initial_ms,
    guint max_ms);

/*
 * Replaces the clock used to rate-limit warnings; defaults to
 * g_get_monotonic_time(). @user_data is not owned and must outlive @self.
 */
void wyrebox_daemon_delivery_materialization_set_clock (
    WyreboxDaemonDeliveryMaterialization *self,
    WyreboxDaemonDeliveryMaterializationClockFunc clock,
    gpointer user_data);

/*
 * Recomputes state derived from the materialized catalog for @account_id,
 * such as virtual mailbox membership. Called with the internal lock held, so
 * it must not call back into the materialization service.
 */
typedef gboolean (*WyreboxDaemonDeliveryMaterializationRefreshFunc) (
    const char *account_id,
    gpointer user_data,
    GError **error);

/*
 * Installs @refresh, which every pass that does not abort runs for each
 * account it materialized a record for and each account still queued, except
 * accounts the pass holds. A failed refresh holds the account like a failed
 * run: the pass reports it in the status and failure log, a retry is scheduled,
 * and the account stays queued until a refresh succeeds. A refresh hold does
 * not stop the account's records from materializing.
 *
 * Without a refresh function, queued accounts are dropped after each pass.
 * Takes ownership of @user_data and releases it with @user_data_destroy when
 * replaced or when @self is disposed.
 */
void wyrebox_daemon_delivery_materialization_set_refresh_func (
    WyreboxDaemonDeliveryMaterialization *self,
    WyreboxDaemonDeliveryMaterializationRefreshFunc refresh,
    gpointer user_data,
    GDestroyNotify user_data_destroy);

/*
 * Journals the derived facts of @account_id's materialized messages that have
 * not been extracted yet and stores in @out_appended how many records it
 * appended, also when it fails. Called with the internal lock held, so it must not call back into
 * the materialization service.
 */
typedef gboolean (*WyreboxDaemonDeliveryMaterializationExtractFunc) (
    const char *account_id,
    guint *out_appended,
    gpointer user_data,
    GError **error);

/*
 * Installs @extract, which every pass that does not abort runs before the
 * refresh function for the same accounts. When it appends records, the pass
 * runs once more, the same way, so the appended records are materialized
 * before any account is refreshed.
 *
 * A failed extraction holds the account like a failed refresh, with an error
 * prefixed by "fact extraction failed: ", and the account is not refreshed
 * by that pass. It stays queued, so the next pass extracts it again.
 *
 * Takes ownership of @user_data and releases it with @user_data_destroy when
 * replaced or when @self is disposed.
 */
void wyrebox_daemon_delivery_materialization_set_extract_func (
    WyreboxDaemonDeliveryMaterialization *self,
    WyreboxDaemonDeliveryMaterializationExtractFunc extract,
    gpointer user_data,
    GDestroyNotify user_data_destroy);

/*
 * Queues @account_id for refresh by the next pass, for example after a rule
 * change at startup. Safe to call from any thread.
 */
void wyrebox_daemon_delivery_materialization_queue_refresh (
    WyreboxDaemonDeliveryMaterialization *self,
    const char *account_id);

G_END_DECLS
/* *INDENT-ON* */
