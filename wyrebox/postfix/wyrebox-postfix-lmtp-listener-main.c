#include "wyrebox-daemon-delivery-ingestion-request.h"
#include "wyrebox-daemon-request-identity.h"
#include "wyrebox-postfix-lmtp-delivery-bridge.h"

#include <gio/gio.h>
#include <gio/gunixsocketaddress.h>
#include <glib/gstdio.h>

#include <errno.h>
#include <string.h>

#define WYREBOX_POSTFIX_LMTP_LISTENER_DEFAULT_DAEMON_SOCKET \
  "/run/wyrebox/wyrebox.sock"
#define WYREBOX_POSTFIX_LMTP_LISTENER_DEFAULT_LISTEN_SOCKET \
  "/run/wyrebox/wyrebox-lmtp.sock"
#define WYREBOX_POSTFIX_LMTP_LISTENER_CALLER_IDENTITY "postfix"
#define WYREBOX_POSTFIX_LMTP_LISTENER_TOOL_IDENTITY \
  "wyrebox-postfix-lmtp-listener"
#define WYREBOX_POSTFIX_LMTP_LISTENER_MAX_LINE_BYTES 8192
#define WYREBOX_POSTFIX_LMTP_LISTENER_DEFAULT_MAX_MESSAGE_BYTES \
  ((guint64) 64 * 1024 * 1024)

typedef struct
{
  char *account_id;
  char *delivery_id;
  char *listen_socket_path;
  char *daemon_socket_path;
  gint64 max_message_bytes;
} ListenerOptions;

typedef struct
{
  gboolean seen_lhlo;
  gboolean seen_mail;
  char *sender;
  GPtrArray *recipients;
  GByteArray *message;
} ListenerTransaction;

static void
listener_options_clear (ListenerOptions *options)
{
  if (options == NULL)
    return;

  g_clear_pointer (&options->account_id, g_free);
  g_clear_pointer (&options->delivery_id, g_free);
  g_clear_pointer (&options->listen_socket_path, g_free);
  g_clear_pointer (&options->daemon_socket_path, g_free);
}

/* *INDENT-OFF* */
G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (ListenerOptions, listener_options_clear)
/* *INDENT-ON* */

static void
listener_transaction_clear (ListenerTransaction *transaction)
{
  if (transaction == NULL)
    return;

  g_clear_pointer (&transaction->sender, g_free);
  g_clear_pointer (&transaction->recipients, g_ptr_array_unref);
  g_clear_pointer (&transaction->message, g_byte_array_unref);
}

/* *INDENT-OFF* */
G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (ListenerTransaction,
    listener_transaction_clear)
/* *INDENT-ON* */

static gboolean
require_non_empty_argument (const char *value, const char *option_name,
    GError **error)
{
  if (value != NULL && *value != '\0')
    return TRUE;

  g_set_error (error,
      G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "%s is required", option_name);
  return FALSE;
}

static gchar **
copy_argv (int argc, char **argv)
{
  gchar **copy = g_new0 (gchar *, argc + 1);

  for (int i = 0; i < argc; i++)
    copy[i] = g_strdup (argv[i]);

  return copy;
}

static gboolean
parse_options (int argc, char **argv, ListenerOptions *options, GError **error)
{
  g_auto (GStrv) mutable_argv = NULL;
  g_autoptr (GOptionContext) context = NULL;
  GOptionEntry entries[] = {
    {"account-id", 0, 0, G_OPTION_ARG_STRING, &options->account_id,
        "WyreBox account identity", "ACCOUNT"},
    {"delivery-id", 0, 0, G_OPTION_ARG_STRING, &options->delivery_id,
        "Stable Postfix delivery identity", "DELIVERY_ID"},
    {"listen-socket", 0, 0, G_OPTION_ARG_STRING, &options->listen_socket_path,
        "LMTP Unix-domain listener socket path", "PATH"},
    {"daemon-socket", 0, 0, G_OPTION_ARG_STRING, &options->daemon_socket_path,
        "WyreBox daemon API Unix-domain socket path", "PATH"},
    {"max-message-bytes", 0, 0, G_OPTION_ARG_INT64,
          &options->max_message_bytes,
        "Maximum accepted LMTP DATA bytes", "BYTES"},
    {NULL}
  };

  mutable_argv = copy_argv (argc, argv);
  context = g_option_context_new (NULL);
  g_option_context_set_help_enabled (context, FALSE);
  g_option_context_add_main_entries (context, entries, NULL);

  if (!g_option_context_parse_strv (context, &mutable_argv, error))
    return FALSE;

  if (!require_non_empty_argument (options->account_id, "--account-id", error))
    return FALSE;
  if (!require_non_empty_argument (options->delivery_id, "--delivery-id",
          error))
    return FALSE;

  if (mutable_argv[1] != NULL) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "unexpected positional argument: %s", mutable_argv[1]);
    return FALSE;
  }

  if (options->listen_socket_path == NULL)
    options->listen_socket_path =
        g_strdup (WYREBOX_POSTFIX_LMTP_LISTENER_DEFAULT_LISTEN_SOCKET);
  if (options->daemon_socket_path == NULL)
    options->daemon_socket_path =
        g_strdup (WYREBOX_POSTFIX_LMTP_LISTENER_DEFAULT_DAEMON_SOCKET);
  if (options->max_message_bytes == 0)
    options->max_message_bytes =
        WYREBOX_POSTFIX_LMTP_LISTENER_DEFAULT_MAX_MESSAGE_BYTES;
  if (options->max_message_bytes < 0) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT, "--max-message-bytes must be positive");
    return FALSE;
  }

  return TRUE;
}

static gboolean
write_line (GOutputStream *output, const char *line, GError **error)
{
  gsize bytes_written = 0;
  g_autofree char *wire_line = NULL;

  wire_line = g_strdup_printf ("%s\r\n", line);
  return g_output_stream_write_all (output,
      wire_line, strlen (wire_line), &bytes_written, NULL, error);
}

static gboolean
read_crlf_line (GInputStream *input, GByteArray *line, GError **error)
{
  gboolean pending_cr = FALSE;

  g_byte_array_set_size (line, 0);

  for (;;) {
    guint8 byte = 0;
    gssize bytes_read =
        g_input_stream_read (input, &byte, sizeof byte, NULL, error);

    if (bytes_read < 0)
      return FALSE;
    if (bytes_read == 0) {
      g_set_error (error,
          G_IO_ERROR,
          G_IO_ERROR_CONNECTION_CLOSED, "LMTP client closed connection");
      return FALSE;
    }

    if (pending_cr) {
      if (byte == '\n')
        return TRUE;

      if (line->len >= WYREBOX_POSTFIX_LMTP_LISTENER_MAX_LINE_BYTES) {
        g_set_error (error,
            G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "LMTP line is too long");
        return FALSE;
      }
      g_byte_array_append (line, (const guint8 *) "\r", 1);
      pending_cr = FALSE;
    }

    if (byte == '\r') {
      pending_cr = TRUE;
      continue;
    }

    if (line->len >= WYREBOX_POSTFIX_LMTP_LISTENER_MAX_LINE_BYTES) {
      g_set_error (error,
          G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "LMTP line is too long");
      return FALSE;
    }

    g_byte_array_append (line, &byte, 1);
  }
}

static gboolean
line_has_prefix_ci (const char *line, const char *prefix)
{
  return g_ascii_strncasecmp (line, prefix, strlen (prefix)) == 0;
}

static char *
extract_path_argument (const char *line, const char *command)
{
  const char *start = NULL;
  const char *end = NULL;

  if (!line_has_prefix_ci (line, command))
    return NULL;

  start = strchr (line, '<');
  end = strrchr (line, '>');
  if (start == NULL || end == NULL || end < start)
    return NULL;

  return g_strndup (start + 1, (gsize) (end - start - 1));
}

static gboolean
is_valid_delivery_recipient (const char *recipient)
{
  if (recipient == NULL || *recipient == '\0')
    return FALSE;

  for (const char *cursor = recipient; *cursor != '\0'; cursor++) {
    if (g_ascii_iscntrl (*cursor))
      return FALSE;
  }

  return TRUE;
}

static gboolean
append_data_line (ListenerTransaction *transaction,
    const guint8 *line, gsize line_len, guint64 max_message_bytes,
    GError **error)
{
  const guint8 *payload = line;
  gsize payload_len = line_len;
  guint64 current_len = 0;
  guint64 next_len = 0;

  if (transaction->message == NULL)
    transaction->message = g_byte_array_new ();

  if (line_len > 0 && line[0] == '.') {
    payload = line + 1;
    payload_len = line_len - 1;
  }

  current_len = transaction->message->len;
  next_len = current_len + payload_len + 2;
  if (payload_len > G_MAXUINT || next_len > G_MAXUINT ||
      next_len > max_message_bytes) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_NO_SPACE,
        "LMTP DATA exceeds maximum size of %" G_GUINT64_FORMAT " bytes",
        max_message_bytes);
    return FALSE;
  }

  g_byte_array_append (transaction->message, payload, (guint) payload_len);
  g_byte_array_append (transaction->message, (const guint8 *) "\r\n", 2);
  return TRUE;
}

static char *
build_recipient_delivery_id (const ListenerOptions *options,
    guint recipient_index)
{
  return g_strdup_printf ("%s/rcpt/%u", options->delivery_id,
      recipient_index + 1);
}

static char *
build_request_id (const char *delivery_id)
{
  return g_strdup_printf ("postfix-lmtp:%s", delivery_id);
}

static gboolean
validate_delivery_transaction (const ListenerTransaction *transaction,
    GError **error)
{
  guint recipient_count = 0;

  if (transaction->recipients != NULL)
    recipient_count = transaction->recipients->len;

  if (transaction->sender == NULL || recipient_count == 0 ||
      transaction->message == NULL || transaction->message->len == 0) {
    g_set_error (error,
        G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
        "LMTP transaction is incomplete");
    return FALSE;
  }

  return TRUE;
}

static gboolean
build_delivery_request (const ListenerOptions *options,
    ListenerTransaction *transaction, guint recipient_index,
    GBytes *message_bytes,
    WyreboxPostfixLmtpOptions *bridge_options,
    WyreboxDaemonRequestIdentity *identity,
    WyreboxDaemonDeliveryIngestionRequest *request, GError **error)
{
  g_autofree char *delivery_id = NULL;
  g_autofree char *request_id = NULL;

  delivery_id = build_recipient_delivery_id (options, recipient_index);
  request_id = build_request_id (delivery_id);
  if (!wyrebox_daemon_request_identity_init (identity,
          request_id,
          WYREBOX_POSTFIX_LMTP_LISTENER_CALLER_IDENTITY,
          options->account_id,
          WYREBOX_POSTFIX_LMTP_LISTENER_TOOL_IDENTITY, request_id, error))
    return FALSE;

  {
    const gchar *recipients[] = {
      (const gchar *) g_ptr_array_index (transaction->recipients,
          recipient_index),
      NULL
    };

    if (!wyrebox_daemon_delivery_ingestion_request_init (request,
            delivery_id,
            NULL, transaction->sender, recipients, message_bytes, error))
      return FALSE;
  }

  bridge_options->socket_path = g_strdup (options->daemon_socket_path);
  return TRUE;
}

static guint
transaction_recipient_count (const ListenerTransaction *transaction)
{
  if (transaction->recipients == NULL)
    return 0;

  return transaction->recipients->len;
}

static gboolean
write_line_for_each_recipient (GOutputStream *output,
    const ListenerTransaction *transaction, const char *line, GError **error)
{
  guint recipient_count = transaction_recipient_count (transaction);

  for (guint i = 0; i < recipient_count; i++) {
    if (!write_line (output, line, error))
      return FALSE;
  }

  return TRUE;
}

static gboolean
write_local_temporary_failure (GOutputStream *output, const GError *cause,
    const ListenerTransaction *transaction, GError **error)
{
  g_printerr ("postfix_lmtp_listener outcome=temporary_failure "
      "failure_source=local local_error_domain=%s local_error_code=%d\n",
      cause != NULL ? g_quark_to_string (cause->domain) : "none",
      cause != NULL ? cause->code : 0);

  if (transaction_recipient_count (transaction) > 0) {
    return write_line_for_each_recipient (output,
        transaction, "451 4.3.0 Temporary local delivery failure", error);
  }

  return write_line (output,
      "451 4.3.0 Temporary local delivery failure", error);
}

static gboolean
deliver_transaction (const ListenerOptions *options,
    ListenerTransaction *transaction, GOutputStream *output, GError **error)
{
  g_autoptr (GError) local_error = NULL;
  g_autoptr (GBytes) message_bytes = NULL;
  guint recipient_count = transaction_recipient_count (transaction);

  if (!validate_delivery_transaction (transaction, &local_error))
    return write_local_temporary_failure (output, local_error, transaction,
        error);

  message_bytes =
      g_byte_array_free_to_bytes (g_steal_pointer (&transaction->message));

  for (guint i = 0; i < recipient_count; i++) {
    g_auto (WyreboxPostfixLmtpOptions) bridge_options = { 0 };
    g_auto (WyreboxDaemonRequestIdentity) identity = { 0 };
    g_auto (WyreboxDaemonDeliveryIngestionRequest) request = { 0 };
    g_auto (WyreboxPostfixLmtpReply) reply = { 0 };
    g_autofree char *reply_line = NULL;

    g_clear_error (&local_error);
    if (!build_delivery_request (options, transaction, i, message_bytes,
            &bridge_options, &identity, &request, &local_error))
      return write_local_temporary_failure (output, local_error, transaction,
          error);

    g_clear_error (&local_error);
    if (!wyrebox_postfix_lmtp_delivery_bridge_deliver (&bridge_options,
            &identity, &request, &reply, &local_error))
      return write_local_temporary_failure (output, local_error, transaction,
          error);

    reply_line = g_strdup_printf ("%d %s %s",
        reply.reply_code, reply.enhanced_status, reply.reply_text);
    g_printerr ("%s\n", reply.log_message);
    if (!write_line (output, reply_line, error))
      return FALSE;
  }

  return TRUE;
}

static gboolean
finish_after_transaction (GOutputStream *output, GError **error)
{
  return write_line (output, "221 2.0.0 Bye", error);
}

static void
reset_transaction (ListenerTransaction *transaction)
{
  g_clear_pointer (&transaction->sender, g_free);
  if (transaction->recipients != NULL)
    g_ptr_array_set_size (transaction->recipients, 0);
  g_clear_pointer (&transaction->message, g_byte_array_unref);
  transaction->seen_mail = FALSE;
}

static gboolean
handle_connection (const ListenerOptions *options,
    GSocketConnection *connection, GError **error)
{
  g_auto (ListenerTransaction) transaction = { 0 };
  g_autoptr (GByteArray) raw_line = NULL;
  GInputStream *input = NULL;
  GOutputStream *output = NULL;

  input = g_io_stream_get_input_stream (G_IO_STREAM (connection));
  output = g_io_stream_get_output_stream (G_IO_STREAM (connection));
  raw_line = g_byte_array_new ();

  if (!write_line (output, "220 wyrebox LMTP ready", error))
    return FALSE;

  for (;;) {
    g_autofree char *line = NULL;

    if (!read_crlf_line (input, raw_line, error))
      return FALSE;

    line = g_strndup ((const char *) raw_line->data, raw_line->len);

    if (line_has_prefix_ci (line, "LHLO ")) {
      transaction.seen_lhlo = TRUE;
      if (!write_line (output, "250-wyrebox", error) ||
          !write_line (output, "250 PIPELINING", error))
        return FALSE;
      continue;
    }

    if (line_has_prefix_ci (line, "MAIL FROM:")) {
      g_autofree char *sender = NULL;

      if (!transaction.seen_lhlo || transaction.seen_mail) {
        if (!write_line (output, "503 5.5.1 Bad command sequence", error))
          return FALSE;
        continue;
      }

      sender = extract_path_argument (line, "MAIL FROM:");
      if (sender == NULL) {
        if (!write_line (output, "501 5.5.4 Bad sender address", error))
          return FALSE;
        continue;
      }

      transaction.sender = g_steal_pointer (&sender);
      transaction.seen_mail = TRUE;
      if (!write_line (output, "250 2.1.0 Sender accepted", error))
        return FALSE;
      continue;
    }

    if (line_has_prefix_ci (line, "RCPT TO:")) {
      g_autofree char *recipient = NULL;

      if (!transaction.seen_mail) {
        if (!write_line (output, "503 5.5.1 Bad command sequence", error))
          return FALSE;
        continue;
      }

      recipient = extract_path_argument (line, "RCPT TO:");
      if (!is_valid_delivery_recipient (recipient)) {
        if (!write_line (output, "501 5.5.4 Bad recipient address", error))
          return FALSE;
        continue;
      }

      if (transaction.recipients == NULL)
        transaction.recipients = g_ptr_array_new_with_free_func (g_free);
      g_ptr_array_add (transaction.recipients, g_steal_pointer (&recipient));
      if (!write_line (output, "250 2.1.5 Recipient accepted", error))
        return FALSE;
      continue;
    }

    if (g_ascii_strcasecmp (line, "DATA") == 0) {
      if (transaction_recipient_count (&transaction) == 0) {
        if (!write_line (output, "503 5.5.1 Bad command sequence", error))
          return FALSE;
        continue;
      }

      g_clear_pointer (&transaction.message, g_byte_array_unref);
      transaction.message = g_byte_array_new ();

      if (!write_line (output, "354 End data with <CR><LF>.<CR><LF>", error))
        return FALSE;

      for (;;) {
        if (!read_crlf_line (input, raw_line, error))
          return FALSE;
        if (raw_line->len == 1 && raw_line->data[0] == '.')
          break;
        if (!append_data_line (&transaction,
                raw_line->data,
                raw_line->len, (guint64) options->max_message_bytes, error)) {
          if (!write_line_for_each_recipient (output,
                  &transaction, "552 5.3.4 Message size exceeds limit", error))
            return FALSE;
          return finish_after_transaction (output, error);
        }
      }

      if (!deliver_transaction (options, &transaction, output, error))
        return FALSE;
      return finish_after_transaction (output, error);
    }

    if (g_ascii_strcasecmp (line, "RSET") == 0) {
      reset_transaction (&transaction);
      if (!write_line (output, "250 2.0.0 Reset state", error))
        return FALSE;
      continue;
    }

    if (g_ascii_strcasecmp (line, "QUIT") == 0)
      return write_line (output, "221 2.0.0 Bye", error);

    if (!write_line (output, "500 5.5.2 Command not recognized", error))
      return FALSE;
  }
}

static gboolean
ensure_socket_parent_dir (const char *socket_path, GError **error)
{
  g_autofree char *parent_dir = g_path_get_dirname (socket_path);

  if (g_mkdir_with_parents (parent_dir, 0750) == 0)
    return TRUE;

  g_set_error (error,
      G_IO_ERROR,
      g_io_error_from_errno (errno),
      "failed to create listener socket directory '%s': %s",
      parent_dir, g_strerror (errno));
  return FALSE;
}

static gboolean
run_listener_once (const ListenerOptions *options, GError **error)
{
  g_autoptr (GSocketListener) listener = NULL;
  g_autoptr (GSocketAddress) address = NULL;
  g_autoptr (GSocketConnection) connection = NULL;

  if (!ensure_socket_parent_dir (options->listen_socket_path, error))
    return FALSE;

  (void) g_unlink (options->listen_socket_path);

  listener = g_socket_listener_new ();
  address = g_unix_socket_address_new (options->listen_socket_path);
  if (!g_socket_listener_add_address (listener,
          address,
          G_SOCKET_TYPE_STREAM, G_SOCKET_PROTOCOL_DEFAULT, NULL, NULL, error))
    return FALSE;

  connection = g_socket_listener_accept (listener, NULL, NULL, error);
  if (connection == NULL)
    return FALSE;

  if (!handle_connection (options, connection, error))
    return FALSE;

  (void) g_unlink (options->listen_socket_path);
  return TRUE;
}

int
main (int argc, char **argv)
{
  g_auto (ListenerOptions) options = { 0 };
  g_autoptr (GError) error = NULL;

  if (!parse_options (argc, argv, &options, &error)) {
    g_printerr ("wyrebox-postfix-lmtp-listener: %s\n", error->message);
    return 64;
  }

  if (!run_listener_once (&options, &error)) {
    g_printerr ("wyrebox-postfix-lmtp-listener: %s\n", error->message);
    (void) g_unlink (options.listen_socket_path);
    return 75;
  }

  return 0;
}
