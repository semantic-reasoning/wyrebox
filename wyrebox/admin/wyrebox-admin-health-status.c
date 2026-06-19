#include "wyrebox-admin-health-status.h"

const char *
wyrebox_admin_health_status_state_to_name (WyreboxAdminHealthStatus state)
{
  switch (state) {
    case WYREBOX_ADMIN_HEALTH_STATUS_HEALTHY:
      return "healthy";
    case WYREBOX_ADMIN_HEALTH_STATUS_DEGRADED:
      return "degraded";
    case WYREBOX_ADMIN_HEALTH_STATUS_REPLAYING:
      return "replaying";
    case WYREBOX_ADMIN_HEALTH_STATUS_MATERIALIZING:
      return "materializing";
    case WYREBOX_ADMIN_HEALTH_STATUS_READ_ONLY:
      return "read-only";
    case WYREBOX_ADMIN_HEALTH_STATUS_STORAGE_FULL:
      return "storage-full";
    case WYREBOX_ADMIN_HEALTH_STATUS_STORAGE_ERROR:
      return "storage-error";
    case WYREBOX_ADMIN_HEALTH_STATUS_UNAVAILABLE:
    default:
      return "unavailable";
  }
}
