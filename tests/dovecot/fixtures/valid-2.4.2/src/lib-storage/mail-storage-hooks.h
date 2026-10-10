#ifndef MAIL_STORAGE_HOOKS_H
#define MAIL_STORAGE_HOOKS_H

struct module;
struct mail_user;
struct mail_storage;
struct mail_namespace;
struct mailbox_list;
struct mailbox;
struct mail;

struct mail_storage_hooks {
  void (*mail_user_created)(struct mail_user *user);
};

void mail_storage_hooks_add(struct module *module,
                           const struct mail_storage_hooks *hooks);

#endif
