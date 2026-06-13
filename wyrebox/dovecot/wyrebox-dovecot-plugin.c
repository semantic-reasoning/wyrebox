#include "config.h"

#include "lib.h"
#include "module-dir.h"

#define WYREBOX_DOVECOT_PLUGIN_EXPORT __attribute__ ((visibility ("default")))

WYREBOX_DOVECOT_PLUGIN_EXPORT const char *wyrebox_plugin_version =
    DOVECOT_ABI_VERSION;

WYREBOX_DOVECOT_PLUGIN_EXPORT void
wyrebox_plugin_init (struct module *module)
{
  (void) module;
}

WYREBOX_DOVECOT_PLUGIN_EXPORT void
wyrebox_plugin_deinit (void)
{
}
