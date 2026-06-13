#ifndef MODULE_DIR_H
#define MODULE_DIR_H

struct module;

struct module {
  void (*init)(struct module *module);
};

struct module_dir_load_settings {
  const char *abi_version;
  int require_init_funcs;
};

void *module_get_symbol(struct module *module, const char *name);

#endif
