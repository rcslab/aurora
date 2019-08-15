#include <stdio.h>
#include <stdarg.h>
#include <fcntl.h>
#include <unistd.h>
#include <dlfcn.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <libgen.h>

#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>

typedef int (*open_t)(const char *, int, ...);
typedef int (*openat_t)(int, const char *, int, ...);
typedef int (*close_t)(int);

typedef int (*dup_t)(int);
typedef int (*dup2_t)(int, int);

#define BUFSIZE (64 * 2 * 2 * 2)
#define LINK(path, r) link_path(path, r, AT_FDCWD, AT_FDCWD)

static open_t _open = NULL;
static openat_t _openat = NULL;
static close_t _close = NULL;
static dup_t _dup = NULL;
static dup2_t _dup2 = NULL;

static int init_flag = 0;
static const char * TMP_PATH = "/tmp/slstmp/";
static int log_fd = 0;

void 
__attribute__((constructor)) init()
{
    if (init_flag)
	return;

    _open = dlsym(RTLD_NEXT, "open");
    _openat = dlsym(RTLD_NEXT, "openat");
    _close = dlsym(RTLD_NEXT, "close");
    _dup = dlsym(RTLD_NEXT, "dup");
    _dup2 = dlsym(RTLD_NEXT, "dup2");

    init_flag = 1;
    char buf[BUFSIZE] = { };
    char spid[BUFSIZE] = {};
    int pid = getpid();
    sprintf(spid, "%d", pid);
    strcat(buf, TMP_PATH);

    int err = mkdir(TMP_PATH, ALLPERMS); 
    if ((errno != EEXIST && err != 0)) {
	printf("%s", strerror(errno));
	exit(1);
    }

    strcat(buf, spid);
    err = mkdir(buf, ALLPERMS);
    if (err != EEXIST && err != 0) {
	printf("%s", strerror(errno));
	exit(1);
    }

    strcat(buf, "/log");
    log_fd = _open(buf, O_CREAT | O_APPEND | O_RDWR, ALLPERMS);
    if (log_fd < 0) {
	printf("%s", strerror(errno));
	exit(1);
    }
}

void 
__attribute__((deconstructor)) deconstruct()
{
    char buf[BUFSIZE] = { };
    char spid[BUFSIZE] = {};
    int pid = getpid();
    sprintf(spid, "%d", pid);
    strcat(buf, TMP_PATH);
    strcat(buf, spid);

    printf("Removing %s\n", buf);
    int err = rmdir(buf);

    if (err != 0) {
	printf("%s", strerror(errno));
	exit(1);
    }
    close(log_fd);
}

static void
create_path(int fd, char * buf)
{
    strcat(buf, TMP_PATH);
    char spid[BUFSIZE] = {};
    int pid = getpid();
    sprintf(spid, "%d", pid);
    strcat(buf, spid);
    char sfd[BUFSIZE] = {};
    sprintf(sfd, "/%d", fd);
    strcat(buf, sfd);
}

static int
link_path(char * path, int fd, int at1, int at2)
{
    char full_path[BUFSIZE] = { };
    create_path(fd, &full_path);
    return linkat(at1, path, at2, full_path, 0);
}
static int
unlink_path(int fd)
{
    char full_path[BUFSIZE] = { };
    create_path(fd, &full_path);
    return unlink(full_path);
}

enum action { OPEN = 'O', OPENAT = 'A', CLOSE ='C', DUP = 'D', DUP2 = '2' };

int 
log(enum action a, int fd, int dup) 
{
    char buf[BUFSIZE] = { };
    char character[BUFSIZE] = { };
    character[0] = a;
    strcat(buf, character);

    char s[BUFSIZE] = { };
    sprintf(s, ":%d", fd);
    strcat(buf, s);
    bzero(s, BUFSIZE);

    switch(a) {
	case OPEN:
	case CLOSE:
	    sprintf(s, ":%d", -1);
	    break;
	case DUP:
	case DUP2:
	case OPENAT:
	    sprintf(s, ":%d", dup);
	    break;
    }
    strcat(buf, s);
    character[0] = '\n';
    strcat(buf, character);
    int err = write(log_fd, buf, strlen(buf));
    if (err < 0) {
	printf("%s\n", strerror(errno));
	exit(1);
    }
    return 0;
}

int is_dir(int fd)
{
    struct stat stats;
    int err = fstat(fd, &stats);
    if (err < 0) {
	printf("Problem fstating - %s\n", strerror(errno));
    }

    return S_ISDIR(stats.st_mode);
}

int
open(const char *path, int flags, ...)
{
    va_list args;

    init();

    va_start(args, flags);

    int r = _open(path, flags, args);
    va_end(args);
    int dir = is_dir(r);
    if (r < 0  || dir)
	return r;
    int err = LINK(path, r);
    if (err) {
	printf("Problem Creating File\n - %s", path);
	return EIO;
    }
    log(OPEN, r, 0);

    return r;
}

int
close(int fd)
{
    init();

    int dir = is_dir(fd);
    int r = _close(fd);

    if (r < 0 || dir ) 
	return r;

    int err = unlink_path(fd);
    if (err) {
	printf("Problem unlink fd %d\n", fd);
	return EIO;
    }
    log(CLOSE, fd, 0);

    return r;
}

int
dup(int oldd)
{
    int r =  _dup(oldd);
    if (r < 0)
	return r;

    char buf[BUFSIZE] = { };
    create_path(oldd, buf);
    int err = LINK(buf, r);
    if (err) {
	printf("Problem Creating File\n");
	return EIO;
    }
    log(DUP, oldd, r);

    return r;
}

int
dup2(int oldd, int newd)
{
    int r = _dup2(oldd, newd);
    if (r < 0)
	return r;

    char buf[BUFSIZE] = { };
    create_path(oldd, buf);
    int err = LINK(buf, newd);
    if (err) {
	printf("Problem Creating File\n");
	return EIO;
    }
    log(DUP2, oldd, newd);

    return r;
}

int
openat(int fd, const char *path, int flags, ...)
{
    init();

    va_list args;
    va_start(args, flags);

    int r = _openat(fd, path, flags, args);
    va_end(args);

    int dir = is_dir(r);
    if (r < 0 || dir)
	return r;

    int err = link_path(path, r, fd, AT_FDCWD);
    if (err) {
	printf("Problem Creating File %s\n", path);
	return EIO;
    }
    log(OPENAT, r, fd);

    return r;
}
