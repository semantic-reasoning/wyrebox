#include "wyrebox-daemon-delivery-materialization.h"

#include "wyrebox-delivery-catchup.h"
#include "wyrebox-delivery-materializer.h"
#include "wyrebox-journal-reader.h"
#include "wyrebox-schema-metadata-store.h"

#include <gio/gio.h>

#define DEFAULT_RETRY_INTERVAL_MS 5000

struct _WyreboxDaemonDeliveryMaterialization
{
    GObject parent_instance;

    char *journal_root_dir;
    WyreboxJournalWriter *journal_writer;
    WyreboxLocalObjectStore *object_store;
    WyreboxSchemaMetadataStore *metadata_store;
    WyreboxDeliveryMaterializer *materializer;
    GMainContext *context;

    /*
     * Serializes catch-ups and guards retry_source, retry_interval_ms and
     * stopped.
     */
    GMutex lock;
    GSource *retry_source;
    guint retry_interval_ms;
    gboolean stopped;
};

G_DEFINE_TYPE (WyreboxDaemonDeliveryMaterialization,
    wyrebox_daemon_delivery_materialization, G_TYPE_OBJECT)

/*
 * Destroying the source may drop the last reference to @self, so it must
 * happen without holding self->lock.
 */
static void
stop_retries (WyreboxDaemonDeliveryMaterialization *self)
{
    GSource *source = NULL;

    g_mutex_lock (&self->lock);
    self->stopped = TRUE;
    source = g_steal_pointer (&self->retry_source);
    g_mutex_unlock (&self->lock);

    if (source != NULL) {
        g_source_destroy (source);
        g_source_unref (source);
    }
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
    self->retry_interval_ms = DEFAULT_RETRY_INTERVAL_MS;
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

static gboolean
catch_up_locked (WyreboxDaemonDeliveryMaterialization *self, GError **error)
{
    g_autoptr (WyreboxJournalReader) reader = NULL;
    guint64 durable_end = 0;

    durable_end = wyrebox_journal_writer_get_durable_end (self->journal_writer);
    reader = wyrebox_journal_reader_new_with_limit (self->journal_root_dir,
            durable_end, error);
    if (reader == NULL)
        return FALSE;

    return wyrebox_delivery_catchup_materialize_account_inboxes (
        self->metadata_store, reader, self->object_store, self->materializer,
        error);
}

gboolean
wyrebox_daemon_delivery_materialization_catch_up (
    WyreboxDaemonDeliveryMaterialization *self, GError **error)
{
    gboolean success = FALSE;

    g_return_val_if_fail (WYREBOX_IS_DAEMON_DELIVERY_MATERIALIZATION (self),
        FALSE);
    g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

    g_mutex_lock (&self->lock);
    success = catch_up_locked (self, error);
    g_mutex_unlock (&self->lock);

    return success;
}

static gboolean
retry_catch_up (gpointer user_data)
{
    WyreboxDaemonDeliveryMaterialization *self = user_data;
    g_autoptr (GError) error = NULL;

    g_mutex_lock (&self->lock);
    if (!catch_up_locked (self, &error)) {
        g_mutex_unlock (&self->lock);
        g_warning ("delivery materialization retry failed: %s; retrying",
            error->message);
        return G_SOURCE_CONTINUE;
    }

    g_clear_pointer (&self->retry_source, g_source_unref);
    g_mutex_unlock (&self->lock);
    g_message ("delivery materialization retry succeeded");

    return G_SOURCE_REMOVE;
}

void
wyrebox_daemon_delivery_materialization_catch_up_or_schedule_retry (
    WyreboxDaemonDeliveryMaterialization *self)
{
    g_autoptr (GError) error = NULL;

    g_return_if_fail (WYREBOX_IS_DAEMON_DELIVERY_MATERIALIZATION (self));

    g_mutex_lock (&self->lock);
    if (catch_up_locked (self, &error)) {
        g_mutex_unlock (&self->lock);
        return;
    }

    if (self->stopped) {
        g_mutex_unlock (&self->lock);
        g_warning ("delivery materialization failed: %s; service stopped, "
            "no retry scheduled", error->message);
        return;
    }

    if (self->retry_source == NULL) {
        self->retry_source = g_timeout_source_new (self->retry_interval_ms);
        g_source_set_callback (self->retry_source, retry_catch_up,
            g_object_ref (self), g_object_unref);
        g_source_attach (self->retry_source, self->context);
    }
    g_mutex_unlock (&self->lock);

    g_warning ("delivery materialization failed: %s; retry scheduled",
        error->message);
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
wyrebox_daemon_delivery_materialization_set_retry_interval (
    WyreboxDaemonDeliveryMaterialization *self, guint interval_ms)
{
    g_return_if_fail (WYREBOX_IS_DAEMON_DELIVERY_MATERIALIZATION (self));

    g_mutex_lock (&self->lock);
    self->retry_interval_ms = interval_ms;
    g_mutex_unlock (&self->lock);
}
