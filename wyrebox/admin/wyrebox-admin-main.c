#include "wyrebox-admin-health-status.h"
#include "wyrebox-admin-readiness-status.h"
#include "wyrebox-admin-health.h"
#include "wyrebox-admin-socket-status.h"

#include <gio/gio.h>
#include <glib/gstdio.h>
#include <sysexits.h>
#include <string.h>

typedef struct
{
  char *socket_path;
  gboolean json;
} WyreboxAdminSocketStatusOptions;

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

static int
print_usage (void)
{
  g_printerr ("Usage: wyrebox-admin <socket-status|health-status|"
      "readiness-status|health> "
      "[--socket-path PATH] [--json]\n");
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

  return print_usage ();
}

int
main (int argc, char **argv)
{
  return run_command (argc, argv);
}
