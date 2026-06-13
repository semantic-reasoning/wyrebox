#ifndef MAIL_STORAGE_H
#define MAIL_STORAGE_H

struct message_size;

struct mailbox;
struct mail;
struct mail_storage;

enum mailbox_flags {
  MAILBOX_FLAG_READONLY = 0x01,
};

enum mailbox_status_items {
  STATUS_MESSAGES = 0x01,
  STATUS_RECENT = 0x02,
  STATUS_UIDNEXT = 0x04,
  STATUS_UIDVALIDITY = 0x08,
  STATUS_UNSEEN = 0x10,
};

struct mailbox_status {
  unsigned int messages;
  unsigned int uidvalidity;
  unsigned int uidnext;
};

void mail_storage_class_register(struct mail_storage *storage_class);
void mail_storage_class_unregister(struct mail_storage *storage_class);

#endif
