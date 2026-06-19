#pragma once

#include <glib-object.h>

/* *INDENT-OFF* */
G_BEGIN_DECLS

typedef enum
{
  WYREBOX_ADMIN_READINESS_STATUS_READY,
  WYREBOX_ADMIN_READINESS_STATUS_REPLAYING,
  WYREBOX_ADMIN_READINESS_STATUS_MATERIALIZING,
  WYREBOX_ADMIN_READINESS_STATUS_READ_ONLY,
  WYREBOX_ADMIN_READINESS_STATUS_UNAVAILABLE,
} WyreboxAdminReadinessStatus;

const char *wyrebox_admin_readiness_status_state_to_name (
    WyreboxAdminReadinessStatus state);

G_END_DECLS
/* *INDENT-ON* */
