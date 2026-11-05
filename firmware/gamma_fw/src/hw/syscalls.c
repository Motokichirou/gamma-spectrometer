/* Заглушки системных вызовов newlib: ввода-вывода через stdio в прошивке нет.
   Без них линковка с nosys.specs шумит предупреждениями и прячет настоящие. */
#include <errno.h>
#include <sys/types.h>

int _close(int fd) { (void)fd; errno = EBADF; return -1; }
off_t _lseek(int fd, off_t off, int whence) { (void)fd; (void)off; (void)whence; return 0; }
int _read(int fd, char *buf, int len) { (void)fd; (void)buf; (void)len; errno = EBADF; return -1; }
int _write(int fd, const char *buf, int len) { (void)fd; (void)buf; (void)len; errno = EBADF; return -1; }
