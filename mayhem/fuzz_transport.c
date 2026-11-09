/*
 * Fuzz harness for nanomsg's byte-stream transports. One source, three binaries:
 *   -DFUZZ_IPC  ipc://  SP framing over an AF_UNIX stream (sipc.c)
 *   -DFUZZ_TCP  tcp://  SP framing over TCP (stcp.c)
 *   -DFUZZ_WS   ws://   HTTP upgrade handshake + WebSocket frames (ws_handshake.c, sws.c)
 *
 * Each input binds a real nanomsg socket and plays the part of a remote peer with a plain OS
 * socket: it sends the input bytes verbatim, half-closes, and waits until the library closes the
 * connection. nanomsg reads the next message from a pipe only once the application has received
 * the previous one, so the harness drains the socket (answering requests and surveys) while it
 * waits. The library closes the connection only after it has consumed the stream, so no timer or
 * sleep is needed. The first input byte picks the local socket type; the rest is the
 * peer's byte stream (a valid SP handshake is a seed, not something the harness writes).
 */
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>
#include <unistd.h>

#include "../src/nn.h"
#include "../src/pair.h"
#include "../src/reqrep.h"
#include "../src/pipeline.h"
#include "../src/pubsub.h"
#include "../src/survey.h"
#include "../src/bus.h"

#if defined(FUZZ_IPC)
#define FAMILY AF_UNIX
#elif defined(FUZZ_TCP) || defined(FUZZ_WS)
#define FAMILY AF_INET
#else
#error "define one of FUZZ_IPC, FUZZ_TCP, FUZZ_WS"
#endif

static const int socket_types[] = {
    NN_PAIR, NN_REP, NN_PULL, NN_SUB, NN_RESPONDENT, NN_BUS
};
#define NTYPES (sizeof (socket_types) / sizeof (socket_types[0]))

static char nn_addr[256];
static struct sockaddr_storage peer_addr;
static socklen_t peer_len;

static void die (const char *what)
{
    fprintf (stderr, "fuzz_transport: %s failed: errno %d\n", what, errno);
    abort ();
}

int LLVMFuzzerInitialize (int *argc, char ***argv)
{
    (void) argc;
    (void) argv;
    signal (SIGPIPE, SIG_IGN);

#if defined(FUZZ_IPC)
    {
        const char *dir = getenv ("TMPDIR");
        struct sockaddr_un *un = (struct sockaddr_un *) &peer_addr;
        char path[sizeof (un->sun_path)];

        if (!dir || !*dir)
            dir = "/tmp";
        snprintf (path, sizeof path, "%s/nn-fuzz-%d.ipc", dir, (int) getpid ());
        snprintf (nn_addr, sizeof nn_addr, "ipc://%s", path);
        memset (un, 0, sizeof *un);
        un->sun_family = AF_UNIX;
        memcpy (un->sun_path, path, strlen (path) + 1);
        peer_len = sizeof *un;
    }
#else
    {
        /* Let the kernel pick a free loopback port, then hand it to nanomsg. */
        struct sockaddr_in *in = (struct sockaddr_in *) &peer_addr;
        socklen_t len = sizeof *in;
        int probe = socket (AF_INET, SOCK_STREAM, 0);

        if (probe < 0)
            die ("socket");
        memset (in, 0, sizeof *in);
        in->sin_family = AF_INET;
        in->sin_addr.s_addr = htonl (INADDR_LOOPBACK);
        if (bind (probe, (struct sockaddr *) in, sizeof *in) != 0)
            die ("bind");
        if (getsockname (probe, (struct sockaddr *) in, &len) != 0)
            die ("getsockname");
        close (probe);
        peer_len = sizeof *in;
#ifdef FUZZ_WS
        snprintf (nn_addr, sizeof nn_addr, "ws://127.0.0.1:%d", ntohs (in->sin_port));
#else
        snprintf (nn_addr, sizeof nn_addr, "tcp://127.0.0.1:%d", ntohs (in->sin_port));
#endif
    }
#endif
    return 0;
}

int LLVMFuzzerTestOneInput (const uint8_t *data, size_t size)
{
    int s, fd, type;
    size_t off;

    if (size < 1)
        return -1;
    type = socket_types[data[0] % NTYPES];
    data++;
    size--;

    s = nn_socket (AF_SP, type);
    if (s < 0)
        die ("nn_socket");
    if (type == NN_SUB && nn_setsockopt (s, NN_SUB, NN_SUB_SUBSCRIBE, "", 0) != 0)
        die ("nn_setsockopt");
    if (nn_bind (s, nn_addr) < 0)
        die ("nn_bind");

    fd = socket (FAMILY, SOCK_STREAM, 0);
    if (fd < 0)
        die ("socket");
    if (connect (fd, (struct sockaddr *) &peer_addr, peer_len) != 0)
        die ("connect");

    for (off = 0; off < size;) {
        ssize_t n = send (fd, data + off, size - off, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        off += (size_t) n;
    }
    shutdown (fd, SHUT_WR);

    {
        int rcvfd = -1;
        size_t fdsz = sizeof rcvfd;
        struct pollfd pfd[2];
        int done = 0;

        if (nn_getsockopt (s, NN_SOL_SOCKET, NN_RCVFD, &rcvfd, &fdsz) != 0)
            die ("nn_getsockopt");
        pfd[0].fd = fd;
        pfd[0].events = POLLIN;
        pfd[1].fd = rcvfd;
        pfd[1].events = POLLIN;
        while (!done) {
            if (poll (pfd, 2, -1) < 0) {
                if (errno == EINTR)
                    continue;
                die ("poll");
            }
            if (pfd[1].revents & POLLIN) {
                for (;;) {
                    void *msg;
                    int n = nn_recv (s, &msg, NN_MSG, NN_DONTWAIT);
                    if (n < 0)
                        break;
                    nn_freemsg (msg);
                    if (type == NN_REP || type == NN_RESPONDENT)
                        nn_send (s, "r", 1, NN_DONTWAIT);
                }
            }
            if (pfd[0].revents & (POLLIN | POLLHUP | POLLERR)) {
                char sink[4096];
                ssize_t n = read (fd, sink, sizeof sink);
                if (n < 0 && errno == EINTR)
                    continue;
                if (n <= 0)
                    done = 1;
            }
        }
    }
    close (fd);

    if (nn_close (s) != 0)
        die ("nn_close");
    return 0;
}
