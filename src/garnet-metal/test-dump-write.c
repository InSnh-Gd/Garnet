#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct { const void *bytes; size_t length; } rdna4_test_data;
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static unsigned arrived[2];
static int concurrent;
static int fail_rename;
static int clobber_unlink_errno;
static unsigned interrupted_writes;
static size_t write_limit;
static int fail_write;
static int zero_write;
static int fail_chmod;
static int fail_close;

static void rendezvous(unsigned phase) {
    pthread_mutex_lock(&mutex);
    if (++arrived[phase] == 2) pthread_cond_broadcast(&changed);
    while (arrived[phase] < 2) pthread_cond_wait(&changed, &mutex);
    pthread_mutex_unlock(&mutex);
}

static int checked_lstat(const char *path, struct stat *st) {
    int rc = lstat(path, st), saved = errno;
    if (concurrent) rendezvous(0);
    errno = saved;
    return rc;
}

static int checked_rename(const char *from, const char *to) {
    if (fail_rename) { errno = EACCES; return -1; }
    if (concurrent) rendezvous(1);
    return rename(from, to);
}

static int checked_unlink(const char *path) {
    int rc = unlink(path);
    if (clobber_unlink_errno) errno = ENOENT;
    return rc;
}

static ssize_t checked_write(int fd, const void *bytes, size_t count) {
    if (interrupted_writes) { --interrupted_writes; errno = EINTR; return -1; }
    if (fail_write) { errno = ENOSPC; return -1; }
    if (zero_write) return 0;
    if (write_limit && count > write_limit) count = write_limit;
    return write(fd, bytes, count);
}

static int checked_fchmod(int fd, mode_t mode) {
    if (fail_chmod) { errno = EPERM; return -1; }
    return fchmod(fd, mode);
}

static int checked_close(int fd) {
    int rc = close(fd);
    if (fail_close) { errno = EIO; return -1; }
    return rc;
}

#define lstat checked_lstat
#define rename checked_rename
#define unlink checked_unlink
#define write checked_write
#define fchmod checked_fchmod
#define close checked_close
#include RDNA4_DUMP_WRITER
#undef close
#undef fchmod
#undef write
#undef unlink
#undef rename
#undef lstat

typedef struct { const char *path; rdna4_test_data data; int result; } writer;
static void *publish(void *arg) {
    writer *w = arg;
    w->result = rdna4_dump_write(w->path, &w->data);
    return NULL;
}

static int rejected_write(const char *path, rdna4_test_data *data, int expected_errno) {
    clobber_unlink_errno = 1;
    errno = 0;
    int rc = rdna4_dump_write(path, data);
    int saved = errno;
    clobber_unlink_errno = 0;
    struct stat st;
    int absent = lstat(path, &st) != 0 && errno == ENOENT;
    printf("DUMP_WRITE rejected=%d absent=%d errno=%d expected=%d\n", !rc, absent, saved, expected_errno);
    return !rc && absent && saved == expected_errno;
}

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    char path[1024];
    if (snprintf(path, sizeof path, "%s/shader.air", argv[1]) >= (int)sizeof path) return 2;
    unsigned char payload[6880], actual[sizeof payload];
    for (size_t i = 0; i < sizeof payload; ++i) payload[i] = (unsigned char)(i * 37u);
    writer w[2] = { {path, {payload, sizeof payload}, 0}, {path, {payload, sizeof payload}, 0} };
    pthread_t threads[2];
    concurrent = 1;
    for (unsigned i = 0; i < 2; ++i) if (pthread_create(&threads[i], NULL, publish, &w[i])) return 2;
    for (unsigned i = 0; i < 2; ++i) if (pthread_join(threads[i], NULL)) return 2;
    concurrent = 0;
    int fd = open(path, O_RDONLY | O_NOFOLLOW);
    struct stat st = {0};
    int content = fd >= 0 && fstat(fd, &st) == 0 && st.st_size == sizeof payload &&
        read(fd, actual, sizeof actual) == sizeof actual && !memcmp(payload, actual, sizeof actual);
    if (fd >= 0) close(fd);
    int idempotent = rdna4_dump_write(path, &w[0].data);
    char failure_path[1024];
    if (snprintf(failure_path, sizeof failure_path, "%s/failure.air", argv[1]) >= (int)sizeof failure_path) return 2;
    fail_rename = 1;
    int failures_ok = rejected_write(failure_path, &w[0].data, EACCES);
    fail_rename = 0;
    fail_write = fail_close = 1;
    failures_ok &= rejected_write(failure_path, &w[0].data, ENOSPC);
    fail_write = fail_close = 0;
    zero_write = 1;
    failures_ok &= rejected_write(failure_path, &w[0].data, EIO);
    zero_write = 0;
    fail_chmod = 1;
    failures_ok &= rejected_write(failure_path, &w[0].data, EPERM);
    fail_chmod = 0;
    fail_close = 1;
    failures_ok &= rejected_write(failure_path, &w[0].data, EIO);
    fail_close = 0;
    interrupted_writes = 1;
    write_limit = 73;
    int interrupted_ok = rdna4_dump_write(failure_path, &w[0].data) && !interrupted_writes;
    write_limit = 0;
    fd = open(failure_path, O_RDONLY | O_NOFOLLOW);
    interrupted_ok &= fd >= 0 && read(fd, actual, sizeof actual) == sizeof actual && !memcmp(payload, actual, sizeof actual);
    if (fd >= 0) close(fd);
    unsigned leftovers = 0;
    DIR *dir = opendir(argv[1]);
    if (!dir) return 2;
    struct dirent *entry;
    while ((entry = readdir(dir))) if (strstr(entry->d_name, ".tmp")) ++leftovers;
    closedir(dir);
    printf("DUMP_WRITE concurrent_success=%d/2 exact_bytes=%d mode=%o idempotent=%d failures_ok=%d interrupted_and_short=%d temp_leftovers=%u\n",
           w[0].result + w[1].result, content, (unsigned)(st.st_mode & 0777), idempotent,
           failures_ok, interrupted_ok, leftovers);
    return !(w[0].result && w[1].result && content && (st.st_mode & 0777) == 0644 && idempotent &&
             failures_ok && interrupted_ok && !leftovers);
}
