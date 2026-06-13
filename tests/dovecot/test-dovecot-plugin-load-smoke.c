#include "config.h"

#include "lib.h"
#include "module-dir.h"

#include <stdio.h>

#if defined(WYREBOX_DOVECOT_LOADER_STUB_REGISTRATION)
extern int wyrebox_dovecot_loader_shim_mail_storage_class_register_calls;
extern int wyrebox_dovecot_loader_shim_mail_storage_class_unregister_calls;
#endif

int
main (int argc, char **argv)
{
  struct module *modules = NULL;
  struct module_dir_load_settings settings = { 0 };

  if (argc != 2) {
    fprintf (stderr, "usage: %s PLUGIN_DIR\n", argv[0]);
    return 2;
  }

  settings.abi_version = DOVECOT_ABI_VERSION;
  settings.require_init_funcs = TRUE;

  lib_init ();
  modules = module_dir_load (argv[1], "wyrebox", &settings);
  if (modules == NULL) {
    fprintf (stderr, "failed to load wyrebox Dovecot plugin from %s\n",
        argv[1]);
    lib_deinit ();
    return 1;
  }

  module_dir_init (modules);
  module_dir_unload (&modules);
  lib_deinit ();

#if defined(WYREBOX_DOVECOT_LOADER_STUB_REGISTRATION)
  if (wyrebox_dovecot_loader_shim_mail_storage_class_register_calls == 0) {
    fprintf (stderr,
        "expected mail_storage_class_register() to be called by wyrebox plugin init\n");
    return 1;
  }

  if (wyrebox_dovecot_loader_shim_mail_storage_class_unregister_calls == 0) {
    fprintf (stderr,
        "expected mail_storage_class_unregister() to be called by wyrebox plugin deinit\n");
    return 1;
  }
#endif

  return 0;
}
