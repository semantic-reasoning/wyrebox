#include "wyrebox-build-config.h"
#include "wyrebox-daemon-config.h"
#if defined(WYREBOX_HAVE_CAPNP_SERIALIZATION) && \
    WYREBOX_HAVE_CAPNP_SERIALIZATION
#include "wyrebox-daemon-capnp-codec.h"
#endif
#include "wyrebox-daemon-connection-server.h"
#include "wyrebox-daemon-delivery-ingestion-service.h"
#include "wyrebox-daemon-delivery-materialization.h"
#include "wyrebox-daemon-exit-code.h"
#include "wyrebox-daemon-mailbox-catalog-duckdb.h"
#include "wyrebox-daemon-request-adapter.h"
#include "wyrebox-daemon-runtime.h"
#include "wyrebox-eml-ingestor.h"
#include "wyrebox-journal-writer.h"
#include "wyrebox-local-object-store.h"

#include <gio/gio.h>
#include <glib.h>
#include <glib-unix.h>
#include <sysexits.h>

typedef struct
{
    char *config_path;
} WyreboxdOptions;

static void
wyreboxd_options_clear (WyreboxdOptions *options)
{
    if (options == NULL)
        return;

    g_clear_pointer (&options->config_path, g_free);
}

/* *INDENT-OFF* */
G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (WyreboxdOptions, wyreboxd_options_clear)
/* *INDENT-ON* */

static gchar **
copy_argv (int argc, char **argv)
{
    gchar **copy = g_new0 (gchar *, argc + 1);

    for (int i = 0; i < argc; i++)
        copy[i] = g_strdup (argv[i]);

    return copy;
}

static gboolean
parse_options (int argc, char **argv, WyreboxdOptions *options, GError **error)
{
    g_auto (GStrv) mutable_argv = NULL;
    g_autoptr (GOptionContext) context = NULL;
    GOptionEntry entries[] = {
        {"config", 'c', 0, G_OPTION_ARG_STRING, &options->config_path,
         "WyreBox daemon config file", "PATH"},
        {NULL}
    };

    mutable_argv = copy_argv (argc, argv);
    context = g_option_context_new (NULL);
    g_option_context_set_help_enabled (context, FALSE);
    g_option_context_add_main_entries (context, entries, NULL);

    if (!g_option_context_parse_strv (context, &mutable_argv, error))
        return FALSE;

    if (mutable_argv[1] != NULL) {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_INVALID_ARGUMENT,
            "unexpected positional argument: %s", mutable_argv[1]);
        return FALSE;
    }

    if (options->config_path == NULL)
        options->config_path = g_strdup (WYREBOX_DAEMON_DEFAULT_CONFIG_PATH);

    return TRUE;
}

static gboolean
decode_request_frame (const WyreboxDaemonPeerCredentials *peer_credentials,
    GBytes *request, WyreboxDaemonDecodedRequestFrame *out_request_frame,
    gpointer *out_decoded_state, GDestroyNotify *out_decoded_state_clear,
    gpointer user_data, GError **error)
{
#if defined(WYREBOX_HAVE_CAPNP_SERIALIZATION) && \
    WYREBOX_HAVE_CAPNP_SERIALIZATION
    return wyrebox_daemon_capnp_codec_decode_request_frame (peer_credentials,
               request, out_request_frame, out_decoded_state,
               out_decoded_state_clear, user_data, error);
#else
    (void)peer_credentials;
    (void)request;
    (void)out_request_frame;
    (void)out_decoded_state;
    (void)out_decoded_state_clear;
    (void)user_data;

    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_NOT_SUPPORTED,
        "wyreboxd request framing is not available without Cap'n Proto");
    return FALSE;
#endif
}

static GBytes *
encode_response_frame (const WyreboxDaemonResponseFrame *response_frame,
    gpointer user_data, GError **error)
{
#if defined(WYREBOX_HAVE_CAPNP_SERIALIZATION) && \
    WYREBOX_HAVE_CAPNP_SERIALIZATION
    return wyrebox_daemon_capnp_codec_encode_response_frame (response_frame,
               user_data, error);
#else
    (void)response_frame;
    (void)user_data;

    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_NOT_SUPPORTED,
        "wyreboxd response framing is not available without Cap'n Proto");
    return NULL;
#endif
}

static gboolean
quit_main_loop_on_signal (gpointer user_data)
{
    g_main_loop_quit (user_data);
    return G_SOURCE_REMOVE;
}

static void
materialize_after_ingest (const WyreboxEmlIngestResult *result,
    gpointer user_data)
{
    (void)result;

    wyrebox_daemon_delivery_materialization_catch_up_or_schedule_retry
        (user_data);
}

static int
run_daemon (int argc, char **argv)
{
    g_auto (WyreboxdOptions) options = { 0 };
    g_autoptr (GError) error = NULL;
    g_autoptr (WyreboxDaemonConfig) config = NULL;
    g_autoptr (WyreboxDaemonMailboxCatalogDuckDB) mailbox_catalog = NULL;
    g_autoptr (WyreboxDaemonMailboxListService) mailbox_list_service = NULL;
    g_autoptr (WyreboxDaemonMailboxSelectService) mailbox_select_service = NULL;
    g_autoptr (WyreboxLocalObjectStore) object_store = NULL;
    g_autoptr (WyreboxJournalWriter) journal_writer = NULL;
    g_autoptr (WyreboxDaemonDeliveryMaterialization) materialization = NULL;
    g_autoptr (WyreboxEmlIngestor) ingestor = NULL;
    g_autoptr (WyreboxDaemonDeliveryIngestionService) delivery_service = NULL;
    g_autoptr (WyreboxDaemonRequestAdapter) request_adapter = NULL;
    g_autoptr (WyreboxDaemonConnectionServer) server = NULL;
    g_autoptr (GMainLoop) loop = NULL;
    g_autofree char *socket_path = NULL;
    WyreboxDaemonDeliveryStorageValidationReport storage_report = { 0 };
    const char *catalog_path = NULL;
    const char *journal_root_dir = NULL;
    const char *object_root_dir = NULL;

    if (!parse_options (argc, argv, &options, &error)) {
        g_printerr ("wyreboxd: %s\n", error->message);
        return EX_USAGE;
    }

    config = wyrebox_daemon_config_new_from_file (options.config_path, &error);
    if (config == NULL) {
        g_printerr ("wyreboxd: %s\n", error->message);
        return EX_CONFIG;
    }

    if (!wyrebox_daemon_config_validate_for_startup (config, &error)) {
        g_printerr ("wyreboxd: %s\n", error->message);
        return EX_CONFIG;
    }

    socket_path = g_strdup (wyrebox_daemon_config_get_socket_path (config));
    catalog_path = wyrebox_daemon_config_get_catalog_path (config);
    journal_root_dir = wyrebox_daemon_config_get_journal_root_dir (config);
    object_root_dir = wyrebox_daemon_config_get_object_root_dir (config);

    object_store = wyrebox_local_object_store_new (object_root_dir, &error);
    if (object_store == NULL) {
        g_printerr ("wyreboxd: %s\n", error->message);
        return EX_OSERR;
    }

    if (!wyrebox_daemon_runtime_recover_and_validate_delivery_storage
            (journal_root_dir, object_root_dir, &storage_report, &error)) {
        g_printerr ("wyreboxd: delivery storage is invalid: %s\n",
            error->message);
        return EX_DATAERR;
    }

    journal_writer = wyrebox_journal_writer_new (journal_root_dir, &error);
    if (journal_writer == NULL) {
        g_printerr ("wyreboxd: %s\n", error->message);
        return EX_OSERR;
    }

    if (!wyrebox_daemon_runtime_prepare_catalog (journal_root_dir,
        catalog_path, FALSE, &error)) {
        g_printerr ("wyreboxd: catalog preparation failed: %s\n",
            error->message);
        return wyrebox_daemon_exit_code_for_startup_error (error);
    }

    mailbox_catalog = wyrebox_daemon_mailbox_catalog_duckdb_new (catalog_path,
            &error);
    if (mailbox_catalog == NULL) {
        g_printerr ("wyreboxd: %s\n", error->message);
        return EX_OSERR;
    }

    mailbox_list_service =
        wyrebox_daemon_mailbox_catalog_duckdb_new_list_service (catalog_path,
            &error);
    if (mailbox_list_service == NULL) {
        g_printerr ("wyreboxd: %s\n", error->message);
        return EX_OSERR;
    }

    mailbox_select_service =
        wyrebox_daemon_mailbox_catalog_duckdb_new_select_service (catalog_path,
            &error);
    if (mailbox_select_service == NULL) {
        g_printerr ("wyreboxd: %s\n", error->message);
        return EX_OSERR;
    }

    materialization = wyrebox_daemon_delivery_materialization_new
            (catalog_path, journal_root_dir, journal_writer, object_store,
            &error);
    if (materialization == NULL) {
        g_printerr ("wyreboxd: %s\n", error->message);
        return EX_OSERR;
    }

    if (!wyrebox_daemon_delivery_materialization_catch_up (materialization,
        &error)) {
        g_printerr ("wyreboxd: delivery materialization failed: %s\n",
            error->message);
        return wyrebox_daemon_exit_code_for_startup_error (error);
    }

    ingestor = wyrebox_eml_ingestor_new_with_journal (object_store,
            journal_writer);
    delivery_service =
        wyrebox_daemon_delivery_ingestion_service_new_with_ingestor (ingestor);
    wyrebox_daemon_delivery_ingestion_service_set_post_ingest_hook
        (delivery_service, materialize_after_ingest,
        g_object_ref (materialization), g_object_unref);
    request_adapter = wyrebox_daemon_request_adapter_new (delivery_service,
            NULL,
            mailbox_list_service, mailbox_select_service, NULL, NULL, NULL,
            NULL,
            decode_request_frame, NULL, NULL, encode_response_frame, NULL,
            NULL);
    server = wyrebox_daemon_connection_server_new (socket_path,
            request_adapter);

    /*
     * Clients treat the socket as readiness, so the handlers must be in place
     * before it appears; a signal caught before g_main_loop_run() stays pending
     * and quits the loop on its first iteration.
     */
    loop = g_main_loop_new (NULL, FALSE);
    (void)g_unix_signal_add (SIGTERM, quit_main_loop_on_signal, loop);
    (void)g_unix_signal_add (SIGINT, quit_main_loop_on_signal, loop);

    if (!wyrebox_daemon_connection_server_start (server, &error)) {
        g_printerr ("wyreboxd: %s\n", error->message);
        wyrebox_daemon_delivery_materialization_stop (materialization);
        return EX_OSERR;
    }

    g_main_loop_run (loop);
    wyrebox_daemon_delivery_materialization_stop (materialization);

    if (!wyrebox_daemon_connection_server_stop (server, &error)) {
        g_printerr ("wyreboxd: %s\n", error->message);
        return EX_OSERR;
    }

    return EX_OK;
}

int
main (int argc, char **argv)
{
    return run_daemon (argc, argv);
}
