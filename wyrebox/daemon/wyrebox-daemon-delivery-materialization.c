#include "wyrebox-daemon-delivery-materialization.h"

#include "wyrebox-delivery-catchup.h"
#include "wyrebox-delivery-materializer.h"
#include "wyrebox-journal-reader.h"
#include "wyrebox-schema-metadata-store.h"

#include <gio/gio.h>

#define DEFAULT_RETRY_INITIAL_MS 5000
#define DEFAULT_RETRY_MAX_MS (5 * 60 * 1000)

typedef enum
{
    PASS_CATCH_UP,
    PASS_POST_INGEST,
    PASS_RETRY,
} PassKind;

struct _WyreboxDaemonDeliveryMaterialization
{
    GObject parent_instance;

    char *journal_root_dir;
    WyreboxJournalWriter *journal_writer;
    WyreboxLocalObjectStore *object_store;
    WyreboxSchemaMetadataStore *metadata_store;
    WyreboxDeliveryMaterializer *materializer;
    GMainContext *context;
    WyreboxDaemonDeliveryMaterializationClockFunc clock;
    gpointer clock_data;

    /*
     * Serializes catch-ups and guards every field below.
     */
    GMutex lock;
    GSource *retry_source;
    guint retry_initial_ms;
    guint retry_max_ms;
    guint retry_interval_ms;
    guint consecutive_failures;
    gboolean aborted;
    gboolean stopped;
    GPtrArray *holds;
    WyreboxDeliveryCatchupCursor cursor;
    gchar *last_error;
    gchar *warned_signature;
    gint64 warned_at;
};

/*
 * What a finished pass leaves to do once self->lock is released.
 */
typedef struct
{
    GLogLevelFlags level;
    gchar *text;
    GSource *cancelled_retry;
} PassOutcome;

static const WyreboxDeliveryCatchupCursor no_cursor = { 0 };

G_DEFINE_TYPE (WyreboxDaemonDeliveryMaterialization,
    wyrebox_daemon_delivery_materialization, G_TYPE_OBJECT)

static GPtrArray *
new_hold_array (void)
{
    return g_ptr_array_new_with_free_func (
        (GDestroyNotify)wyrebox_delivery_catchup_hold_free);
}

static gint64
monotonic_clock (gpointer user_data)
{
    (void)user_data;

    return g_get_monotonic_time ();
}

/*
 * Destroying the source may drop the last reference to @self, so it must
 * happen without holding self->lock.
 */
static void
destroy_retry_source (GSource *source)
{
    if (source != NULL) {
        g_source_destroy (source);
        g_source_unref (source);
    }
}

static void
stop_retries (WyreboxDaemonDeliveryMaterialization *self)
{
    GSource *source = NULL;

    g_mutex_lock (&self->lock);
    self->stopped = TRUE;
    source = g_steal_pointer (&self->retry_source);
    g_mutex_unlock (&self->lock);

    destroy_retry_source (source);
}

static void
wyrebox_daemon_delivery_materialization_dispose (GObject *object)
{
    WyreboxDaemonDeliveryMaterialization *self =
        WYREBOX_DAEMON_DELIVERY_MATERIALIZATION (object);

    stop_retries (self);

    g_clear_object (&self->materializer);
    g_clear_object (&self->metadata_store);
    g_clear_object (&self->object_store);
    g_clear_object (&self->journal_writer);
    g_clear_pointer (&self->context, g_main_context_unref);

    G_OBJECT_CLASS (wyrebox_daemon_delivery_materialization_parent_class)->
    dispose (object);
}

static void
wyrebox_daemon_delivery_materialization_finalize (GObject *object)
{
    WyreboxDaemonDeliveryMaterialization *self =
        WYREBOX_DAEMON_DELIVERY_MATERIALIZATION (object);

    g_clear_pointer (&self->journal_root_dir, g_free);
    g_clear_pointer (&self->holds, g_ptr_array_unref);
    g_clear_pointer (&self->last_error, g_free);
    g_clear_pointer (&self->warned_signature, g_free);
    g_mutex_clear (&self->lock);

    G_OBJECT_CLASS (wyrebox_daemon_delivery_materialization_parent_class)->
    finalize (object);
}

static void
wyrebox_daemon_delivery_materialization_class_init (
    WyreboxDaemonDeliveryMaterializationClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);

    object_class->dispose = wyrebox_daemon_delivery_materialization_dispose;
    object_class->finalize = wyrebox_daemon_delivery_materialization_finalize;
}

static void
wyrebox_daemon_delivery_materialization_init (
    WyreboxDaemonDeliveryMaterialization *self)
{
    g_mutex_init (&self->lock);
    self->clock = monotonic_clock;
    self->retry_initial_ms = DEFAULT_RETRY_INITIAL_MS;
    self->retry_max_ms = DEFAULT_RETRY_MAX_MS;
    self->retry_interval_ms = DEFAULT_RETRY_INITIAL_MS;
    self->holds = new_hold_array ();
}

WyreboxDaemonDeliveryMaterialization *
wyrebox_daemon_delivery_materialization_new (const char *catalog_path,
    const char *journal_root_dir, WyreboxJournalWriter *journal_writer,
    WyreboxLocalObjectStore *object_store, GError **error)
{
    g_autoptr (WyreboxDaemonDeliveryMaterialization) self = NULL;

    g_return_val_if_fail (catalog_path != NULL && *catalog_path != '\0', NULL);
    g_return_val_if_fail (journal_root_dir != NULL && *journal_root_dir != '\0',
        NULL);
    g_return_val_if_fail (WYREBOX_IS_JOURNAL_WRITER (journal_writer), NULL);
    g_return_val_if_fail (WYREBOX_IS_LOCAL_OBJECT_STORE (object_store), NULL);
    g_return_val_if_fail (error == NULL || *error == NULL, NULL);

    self = g_object_new (WYREBOX_TYPE_DAEMON_DELIVERY_MATERIALIZATION, NULL);
    self->journal_root_dir = g_strdup (journal_root_dir);
    self->journal_writer = g_object_ref (journal_writer);
    self->object_store = g_object_ref (object_store);
    self->context = g_main_context_ref_thread_default ();

    self->metadata_store =
        wyrebox_schema_metadata_store_new_duckdb (catalog_path, error);
    if (self->metadata_store == NULL)
        return NULL;

    self->materializer = wyrebox_delivery_materializer_new_duckdb (catalog_path,
            error);
    if (self->materializer == NULL)
        return NULL;

    return g_steal_pointer (&self);
}

/*
 * Post-ingest passes continue from the scan cursor with the current holds;
 * every other pass is a full pass from the checkpoint with no holds.
 */
static gboolean
run_pass_locked (WyreboxDaemonDeliveryMaterialization *self, PassKind kind,
    WyreboxDeliveryCatchupReport *out_report, GError **error)
{
    g_autoptr (WyreboxJournalReader) reader = NULL;
    guint64 durable_end = 0;
    gboolean resume = kind == PASS_POST_INGEST;

    /*
     * A resumed pass carries the holds into its report, so it can never be
     * mistaken for a clean pass from the checkpoint.
     */
    g_assert (!self->cursor.present || self->holds->len > 0);

    durable_end = wyrebox_journal_writer_get_durable_end (self->journal_writer);
    reader = wyrebox_journal_reader_new_with_limit (self->journal_root_dir,
            durable_end, error);
    if (reader == NULL)
        return FALSE;

    return wyrebox_delivery_catchup_materialize_account_inboxes_resumed (
        self->metadata_store, reader, self->object_store, self->materializer,
        resume ? &self->cursor : NULL, resume ? self->holds : NULL, out_report,
        error);
}

static gboolean retry_catch_up (gpointer user_data);

static void
schedule_retry_locked (WyreboxDaemonDeliveryMaterialization *self)
{
    g_assert (self->retry_source == NULL);

    self->retry_source = g_timeout_source_new (self->retry_interval_ms);
    g_source_set_callback (self->retry_source, retry_catch_up,
        g_object_ref (self), g_object_unref);
    g_source_attach (self->retry_source, self->context);
}

static gint
compare_strings (gconstpointer a, gconstpointer b)
{
    return g_strcmp0 (*(const gchar * const *)a, *(const gchar * const *)b);
}

/*
 * Identifies a failure by held account and error, not by the hold's journal
 * position, so a hold that persists while new deliveries arrive is not
 * reported as a new failure. Abort messages may embed journal offsets.
 */
static gchar *
failure_signature (const WyreboxDeliveryCatchupReport *report,
    const GError *abort_error)
{
    g_autoptr (GPtrArray) parts = NULL;

    if (abort_error != NULL) {
        return g_strdup_printf ("abort\x1f%s\x1f%d\x1f%s",
                   g_quark_to_string (abort_error->domain), abort_error->code,
                   abort_error->message);
    }

    parts = g_ptr_array_new_with_free_func (g_free);
    for (guint i = 0; i < report->holds->len; i++) {
        const WyreboxDeliveryCatchupHold *hold =
            g_ptr_array_index (report->holds, i);

        g_ptr_array_add (parts, g_strdup_printf ("%s\x1f%s", hold->account_id,
            hold->error->message));
    }
    g_ptr_array_sort (parts, compare_strings);
    g_ptr_array_add (parts, NULL);

    return g_strjoinv ("\x1e", (gchar **)parts->pdata);
}

static gchar *
describe_failure (const WyreboxDeliveryCatchupReport *report,
    const GError *abort_error)
{
    g_autoptr (GString) text = g_string_new ("delivery materialization ");

    if (abort_error != NULL) {
        g_string_append_printf (text, "failed: %s", abort_error->message);
        return g_string_free (g_steal_pointer (&text), FALSE);
    }

    for (guint i = 0; i < report->holds->len; i++) {
        const WyreboxDeliveryCatchupHold *hold =
            g_ptr_array_index (report->holds, i);

        if (i > 0)
            g_string_append (text, "; ");
        g_string_append_printf (text, "held account %s at journal offset %"
            G_GUINT64_FORMAT " sequence %" G_GUINT64_FORMAT ": %s",
            hold->account_id, hold->journal_offset, hold->journal_sequence,
            hold->error->message);
    }

    return g_string_free (g_steal_pointer (&text), FALSE);
}

static GStrv
held_account_list (const GPtrArray *holds)
{
    GStrv accounts = g_new0 (gchar *, holds->len + 1);

    for (guint i = 0; i < holds->len; i++) {
        const WyreboxDeliveryCatchupHold *hold = g_ptr_array_index (holds, i);

        accounts[i] = g_strdup (hold->account_id);
    }

    return accounts;
}

static void
finish_clean_pass_locked (WyreboxDaemonDeliveryMaterialization *self,
    PassOutcome *outcome)
{
    if (self->consecutive_failures > 0) {
        outcome->level = G_LOG_LEVEL_MESSAGE;
        outcome->text = g_strdup ("delivery materialization recovered");
    }

    self->consecutive_failures = 0;
    self->aborted = FALSE;
    self->retry_interval_ms = self->retry_initial_ms;
    g_ptr_array_set_size (self->holds, 0);
    self->cursor = no_cursor;
    g_clear_pointer (&self->last_error, g_free);
    g_clear_pointer (&self->warned_signature, g_free);
    outcome->cancelled_retry = g_steal_pointer (&self->retry_source);
}

/*
 * Records a failed pass. @report is the completed pass with holds, which
 * replace the current ones and move the scan cursor to the end of the pass, or
 * NULL when @abort_error aborted it; an aborted pass keeps the current holds
 * and resets the cursor to the checkpoint.
 */
static void
finish_failed_pass_locked (WyreboxDaemonDeliveryMaterialization *self,
    PassKind kind, WyreboxDeliveryCatchupReport *report,
    const GError *abort_error, PassOutcome *outcome)
{
    g_autofree gchar *signature = failure_signature (report, abort_error);
    gint64 now = self->clock (self->clock_data);
    gboolean warn = FALSE;

    self->aborted = abort_error != NULL;
    g_free (self->last_error);
    self->last_error = describe_failure (report, abort_error);
    if (report != NULL) {
        g_ptr_array_unref (self->holds);
        self->holds = g_steal_pointer (&report->holds);
        self->cursor = report->scanned_through;
    } else {
        self->cursor = no_cursor;
    }

    if (kind == PASS_RETRY) {
        self->consecutive_failures++;
        self->retry_interval_ms = self->retry_interval_ms >
            self->retry_max_ms / 2
            ? self->retry_max_ms : self->retry_interval_ms * 2;
    } else if (self->consecutive_failures == 0) {
        self->consecutive_failures = 1;
    }

    if (!self->stopped && self->retry_source == NULL)
        schedule_retry_locked (self);

    warn = self->stopped ||
        g_strcmp0 (signature, self->warned_signature) != 0 ||
        now - self->warned_at >=
        (gint64)self->retry_max_ms * G_TIME_SPAN_MILLISECOND;
    if (warn) {
        g_free (self->warned_signature);
        self->warned_signature = g_steal_pointer (&signature);
        self->warned_at = now;
    }

    outcome->level = warn ? G_LOG_LEVEL_WARNING : G_LOG_LEVEL_DEBUG;
    if (self->stopped) {
        outcome->text = g_strdup_printf ("%s; service stopped, no retry "
                "scheduled", self->last_error);
    } else {
        outcome->text = g_strdup_printf ("%s; retry in %u ms",
                self->last_error, self->retry_interval_ms);
    }
}

static void
finish_pass_locked (WyreboxDaemonDeliveryMaterialization *self,
    PassKind kind, WyreboxDeliveryCatchupReport *report,
    const GError *abort_error, PassOutcome *outcome)
{
    if (abort_error == NULL && report->holds->len == 0)
        finish_clean_pass_locked (self, outcome);
    else
        finish_failed_pass_locked (self, kind, report, abort_error, outcome);
}

static void
complete_outcome (PassOutcome *outcome)
{
    destroy_retry_source (g_steal_pointer (&outcome->cancelled_retry));

    switch (outcome->level) {
    case G_LOG_LEVEL_WARNING:
        g_warning ("%s", outcome->text);
        break;
    case G_LOG_LEVEL_MESSAGE:
        g_message ("%s", outcome->text);
        break;
    case G_LOG_LEVEL_DEBUG:
        g_debug ("%s", outcome->text);
        break;
    default:
        break;
    }
    g_clear_pointer (&outcome->text, g_free);
}

static void
run_and_finish_pass_locked (WyreboxDaemonDeliveryMaterialization *self,
    PassKind kind, PassOutcome *outcome)
{
    g_auto (WyreboxDeliveryCatchupReport) report = { 0 };
    g_autoptr (GError) error = NULL;

    if (run_pass_locked (self, kind, &report, &error))
        finish_pass_locked (self, kind, &report, NULL, outcome);
    else
        finish_pass_locked (self, kind, NULL, error, outcome);
}

gboolean
wyrebox_daemon_delivery_materialization_catch_up (
    WyreboxDaemonDeliveryMaterialization *self, GError **error)
{
    g_auto (WyreboxDeliveryCatchupReport) report = { 0 };
    PassOutcome outcome = { 0 };

    g_return_val_if_fail (WYREBOX_IS_DAEMON_DELIVERY_MATERIALIZATION (self),
        FALSE);
    g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

    g_mutex_lock (&self->lock);
    if (!run_pass_locked (self, PASS_CATCH_UP, &report, error)) {
        self->cursor = no_cursor;
        g_mutex_unlock (&self->lock);
        return FALSE;
    }
    finish_pass_locked (self, PASS_CATCH_UP, &report, NULL, &outcome);
    g_mutex_unlock (&self->lock);

    complete_outcome (&outcome);
    return TRUE;
}

static gboolean
retry_catch_up (gpointer user_data)
{
    WyreboxDaemonDeliveryMaterialization *self = user_data;
    PassOutcome outcome = { 0 };

    g_mutex_lock (&self->lock);
    if (self->stopped || self->retry_source != g_main_current_source ()) {
        g_mutex_unlock (&self->lock);
        return G_SOURCE_REMOVE;
    }
    g_clear_pointer (&self->retry_source, g_source_unref);
    run_and_finish_pass_locked (self, PASS_RETRY, &outcome);
    g_mutex_unlock (&self->lock);

    complete_outcome (&outcome);
    return G_SOURCE_REMOVE;
}

void
wyrebox_daemon_delivery_materialization_catch_up_or_schedule_retry (
    WyreboxDaemonDeliveryMaterialization *self)
{
    PassOutcome outcome = { 0 };

    g_return_if_fail (WYREBOX_IS_DAEMON_DELIVERY_MATERIALIZATION (self));

    g_mutex_lock (&self->lock);
    run_and_finish_pass_locked (self, PASS_POST_INGEST, &outcome);
    g_mutex_unlock (&self->lock);

    complete_outcome (&outcome);
}

void
wyrebox_daemon_delivery_materialization_stop (
    WyreboxDaemonDeliveryMaterialization *self)
{
    g_return_if_fail (WYREBOX_IS_DAEMON_DELIVERY_MATERIALIZATION (self));

    stop_retries (self);
}

gboolean
wyrebox_daemon_delivery_materialization_is_retry_pending (
    WyreboxDaemonDeliveryMaterialization *self)
{
    gboolean pending = FALSE;

    g_return_val_if_fail (WYREBOX_IS_DAEMON_DELIVERY_MATERIALIZATION (self),
        FALSE);

    g_mutex_lock (&self->lock);
    pending = self->retry_source != NULL;
    g_mutex_unlock (&self->lock);

    return pending;
}

void
wyrebox_daemon_delivery_materialization_status_clear (
    WyreboxDaemonDeliveryMaterializationStatus *status)
{
    if (status == NULL)
        return;

    g_clear_pointer (&status->held_accounts, g_strfreev);
    g_clear_pointer (&status->last_error, g_free);
}

void
wyrebox_daemon_delivery_materialization_get_status (
    WyreboxDaemonDeliveryMaterialization *self,
    WyreboxDaemonDeliveryMaterializationStatus *out_status)
{
    g_return_if_fail (WYREBOX_IS_DAEMON_DELIVERY_MATERIALIZATION (self));
    g_return_if_fail (out_status != NULL);

    g_mutex_lock (&self->lock);
    if (self->consecutive_failures == 0)
        out_status->state = WYREBOX_DAEMON_DELIVERY_MATERIALIZATION_STATE_OK;
    else if (self->aborted)
        out_status->state =
            WYREBOX_DAEMON_DELIVERY_MATERIALIZATION_STATE_RETRYING;
    else
        out_status->state = WYREBOX_DAEMON_DELIVERY_MATERIALIZATION_STATE_HELD;
    out_status->consecutive_failures = self->consecutive_failures;
    out_status->next_retry_interval_ms = self->retry_interval_ms;
    out_status->held_accounts = held_account_list (self->holds);
    out_status->last_error = g_strdup (self->last_error);
    g_mutex_unlock (&self->lock);
}

void
wyrebox_daemon_delivery_materialization_set_retry_backoff (
    WyreboxDaemonDeliveryMaterialization *self, guint initial_ms,
    guint max_ms)
{
    g_return_if_fail (WYREBOX_IS_DAEMON_DELIVERY_MATERIALIZATION (self));
    g_return_if_fail (initial_ms > 0 && max_ms >= initial_ms);

    g_mutex_lock (&self->lock);
    self->retry_initial_ms = initial_ms;
    self->retry_max_ms = max_ms;
    self->retry_interval_ms = initial_ms;
    g_mutex_unlock (&self->lock);
}

void
wyrebox_daemon_delivery_materialization_set_clock (
    WyreboxDaemonDeliveryMaterialization *self,
    WyreboxDaemonDeliveryMaterializationClockFunc clock, gpointer user_data)
{
    g_return_if_fail (WYREBOX_IS_DAEMON_DELIVERY_MATERIALIZATION (self));
    g_return_if_fail (clock != NULL);

    g_mutex_lock (&self->lock);
    self->clock = clock;
    self->clock_data = user_data;
    g_mutex_unlock (&self->lock);
}
