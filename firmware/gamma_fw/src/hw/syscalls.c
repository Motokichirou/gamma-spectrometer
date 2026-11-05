/* Заглушки системных вызовов newlib: ввода-вывода через stdio в прошивке нет.
   Без них линковка с nosys.specs шумит предупреждениями ("_fstat is not implemented…")
   и прячет настоящие. */
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/stat.h>

int _close(int fd) { (void)fd; errno = EBADF; return -1; }
off_t _lseek(int fd, off_t off, int whence) { (void)fd; (void)off; (void)whence; return 0; }
int _read(int fd, char *buf, int len) { (void)fd; (void)buf; (void)len; errno = EBADF; return -1; }
int _write(int fd, const char *buf, int len) { (void)fd; (void)buf; (void)len; errno = EBADF; return -1; }
int _fstat(int fd, struct stat *st) { (void)fd; st->st_mode = S_IFCHR; return 0; }
int _isatty(int fd) { (void)fd; return 1; }
int _getpid(void) { return 1; }
int _kill(int pid, int sig) { (void)pid; (void)sig; errno = EINVAL; return -1; }
void _exit(int status) { (void)status; for (;;) { } }

/* Куча — от end до _heap_limit (gamma_fw.ld, _Min_Heap_Size); дальше — отказ, а не наезд на стек.
   Нужна strtod newlib (Balloc) в разборе команд. */
extern uint8_t end, _heap_limit;
void *_sbrk(ptrdiff_t incr)
{
    static uint8_t *brk = &end;
    uint8_t *prev = brk;
    if (incr < 0 || (ptrdiff_t)(&_heap_limit - brk) < incr) {
        errno = ENOMEM;
        return (void *)-1;
    }
    brk += incr;
    return prev;
}
