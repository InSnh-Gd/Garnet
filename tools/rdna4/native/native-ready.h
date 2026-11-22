#ifndef RDNA4_NATIVE_READY_H
#define RDNA4_NATIVE_READY_H
#ifdef __APPLE__
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pwd.h>
#include <stdio.h>
#include <string.h>
#include <sys/event.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

static double now(void);
static int openDirectory(const char *path, mode_t createMode);

#define N44_READY_SOCKET ".native-translate.ready"
#define N44_READY_PENDING ".native-translate/ready.pending"

static int readyListener(int kq) {
    const char *paths[] = { N44_READY_SOCKET, N44_READY_PENDING };
    for (unsigned i = 0; i < 2; i++) {
        struct stat st;
        if (lstat(paths[i], &st) == 0) {
            if (!S_ISSOCK(st.st_mode) || st.st_uid != geteuid() || st.st_nlink != 1) { errno = EACCES; return -1; }
            if (unlink(paths[i]) != 0) return -1;
        } else if (errno != ENOENT) return -1;
    }
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_un address = { .sun_len = sizeof address, .sun_family = AF_UNIX };
    strlcpy(address.sun_path, N44_READY_PENDING, sizeof address.sun_path);
    int bound = 0;
    struct kevent event;
    EV_SET(&event, fd, EVFILT_READ, EV_ADD, 0, 0, NULL);
    if (fcntl(fd, F_SETFD, FD_CLOEXEC) == 0 && fcntl(fd, F_SETFL, O_NONBLOCK) == 0 &&
        bind(fd, (const struct sockaddr *)&address, sizeof address) == 0) bound = 1;
    // Publish only after listen: a directory notification must never expose a
    // bound-but-not-listening socket to the waiting activation process.
    if (bound && chmod(N44_READY_PENDING, 0600) == 0 && listen(fd, 8) == 0 &&
        kevent(kq, &event, 1, NULL, 0, NULL) == 0 && rename(N44_READY_PENDING, N44_READY_SOCKET) == 0) return fd;
    int error = errno;
    close(fd);
    if (bound) (void)unlink(N44_READY_PENDING);
    errno = error;
    return -1;
}

static void readyReply(int listener) {
    for (unsigned count = 0; count < 16; count++) {
        int fd = accept(listener, NULL, NULL);
        if (fd < 0) { if (errno == EINTR) { count--; continue; } break; }
        (void)send(fd, "R", 1, MSG_DONTWAIT | MSG_NOSIGNAL);
        close(fd);
    }
}

static int readyPoll(int fd, short events, double deadline) {
    for (;;) {
        double left = deadline - now();
        if (left <= 0) { errno = ETIMEDOUT; return -1; }
        struct pollfd event = { .fd = fd, .events = events };
        int result = poll(&event, 1, (int)(left * 1000) + 1);
        if (result > 0) return 0;
        if (result < 0 && errno == EINTR) continue;
        if (!result) errno = ETIMEDOUT;
        return -1;
    }
}

// The socket cannot acknowledge until this live service has initialized. An old
// filesystem socket left by a crashed process cannot produce a readiness reply.
static int waitReady(const char *output, unsigned seconds) {
    struct passwd *account = getpwnam("nobody");
    if (!account) return 1;
    uid_t owner = account->pw_uid;
    char parent[PATH_MAX];
    if (strlcpy(parent, output, sizeof parent) >= sizeof parent) return 1;
    char *slash = strrchr(parent, '/');
    if (!slash) return 1;
    if (slash == parent) slash[1] = 0; else *slash = 0;
    int directory = openDirectory(parent, 0), kq = kqueue(), result = 1;
    if (directory < 0 || kq < 0) goto done;
    struct kevent watch;
    EV_SET(&watch, directory, EVFILT_VNODE, EV_ADD | EV_CLEAR, NOTE_WRITE | NOTE_RENAME | NOTE_DELETE, 0, NULL);
    if (kevent(kq, &watch, 1, NULL, 0, NULL) != 0) goto done;
    double deadline = now() + seconds;
    int announced = 0;
    while (now() < deadline) {
        int out = openDirectory(output, 0);
        if (out >= 0) {
            struct stat st;
            if (fstat(out, &st) != 0 || st.st_uid != owner || (st.st_mode & 0022) || fchdir(out) != 0) { close(out); break; }
            EV_SET(&watch, out, EVFILT_VNODE, EV_ADD | EV_CLEAR, NOTE_WRITE | NOTE_RENAME | NOTE_DELETE, 0, NULL);
            if (kevent(kq, &watch, 1, NULL, 0, NULL) != 0) { close(out); break; }
            int fd = socket(AF_UNIX, SOCK_STREAM, 0);
            if (fd < 0) { close(out); break; }
            struct sockaddr_un address = { .sun_len = sizeof address, .sun_family = AF_UNIX };
            strlcpy(address.sun_path, N44_READY_SOCKET, sizeof address.sun_path);
            if (fcntl(fd, F_SETFL, O_NONBLOCK) != 0) { close(fd); close(out); break; }
            int connected = connect(fd, (const struct sockaddr *)&address, sizeof address), error = errno;
            if (connected < 0 && error == EINPROGRESS) {
                socklen_t size = sizeof error;
                connected = readyPoll(fd, POLLOUT, deadline);
                if (!connected && (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &size) != 0 || error)) connected = -1;
            }
            if (!connected) {
                uid_t peer; gid_t group; char reply = 0;
                if (getpeereid(fd, &peer, &group) == 0 && peer == owner &&
                    readyPoll(fd, POLLIN, deadline) == 0 && recv(fd, &reply, 1, 0) == 1 && reply == 'R') result = 0;
                close(fd); close(out);
                break;
            }
            close(fd);
            if (error != ENOENT && error != ECONNREFUSED) { close(out); break; }
        } else if (errno != ENOENT) break;
        if (!announced) { puts("TRANSLATOR_WAITING"); fflush(stdout); announced = 1; }
        double left = deadline - now();
        if (left < 0) left = 0;
        struct timespec timeout = { (time_t)left, (long)((left - (time_t)left) * 1e9) };
        struct kevent event;
        int eventCount = kevent(kq, NULL, 0, &event, 1, &timeout), error = errno;
        if (out >= 0) close(out);
        if (eventCount < 0 && error == EINTR) continue;
        if (eventCount <= 0) break;
    }
done:
    if (directory >= 0) close(directory);
    if (kq >= 0) close(kq);
    if (!result) puts("TRANSLATOR_READY");
    else fprintf(stderr, "Translator readiness failed or timed out\n");
    return result;
}
#endif
#endif
