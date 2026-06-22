#include "wyrebox-daemon-config.h"
#if defined(WYREBOX_HAVE_CAPNP_SERIALIZATION) && WYREBOX_HAVE_CAPNP_SERIALIZATION
#include "wyrebox-daemon-capnp-codec.h"
#endif
#include "wyrebox-daemon-connection-server.h"
#include "wyrebox-daemon-request-adapter.h"
#include "wyrebox-daemon-runtime.h"

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
#if defined(WYREBOX_HAVE_CAPNP_SERIALIZATION) && WYREBOX_HAVE_CAPNP_SERIALIZATION
  return wyrebox_daemon_capnp_codec_decode_request_frame (peer_credentials,
      request, out_request_frame, out_decoded_state,
      out_decoded_state_clear, user_data, error);
#else
  (void) peer_credentials;
  (void) request;
  (void) out_request_frame;
  (void) out_decoded_state;
  (void) out_decoded_state_clear;
  (void) user_data;

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
#if defined(WYREBOX_HAVE_CAPNP_SERIALIZATION) && WYREBOX_HAVE_CAPNP_SERIALIZATION
  return wyrebox_daemon_capnp_codec_encode_response_frame (response_frame,
      user_data, error);
#else
  (void) response_frame;
  (void) user_data;

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

static int
run_daemon (int argc, char **argv)
{
  g_auto (WyreboxdOptions) options = { 0 };
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxDaemonConfig) config = NULL;
  g_autoptr (WyreboxDaemonRequestAdapter) request_adapter = NULL;
  g_autoptr (WyreboxDaemonConnectionServer) server = NULL;
  g_autoptr (GMainLoop) loop = NULL;
  g_autofree char *socket_path = NULL;

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
  request_adapter = wyrebox_daemon_request_adapter_new (NULL, NULL, NULL, NULL,
      NULL, NULL, NULL, NULL, decode_request_frame, NULL, NULL,
      encode_response_frame, NULL, NULL);
  server = wyrebox_daemon_connection_server_new (socket_path, request_adapter);

  if (!wyrebox_daemon_connection_server_start (server, &error)) {
    g_printerr ("wyreboxd: %s\n", error->message);
    return EX_OSERR;
  }

  loop = g_main_loop_new (NULL, FALSE);
  (void) g_unix_signal_add (SIGTERM, quit_main_loop_on_signal, loop);
  (void) g_unix_signal_add (SIGINT, quit_main_loop_on_signal, loop);
  g_main_loop_run (loop);

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
