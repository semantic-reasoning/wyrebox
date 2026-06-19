#include "wyrebox-admin-readiness-status.h"

const char *
wyrebox_admin_readiness_status_state_to_name (WyreboxAdminReadinessStatus state)
{
  switch (state) {
    case WYREBOX_ADMIN_READINESS_STATUS_READY:
      return "ready";
    case WYREBOX_ADMIN_READINESS_STATUS_REPLAYING:
      return "replaying";
    case WYREBOX_ADMIN_READINESS_STATUS_MATERIALIZING:
      return "materializing";
    case WYREBOX_ADMIN_READINESS_STATUS_READ_ONLY:
      return "read-only";
    case WYREBOX_ADMIN_READINESS_STATUS_UNAVAILABLE:
    default:
      return "unavailable";
  }
}
