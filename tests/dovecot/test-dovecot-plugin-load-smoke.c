#include "config.h"

#include "lib.h"
#include "module-dir.h"

#include <stdio.h>

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

  return 0;
}
