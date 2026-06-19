#pragma once

#include <gio/gio.h>

/* *INDENT-OFF* */
G_BEGIN_DECLS

typedef struct
{
  char *socket_path;
  gboolean json;
} WyreboxAdminHealthOptions;

typedef struct
{
  char *socket_path;
  char *status_name;
  gboolean healthy;
} WyreboxAdminHealthResult;

void wyrebox_admin_health_options_clear (WyreboxAdminHealthOptions *options);
void wyrebox_admin_health_result_clear (WyreboxAdminHealthResult *result);

G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (WyreboxAdminHealthOptions,
    wyrebox_admin_health_options_clear)
G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (WyreboxAdminHealthResult,
    wyrebox_admin_health_result_clear)

const char *wyrebox_admin_health_default_socket_path (void);

int wyrebox_admin_health_probe (const char *socket_path,
    WyreboxAdminHealthResult *result);

G_END_DECLS
/* *INDENT-ON* */
