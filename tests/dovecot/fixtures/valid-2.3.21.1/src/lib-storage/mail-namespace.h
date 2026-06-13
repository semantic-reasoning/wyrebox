#ifndef MAIL_NAMESPACE_H
#define MAIL_NAMESPACE_H

struct mail_user;

struct mail_namespace {
  struct mail_user *user, *owner;
};

#endif
