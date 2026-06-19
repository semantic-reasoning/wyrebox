#include "wyrebox-admin-health-status.h"
#include "wyrebox-admin-readiness-status.h"
#include "wyrebox-admin-health.h"
#include "wyrebox-admin-schema-version.h"
#include "wyrebox-admin-socket-status.h"
#include "wyrebox-journal-reader.h"
#include "wyrebox-schema-metadata-store.h"
#include "wyrebox-schema-metadata-store.h"

#include <gio/gio.h>
#include <glib/gstdio.h>
#include <sysexits.h>
#include <string.h>

typedef struct
{
  char *socket_path;
  gboolean json;
} WyreboxAdminSocketStatusOptions;

typedef struct
{
  char *journal_root;
  gboolean json;
} WyreboxAdminJournalPositionOptions;

typedef struct
{
  char *journal_root;
  char *catalog_path;
  gboolean json;
} WyreboxAdminMaterializationCheckpointOptions;

static void
wyrebox_admin_socket_status_options_clear (WyreboxAdminSocketStatusOptions
    *options)
{
  if (options == NULL)
    return;

  g_clear_pointer (&options->socket_path, g_free);
  options->json = FALSE;
}

/* *INDENT-OFF* */
G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (WyreboxAdminSocketStatusOptions,
    wyrebox_admin_socket_status_options_clear)

static void
wyrebox_admin_journal_position_options_clear
    (WyreboxAdminJournalPositionOptions *options)
{
  if (options == NULL)
    return;

  g_clear_pointer (&options->journal_root, g_free);
  options->json = FALSE;
}

/* *INDENT-OFF* */
G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (WyreboxAdminJournalPositionOptions,
    wyrebox_admin_journal_position_options_clear)
/* *INDENT-ON* */

static void
    wyrebox_admin_materialization_checkpoint_options_clear
    (WyreboxAdminMaterializationCheckpointOptions * options)
{
  if (options == NULL)
    return;

  g_clear_pointer (&options->journal_root, g_free);
  g_clear_pointer (&options->catalog_path, g_free);
  options->json = FALSE;
}

/* *INDENT-OFF* */
G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (WyreboxAdminMaterializationCheckpointOptions,
    wyrebox_admin_materialization_checkpoint_options_clear)
/* *INDENT-ON* */

/* *INDENT-ON* */

static gchar **
copy_argv (int argc, char **argv)
{
  gchar **copy = g_new0 (gchar *, argc + 1);

  for (int i = 0; i < argc; i++)
    copy[i] = g_strdup (argv[i]);

  return copy;
}

static void
append_json_escaped (GString *output, const char *value)
{
  const unsigned char *p = (const unsigned char *) value;

  g_string_append_c (output, '"');
  if (p != NULL) {
    for (; *p != '\0'; p++) {
      switch (*p) {
        case '\\':
          g_string_append (output, "\\\\");
          break;
        case '\"':
          g_string_append (output, "\\\"");
          break;
        case '\b':
          g_string_append (output, "\\b");
          break;
        case '\f':
          g_string_append (output, "\\f");
          break;
        case '\n':
          g_string_append (output, "\\n");
          break;
        case '\r':
          g_string_append (output, "\\r");
          break;
        case '\t':
          g_string_append (output, "\\t");
          break;
        default:
          if (g_ascii_isprint (*p)) {
            g_string_append_c (output, (char) *p);
          } else {
            g_string_append_printf (output, "\\u%04x", (guint) * p);
          }
          break;
      }
    }
  }
  g_string_append_c (output, '"');
}

static gboolean
parse_socket_status_options (int argc, char **argv,
    WyreboxAdminSocketStatusOptions *options, GError **error)
{
  g_auto (GStrv) mutable_argv = NULL;
  g_autoptr (GOptionContext) context = NULL;
  GOptionEntry entries[] = {
    {"socket-path", 0, 0, G_OPTION_ARG_STRING, &options->socket_path,
        "WyreBox daemon socket path", "PATH"},
    {"json", 0, 0, G_OPTION_ARG_NONE, &options->json,
        "Emit machine-readable JSON output", NULL},
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

  if (options->socket_path == NULL)
    options->socket_path =
        g_strdup (wyrebox_admin_socket_status_default_socket_path ());

  return TRUE;
}

static gboolean
parse_health_options (int argc, char **argv, WyreboxAdminHealthOptions *options,
    GError **error)
{
  for (int index = 0; index < argc; index++) {
    if (g_strcmp0 (argv[index], "--json") == 0) {
      options->json = TRUE;
      continue;
    }

    if (g_strcmp0 (argv[index], "--socket-path") == 0) {
      if (index + 1 >= argc) {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_INVALID_ARGUMENT, "--socket-path requires a value");
        return FALSE;
      }

      g_free (options->socket_path);
      options->socket_path = g_strdup (argv[++index]);
      continue;
    }

    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "unexpected positional argument: %s", argv[index]);
    return FALSE;
  }

  if (options->socket_path == NULL)
    options->socket_path =
        g_strdup (wyrebox_admin_health_default_socket_path ());

  return TRUE;
}

static gboolean
parse_journal_position_options (int argc, char **argv,
    WyreboxAdminJournalPositionOptions *options, GError **error)
{
  for (int index = 0; index < argc; index++) {
    if (g_strcmp0 (argv[index], "--json") == 0) {
      options->json = TRUE;
      continue;
    }

    if (g_strcmp0 (argv[index], "--journal-root") == 0) {
      if (index + 1 >= argc) {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_INVALID_ARGUMENT, "--journal-root requires a value");
        return FALSE;
      }

      g_free (options->journal_root);
      options->journal_root = g_strdup (argv[++index]);
      continue;
    }

    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "unexpected positional argument: %s", argv[index]);
    return FALSE;
  }

  return TRUE;
}

static gboolean
parse_materialization_checkpoint_options (int argc, char **argv,
    WyreboxAdminMaterializationCheckpointOptions *options, GError **error)
{
  for (int index = 0; index < argc; index++) {
    if (g_strcmp0 (argv[index], "--json") == 0) {
      options->json = TRUE;
      continue;
    }

    if (g_strcmp0 (argv[index], "--journal-root") == 0) {
      if (index + 1 >= argc) {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_INVALID_ARGUMENT, "--journal-root requires a value");
        return FALSE;
      }

      g_free (options->journal_root);
      options->journal_root = g_strdup (argv[++index]);
      continue;
    }

    if (g_strcmp0 (argv[index], "--catalog-path") == 0) {
      if (index + 1 >= argc) {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_INVALID_ARGUMENT, "--catalog-path requires a value");
        return FALSE;
      }

      g_free (options->catalog_path);
      options->catalog_path = g_strdup (argv[++index]);
      continue;
    }

    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "unexpected positional argument: %s", argv[index]);
    return FALSE;
  }

  return TRUE;
}

static int
print_usage (void)
{
  g_printerr ("Usage: wyrebox-admin <socket-status|health-status|"
      "readiness-status|health|schema-version|journal-position|"
      "materialization-checkpoint> [--socket-path PATH] [--journal-root PATH] "
      "[--catalog-path PATH] [--json]\n");
  g_printerr ("Usage: wyrebox-admin <socket-status|health-status|"
      "readiness-status|health|schema-version|journal-position|"
      "materialization-checkpoint|materialization-manifest> "
      "[--socket-path PATH] [--journal-root PATH] [--catalog-path PATH] "
      "[--json]\n");
  return WYREBOX_ADMIN_SOCKET_STATUS_EXIT_USAGE_ERROR;
}

static int
run_socket_status (int argc, char **argv)
{
  g_auto (WyreboxAdminSocketStatusOptions) options = { 0 };
  g_auto (WyreboxAdminSocketStatusResult) result = { 0 };
  g_autoptr (GError) error = NULL;
  g_autoptr (GString) json = NULL;
  int exit_status = WYREBOX_ADMIN_SOCKET_STATUS_EXIT_USAGE_ERROR;

  if (!parse_socket_status_options (argc, argv, &options, &error))
    return print_usage ();

  exit_status = wyrebox_admin_socket_status_probe (options.socket_path,
      &result);

  if (options.json) {
    json = g_string_new (NULL);
    g_string_append (json, "{");
    g_string_append (json, "\"socket_path\":");
    append_json_escaped (json, result.socket_path);
    g_string_append (json, ",\"status\":");
    append_json_escaped (json, result.status_name);
    g_string_append_printf (json, ",\"connectable\":%s}",
        result.connectable ? "true" : "false");
    g_print ("%s\n", json->str);
  } else {
    g_print ("socket-status path=%s status: %s connectable=%s\n",
        result.socket_path, result.status_name,
        result.connectable ? "true" : "false");
  }

  return exit_status;
}

static int
run_health_status (int argc, char **argv)
{
  (void) argc;
  (void) argv;

  g_print ("health-status state=%s\n",
      wyrebox_admin_health_status_state_to_name
      (WYREBOX_ADMIN_HEALTH_STATUS_UNAVAILABLE));

  return WYREBOX_ADMIN_SOCKET_STATUS_EXIT_SUCCESS;
}

static int
run_readiness_status (int argc, char **argv)
{
  (void) argc;
  (void) argv;

  g_print ("readiness-status state=%s\n",
      wyrebox_admin_readiness_status_state_to_name
      (WYREBOX_ADMIN_READINESS_STATUS_UNAVAILABLE));

  return WYREBOX_ADMIN_SOCKET_STATUS_EXIT_SUCCESS;
}

static int
run_schema_version (int argc, char **argv)
{
  g_auto (WyreboxAdminSchemaVersionOptions) options = { 0 };
  g_auto (WyreboxAdminSchemaVersionResult) result = { 0 };
  g_autoptr (GError) error = NULL;
  g_autoptr (GString) json = NULL;
  int exit_status = WYREBOX_ADMIN_SOCKET_STATUS_EXIT_USAGE_ERROR;

  for (int index = 1; index < argc; index++) {
    if (g_strcmp0 (argv[index], "--json") == 0) {
      options.json = TRUE;
      continue;
    }

    g_set_error (&error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "unexpected positional argument: %s", argv[index]);
    return print_usage ();
  }

  exit_status = wyrebox_admin_schema_version_probe (&result);

  if (options.json) {
    json = g_string_new (NULL);
    g_string_append (json, "{");
    g_string_append_printf (json, "\"schema_version\":%" G_GUINT64_FORMAT,
        result.schema_version);
    g_string_append_c (json, '}');
    g_print ("%s\n", json->str);
  } else {
    g_print ("schema-version version=%" G_GUINT64_FORMAT "\n",
        result.schema_version);
  }

  return exit_status;
}

static int
run_health (int argc, char **argv)
{
  g_auto (WyreboxAdminHealthOptions) options = { 0 };
  g_auto (WyreboxAdminHealthResult) result = { 0 };
  g_autoptr (GError) error = NULL;
  g_autoptr (GString) json = NULL;
  int exit_status = WYREBOX_ADMIN_SOCKET_STATUS_EXIT_USAGE_ERROR;

  if (!parse_health_options (argc, argv, &options, &error))
    return print_usage ();

  exit_status = wyrebox_admin_health_probe (options.socket_path, &result);

  if (options.json) {
    json = g_string_new (NULL);
    g_string_append (json, "{");
    g_string_append (json, "\"socket_path\":");
    append_json_escaped (json, result.socket_path);
    g_string_append (json, ",\"status\":");
    append_json_escaped (json, result.status_name);
    g_string_append_printf (json, ",\"healthy\":%s}",
        result.healthy ? "true" : "false");
    g_print ("%s\n", json->str);
  } else {
    g_print ("health path=%s status: %s healthy=%s\n",
        result.socket_path, result.status_name,
        result.healthy ? "true" : "false");
  }

  return exit_status;
}

static int
run_journal_position (int argc, char **argv)
{
  g_auto (WyreboxAdminJournalPositionOptions) options = { 0 };
  g_autoptr (WyreboxJournalReader) reader = NULL;
  g_auto (WyreboxJournalRecord) record = { 0 };
  g_autoptr (GError) error = NULL;
  g_autoptr (GString) json = NULL;
  gboolean eof = FALSE;
  guint64 last_offset = 0;
  guint64 last_sequence = 0;
  int exit_status = WYREBOX_ADMIN_SOCKET_STATUS_EXIT_USAGE_ERROR;

  if (!parse_journal_position_options (argc, argv, &options, &error))
    return print_usage ();

  if (options.journal_root == NULL) {
    g_set_error (&error,
        G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "--journal-root is required");
    return print_usage ();
  }

  reader = wyrebox_journal_reader_new (options.journal_root, &error);
  if (reader == NULL)
    return WYREBOX_ADMIN_SOCKET_STATUS_EXIT_CONNECT_FAILED;

  while (wyrebox_journal_reader_read_next (reader, &record, &eof, &error)) {
    last_offset = record.offset;
    last_sequence = record.sequence;
    wyrebox_journal_record_clear (&record);
    if (eof)
      break;
  }

  if (error != NULL)
    return WYREBOX_ADMIN_SOCKET_STATUS_EXIT_CONNECT_FAILED;

  exit_status = 0;

  if (options.json) {
    json = g_string_new (NULL);
    g_string_append_printf (json, "{\"journal_root\":");
    append_json_escaped (json, options.journal_root);
    g_string_append_printf (json,
        ",\"last_offset\":%" G_GUINT64_FORMAT
        ",\"last_sequence\":%" G_GUINT64_FORMAT "}",
        last_offset, last_sequence);
    g_print ("%s\n", json->str);
  } else {
    g_print ("journal-position root=%s last_offset=%" G_GUINT64_FORMAT
        " last_sequence=%" G_GUINT64_FORMAT "\n",
        options.journal_root, last_offset, last_sequence);
  }

  return exit_status;
}

static int
run_materialization_checkpoint (int argc, char **argv)
{
  g_auto (WyreboxAdminMaterializationCheckpointOptions) options = { 0 };
  g_autoptr (WyreboxSchemaMetadataStore) store = NULL;
  g_autoptr (WyreboxJournalReader) reader = NULL;
  g_auto (WyreboxJournalRecord) record = { 0 };
  g_auto (WyreboxMaterializationManifest) manifest = { 0 };
  g_autoptr (GError) error = NULL;
  g_autoptr (GString) json = NULL;
  gboolean eof = FALSE;
  gboolean stale = FALSE;
  guint64 journal_offset_lag = 0;
  guint64 journal_sequence_lag = 0;
  guint64 journal_last_offset = 0;
  guint64 journal_last_sequence = 0;
  int exit_status = WYREBOX_ADMIN_SOCKET_STATUS_EXIT_USAGE_ERROR;

  if (!parse_materialization_checkpoint_options (argc, argv, &options, &error))
    return print_usage ();

  if (options.journal_root == NULL || options.catalog_path == NULL) {
    g_set_error (&error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "--journal-root and --catalog-path are required");
    return print_usage ();
  }

  store = wyrebox_schema_metadata_store_new_duckdb (options.catalog_path,
      &error);
  if (store == NULL)
    return WYREBOX_ADMIN_SOCKET_STATUS_EXIT_CONNECT_FAILED;

  if (!wyrebox_schema_metadata_store_load_latest_materialization_manifest
      (store, &manifest, &error))
    return WYREBOX_ADMIN_SOCKET_STATUS_EXIT_CONNECT_FAILED;

  reader = wyrebox_journal_reader_new (options.journal_root, &error);
  if (reader == NULL)
    return WYREBOX_ADMIN_SOCKET_STATUS_EXIT_CONNECT_FAILED;

  while (wyrebox_journal_reader_read_next (reader, &record, &eof, &error)) {
    journal_last_offset = record.offset;
    journal_last_sequence = record.sequence;
    wyrebox_journal_record_clear (&record);
    if (eof)
      break;
  }

  if (error != NULL)
    return WYREBOX_ADMIN_SOCKET_STATUS_EXIT_CONNECT_FAILED;

  stale = manifest.end_journal_offset < journal_last_offset ||
      manifest.end_journal_sequence < journal_last_sequence;
  if (journal_last_offset >= manifest.end_journal_offset)
    journal_offset_lag = journal_last_offset - manifest.end_journal_offset;
  if (journal_last_sequence >= manifest.end_journal_sequence)
    journal_sequence_lag =
        journal_last_sequence - manifest.end_journal_sequence;

  exit_status = 0;

  if (options.json) {
    json = g_string_new (NULL);
    g_string_append (json, "{");
    g_string_append (json, "\"journal_root\":");
    append_json_escaped (json, options.journal_root);
    g_string_append (json, ",\"catalog_path\":");
    append_json_escaped (json, options.catalog_path);
    g_string_append (json, ",\"manifest_run_id\":");
    append_json_escaped (json, manifest.run_id);
    g_string_append_printf (json,
        ",\"manifest_end_offset\":%" G_GUINT64_FORMAT
        ",\"manifest_end_sequence\":%" G_GUINT64_FORMAT
        ",\"journal_last_offset\":%" G_GUINT64_FORMAT
        ",\"journal_last_sequence\":%" G_GUINT64_FORMAT
        ",\"journal_offset_lag\":%" G_GUINT64_FORMAT
        ",\"journal_sequence_lag\":%" G_GUINT64_FORMAT
        ",\"stale\":%s}",
        manifest.end_journal_offset,
        manifest.end_journal_sequence,
        journal_last_offset, journal_last_sequence, journal_offset_lag,
        journal_sequence_lag, stale ? "true" : "false");
    g_print ("%s\n", json->str);
  } else {
    g_print ("materialization-checkpoint journal_root=%s catalog_path=%s "
        "manifest_run_id=%s manifest_end_offset=%" G_GUINT64_FORMAT
        " manifest_end_sequence=%" G_GUINT64_FORMAT
        " journal_last_offset=%" G_GUINT64_FORMAT
        " journal_last_sequence=%" G_GUINT64_FORMAT
        " journal_offset_lag=%" G_GUINT64_FORMAT
        " journal_sequence_lag=%" G_GUINT64_FORMAT
        " stale=%s\n",
        options.journal_root, options.catalog_path, manifest.run_id,
        manifest.end_journal_offset, manifest.end_journal_sequence,
        journal_last_offset, journal_last_sequence, journal_offset_lag,
        journal_sequence_lag, stale ? "true" : "false");
  }

  return exit_status;
}

static int
run_materialization_manifest (int argc, char **argv)
{
  g_auto (WyreboxAdminMaterializationCheckpointOptions) options = { 0 };
  g_autoptr (WyreboxSchemaMetadataStore) store = NULL;
  g_auto (WyreboxMaterializationManifest) manifest = { 0 };
  g_autoptr (GError) error = NULL;
  g_autoptr (GString) json = NULL;
  int exit_status = WYREBOX_ADMIN_SOCKET_STATUS_EXIT_USAGE_ERROR;

  if (!parse_materialization_checkpoint_options (argc, argv, &options, &error))
    return print_usage ();

  if (options.catalog_path == NULL) {
    g_set_error (&error,
        G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "--catalog-path is required");
    return print_usage ();
  }

  store = wyrebox_schema_metadata_store_new_duckdb (options.catalog_path,
      &error);
  if (store == NULL)
    return WYREBOX_ADMIN_SOCKET_STATUS_EXIT_CONNECT_FAILED;

  if (!wyrebox_schema_metadata_store_load_latest_materialization_manifest
      (store, &manifest, &error))
    return WYREBOX_ADMIN_SOCKET_STATUS_EXIT_CONNECT_FAILED;

  exit_status = 0;

  if (options.json) {
    json = g_string_new (NULL);
    g_string_append (json, "{");
    g_string_append (json, "\"catalog_path\":");
    append_json_escaped (json, options.catalog_path);
    g_string_append (json, ",\"run_id\":");
    append_json_escaped (json, manifest.run_id);
    g_string_append_printf (json,
        ",\"start_journal_offset\":%" G_GUINT64_FORMAT
        ",\"start_journal_sequence\":%" G_GUINT64_FORMAT
        ",\"end_journal_offset\":%" G_GUINT64_FORMAT
        ",\"end_journal_sequence\":%" G_GUINT64_FORMAT
        ",\"materialized_schema_version\":%" G_GUINT64_FORMAT
        ",\"object_store_identity\":",
        manifest.start_journal_offset,
        manifest.start_journal_sequence,
        manifest.end_journal_offset,
        manifest.end_journal_sequence, manifest.materialized_schema_version);
    append_json_escaped (json, manifest.object_store_identity);
    g_string_append (json, ",\"rule_package_version\":");
    append_json_escaped (json, manifest.rule_package_version);
    g_string_append (json, ",\"view_package_version\":");
    append_json_escaped (json, manifest.view_package_version);
    g_string_append (json, ",\"engine_version\":");
    append_json_escaped (json, manifest.engine_version);
    g_string_append_printf (json,
        ",\"created_at_unix_us\":%" G_GUINT64_FORMAT
        ",\"completion_status\":", manifest.created_at_unix_us);
    append_json_escaped (json, manifest.completion_status);
    g_string_append (json, ",\"error_state\":");
    append_json_escaped (json, manifest.error_state);
    g_string_append_c (json, '}');
    g_print ("%s\n", json->str);
  } else {
    g_print ("materialization-manifest catalog_path=%s run_id=%s "
        "start_journal_offset=%" G_GUINT64_FORMAT
        " start_journal_sequence=%" G_GUINT64_FORMAT
        " end_journal_offset=%" G_GUINT64_FORMAT
        " end_journal_sequence=%" G_GUINT64_FORMAT
        " materialized_schema_version=%" G_GUINT64_FORMAT
        " object_store_identity=%s rule_package_version=%s "
        "view_package_version=%s engine_version=%s created_at_unix_us=%"
        G_GUINT64_FORMAT " completion_status=%s error_state=%s\n",
        options.catalog_path, manifest.run_id,
        manifest.start_journal_offset, manifest.start_journal_sequence,
        manifest.end_journal_offset, manifest.end_journal_sequence,
        manifest.materialized_schema_version,
        manifest.object_store_identity, manifest.rule_package_version,
        manifest.view_package_version, manifest.engine_version,
        manifest.created_at_unix_us, manifest.completion_status,
        manifest.error_state != NULL ? manifest.error_state : "");
  }

  return exit_status;
}

static int
run_command (int argc, char **argv)
{
  if (argc < 2)
    return print_usage ();

  if (g_strcmp0 (argv[1], "socket-status") == 0)
    return run_socket_status (argc - 1, argv + 1);

  if (g_strcmp0 (argv[1], "health-status") == 0)
    return run_health_status (argc - 1, argv + 1);

  if (g_strcmp0 (argv[1], "readiness-status") == 0)
    return run_readiness_status (argc - 1, argv + 1);

  if (g_strcmp0 (argv[1], "health") == 0)
    return run_health (argc - 2, argv + 2);

  if (g_strcmp0 (argv[1], "schema-version") == 0)
    return run_schema_version (argc - 1, argv + 1);

  if (g_strcmp0 (argv[1], "journal-position") == 0)
    return run_journal_position (argc - 2, argv + 2);

  if (g_strcmp0 (argv[1], "materialization-checkpoint") == 0)
    return run_materialization_checkpoint (argc - 2, argv + 2);

  if (g_strcmp0 (argv[1], "materialization-manifest") == 0)
    return run_materialization_manifest (argc - 2, argv + 2);

  return print_usage ();
}

int
main (int argc, char **argv)
{
  return run_command (argc, argv);
}
