#ifndef MAIL_STORAGE_H
#define MAIL_STORAGE_H

struct message_size;

struct mailbox;
struct mail;
struct mail_storage;

struct mailbox_status {
  unsigned int messages;
  unsigned int uidvalidity;
  unsigned int uidnext;
};

#endif
