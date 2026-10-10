#ifndef MESSAGE_SIZE_H
#define MESSAGE_SIZE_H

#include "lib.h"

struct message_size
{
  uoff_t physical_size;
  uoff_t virtual_size;
  unsigned int lines;
};

#endif
