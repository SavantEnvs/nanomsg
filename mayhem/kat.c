/*
 * Known-answer probe for mayhem/test.sh: drives nanomsg's public API with fixed inputs and prints
 * the observed results. test.sh compares the output line by line, from bash, so a neutered or
 * broken library cannot hide behind an exit code.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#include "../src/nn.h"
#include "../src/pair.h"
#include "../src/reqrep.h"
#include "../src/pipeline.h"
#include "../src/ws.h"

static void fail (const char *what)
{
    printf ("ERROR %s: %s\n", what, nn_strerror (nn_errno ()));
    exit (1);
}

static void kat_pair_iovec (void)
{
    int a = nn_socket (AF_SP, NN_PAIR), b = nn_socket (AF_SP, NN_PAIR);
    char buf[64];
    struct nn_iovec iov[2] = {{"hello ", 6}, {"nanomsg", 7}};
    struct nn_msghdr hdr;
    int n;

    if (a < 0 || b < 0 || nn_bind (a, "inproc://kat-pair") < 0 || nn_connect (b, "inproc://kat-pair") < 0)
        fail ("pair setup");
    memset (&hdr, 0, sizeof hdr);
    hdr.msg_iov = iov;
    hdr.msg_iovlen = 2;
    if (nn_sendmsg (b, &hdr, 0) != 13)
        fail ("sendmsg");
    n = nn_recv (a, buf, sizeof buf - 1, 0);
    if (n < 0)
        fail ("recv");
    buf[n] = 0;
    printf ("pair-iovec: %d %s\n", n, buf);
    nn_close (a);
    nn_close (b);
}

static void kat_reqrep (void)
{
    int rep = nn_socket (AF_SP, NN_REP), req = nn_socket (AF_SP, NN_REQ);
    char buf[64], out[64];
    int n;

    if (rep < 0 || req < 0 || nn_bind (rep, "inproc://kat-reqrep") < 0 || nn_connect (req, "inproc://kat-reqrep") < 0)
        fail ("reqrep setup");
    if (nn_send (req, "ABC", 3, 0) != 3)
        fail ("req send");
    n = nn_recv (rep, buf, sizeof buf, 0);
    if (n != 3)
        fail ("rep recv");
    snprintf (out, sizeof out, "echo:%.*s", n, buf);
    if (nn_send (rep, out, (int) strlen (out), 0) < 0)
        fail ("rep send");
    n = nn_recv (req, buf, sizeof buf - 1, 0);
    if (n < 0)
        fail ("req recv");
    buf[n] = 0;
    printf ("reqrep: %s\n", buf);
    nn_close (rep);
    nn_close (req);
}

static void kat_pipeline (void)
{
    int push = nn_socket (AF_SP, NN_PUSH), pull = nn_socket (AF_SP, NN_PULL);
    char line[64] = "pipeline:";
    int i;

    if (push < 0 || pull < 0 || nn_bind (pull, "inproc://kat-pipe") < 0 || nn_connect (push, "inproc://kat-pipe") < 0)
        fail ("pipeline setup");
    for (i = 1; i <= 3; i++) {
        char m[2] = {(char) ('0' + i), 0};
        if (nn_send (push, m, 1, 0) != 1)
            fail ("push");
    }
    for (i = 0; i < 3; i++) {
        char m[8];
        int n = nn_recv (pull, m, sizeof m, 0);
        if (n != 1)
            fail ("pull");
        snprintf (line + strlen (line), sizeof line - strlen (line), " %c", m[0]);
    }
    puts (line);
    nn_close (push);
    nn_close (pull);
}

/* RFC 6455 section 1.3 example: key dGhlIHNhbXBsZSBub25jZQ== must yield accept s3pPLMBiTxaQ9kYGzzhZRbK+xOo= */
static void kat_websocket (void)
{
    int s = nn_socket (AF_SP, NN_PAIR), c, port, n, total = 0;
    struct sockaddr_in in;
    socklen_t len = sizeof in;
    char addr[64], resp[2048], buf[32], *p;
    static const char req[] =
        "GET / HTTP/1.1\r\nHost: 127.0.0.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n"
        "Sec-WebSocket-Protocol: pair.sp.nanomsg.org\r\n\r\n";
    /* masked binary frame carrying "hi" (mask 01 02 03 04) */
    static const unsigned char frame[] = {0x82, 0x82, 0x01, 0x02, 0x03, 0x04, 'h' ^ 0x01, 'i' ^ 0x02};

    memset (&in, 0, sizeof in);
    in.sin_family = AF_INET;
    in.sin_addr.s_addr = htonl (INADDR_LOOPBACK);
    c = socket (AF_INET, SOCK_STREAM, 0);
    if (bind (c, (struct sockaddr *) &in, sizeof in) != 0 || getsockname (c, (struct sockaddr *) &in, &len) != 0)
        fail ("port probe");
    port = ntohs (in.sin_port);
    close (c);
    snprintf (addr, sizeof addr, "ws://127.0.0.1:%d", port);
    if (s < 0 || nn_bind (s, addr) < 0)
        fail ("ws bind");

    c = socket (AF_INET, SOCK_STREAM, 0);
    if (connect (c, (struct sockaddr *) &in, sizeof in) != 0)
        fail ("ws connect");
    if (send (c, req, sizeof req - 1, 0) != (ssize_t) sizeof req - 1)
        fail ("ws send request");
    while (total < (int) sizeof resp - 1) {
        n = (int) read (c, resp + total, sizeof resp - 1 - (size_t) total);
        if (n <= 0)
            break;
        total += n;
        resp[total] = 0;
        if (strstr (resp, "\r\n\r\n"))
            break;
    }
    resp[total] = 0;
    p = strstr (resp, "\r\n");
    if (p)
        *p = 0;
    printf ("ws-status: %s\n", resp);
    p = p ? strstr (p + 2, "Sec-WebSocket-Accept: ") : NULL;
    if (p) {
        char *e = strstr (p, "\r\n");
        if (e)
            *e = 0;
        printf ("ws-accept: %s\n", p + strlen ("Sec-WebSocket-Accept: "));
    } else
        printf ("ws-accept: <missing>\n");

    if (send (c, frame, sizeof frame, 0) != (ssize_t) sizeof frame)
        fail ("ws send frame");
    n = nn_recv (s, buf, sizeof buf - 1, 0);
    if (n < 0)
        fail ("ws recv");
    buf[n] = 0;
    printf ("ws-frame: %d %s\n", n, buf);
    close (c);
    nn_close (s);
}

int main (void)
{
    kat_pair_iovec ();
    kat_reqrep ();
    kat_pipeline ();
    kat_websocket ();
    printf ("KAT-DONE\n");
    return 0;
}
