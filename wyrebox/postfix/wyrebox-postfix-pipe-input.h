#pragma once

#include "wyrebox-daemon-delivery-ingestion-request.h"
#include "wyrebox-daemon-request-identity.h"

#include <gio/gio.h>

/* *INDENT-OFF* */
G_BEGIN_DECLS

#define WYREBOX_POSTFIX_PIPE_DEFAULT_SOCKET_PATH "/run/wyrebox/wyrebox.sock"

typedef struct
{
  /*
   * Owned daemon Unix-domain socket path. Defaults to
   * WYREBOX_POSTFIX_PIPE_DEFAULT_SOCKET_PATH when --socket is omitted.
   */
  char *socket_path;
} WyreboxPostfixPipeOptions;

void wyrebox_postfix_pipe_options_clear (
    WyreboxPostfixPipeOptions *options);

G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (WyreboxPostfixPipeOptions,
    wyrebox_postfix_pipe_options_clear)

/*
 * Parses Postfix pipe-helper argv and reads exact message bytes from @input.
 *
 * On success, @options, @identity, and @request replace any previous contents.
 * On failure, all output structs are cleared to reusable empty state.
 */
gboolean wyrebox_postfix_pipe_input_build (
    int argc,
    const char * const *argv,
    GInputStream *input,
    gsize max_message_bytes,
    WyreboxPostfixPipeOptions *options,
    WyreboxDaemonRequestIdentity *identity,
    WyreboxDaemonDeliveryIngestionRequest *request,
    GError **error);

G_END_DECLS
/* *INDENT-ON* */
