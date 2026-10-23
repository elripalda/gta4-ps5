// LD_PRELOAD fixture: model a PS5 title refusing lstat without following links instead.
#include <errno.h>
#include <sys/stat.h>

int lstat(const char* path, struct stat* status) {
  (void)path;
  (void)status;
  errno = EPERM;
  return -1;
}
