/*
 * Android 11+ wireless debugging TLS-connect support for the legacy
 * (android-5.0.2) adb host.
 *
 * The wireless debugging "connect" port of a stock Android 11+ device is a
 * TLS server that stays silent until the host speaks first.  The handshake
 * is:
 *
 *   host: TCP connect
 *   host: CNXN (version, maxdata)
 *   dev:  STLS (TLS upgrade request, arg0 = 0x01000000)
 *   host: STLS (acknowledge)
 *   host: TLS 1.3 ClientHello (client certificate = adbkey)
 *   dev:  ...TLS handshake, verifies the presented cert public key against
 *         the authorized keys in adb_keys, then sends its CNXN inside TLS
 *
 * After TLS is up the device sends CNXN on its own; the adb protocol then
 * runs over the TLS channel.  This module performs that upgrade and then
 * bridges the TLS socket to a plain socketpair whose other end is handed to
 * the normal adb transport layer, so the legacy transport/main-loop code is
 * never touched and legacy (plaintext, device-speaks-first) connects keep
 * working unchanged.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <limits.h>
#include <time.h>

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <openssl/pem.h>
#include <openssl/bn.h>

#include "sysdeps.h"
#include "adb.h"

/* adb wire commands (little-endian on the wire, host is little-endian) */
#define TLS_A_STLS     0x534c5453u   /* 'STLS' */
#define TLS_A_CNXN     0x4e584e43u   /* 'CNXN' */
#define TLS_A_VERSION  0x01000000u   /* protocol version we claim */
#define TLS_STLS_VERSION 0x01000000u
#define TLS_MAX_PAYLOAD 4096u

/* Locate the adb RSA key the same way the pairing code does: honour
 * $ANDROID_SDK_HOME/$HOME first, then /root, then /. */
static int adb_tls_keyfile(char *out, size_t outlen)
{
    const char *candidates[3];
    char base[PATH_MAX];
    const char *home = getenv("ANDROID_SDK_HOME");
    int n = 0;

    if (!home || !*home)
        home = getenv("HOME");
    if (home && *home && strcmp(home, "/")) {
        snprintf(base, sizeof(base), "%s/.android", home);
        candidates[n++] = base;
    }
    candidates[n++] = "/root/.android";
    candidates[n++] = "/.android";

    while (n-- > 0) {
        char p[PATH_MAX];
        struct stat st;
        snprintf(p, sizeof(p), "%s/adbkey", candidates[n]);
        if (stat(p, &st) == 0 && S_ISREG(st.st_mode))
            return snprintf(out, outlen, "%s", p);
    }
    return -1;
}

static EVP_PKEY *adb_tls_load_key(void)
{
    char path[PATH_MAX];
    FILE *f;
    RSA *rsa;
    EVP_PKEY *pkey;

    if (adb_tls_keyfile(path, sizeof(path)) < 0)
        return NULL;
    f = fopen(path, "r");
    if (!f)
        return NULL;
    rsa = PEM_read_RSAPrivateKey(f, NULL, NULL, NULL);
    fclose(f);
    if (!rsa)
        return NULL;
    pkey = EVP_PKEY_new();
    if (!pkey || EVP_PKEY_set1_RSA(pkey, rsa) != 1) {
        if (pkey)
            EVP_PKEY_free(pkey);
        RSA_free(rsa);
        return NULL;
    }
    RSA_free(rsa);
    return pkey;
}

/* Build a short-lived self-signed certificate whose public key is the adb
 * key.  The device does not verify the chain: it only compares the public
 * key of the presented certificate against its authorized adb_keys. */
static X509 *adb_tls_make_cert(EVP_PKEY *pkey)
{
    X509 *x;
    X509_NAME *name;
    BIGNUM *bn;
    unsigned char cn[] = "adbkey";

    x = X509_new();
    if (!x)
        return NULL;
    X509_set_version(x, 2);
    bn = BN_new();
    BN_pseudo_rand(bn, 64, 0, 0);
    BN_to_ASN1_INTEGER(bn, X509_get_serialNumber(x));
    BN_free(bn);
    X509_gmtime_adj(X509_getm_notBefore(x), -60L);
    X509_gmtime_adj(X509_getm_notAfter(x), 86400L);
    if (!X509_set_pubkey(x, pkey)) {
        X509_free(x);
        return NULL;
    }
    name = X509_get_subject_name(x);
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, cn, -1, -1, 0);
    X509_set_issuer_name(x, name);          /* self-signed */
    if (!X509_sign(x, pkey, EVP_sha256())) {
        X509_free(x);
        return NULL;
    }
    return x;
}

/* write a complete adb message (24 byte header + optional payload) */
static int adb_tls_write_msg(int fd, unsigned cmd, unsigned arg0, unsigned arg1,
                             const void *payload, unsigned plen)
{
    amessage msg;
    const unsigned char *p = payload;
    unsigned sum = 0, i;

    for (i = 0; i < plen; i++)
        sum += p[i];
    memset(&msg, 0, sizeof(msg));
    msg.command = cmd;
    msg.arg0 = arg0;
    msg.arg1 = arg1;
    msg.data_length = plen;
    msg.data_check = sum;
    msg.magic = cmd ^ 0xffffffffu;
    if (writex(fd, &msg, sizeof(msg)))
        return -1;
    if (plen && writex(fd, payload, plen))
        return -1;
    return 0;
}

/* wait up to ms for inbound data; 1 if readable, 0 on timeout, -1 on error */
int adb_tls_peek_data(int fd, int ms)
{
    struct pollfd pfd;
    int r;
    pfd.fd = fd;
    pfd.events = POLLIN;
    r = poll(&pfd, 1, ms);
    if (r < 0)
        return -1;
    if (r == 0)
        return 0;
    return (pfd.revents & (POLLIN | POLLHUP | POLLERR)) ? 1 : 0;
}

struct adb_tls_relay {
    SSL *ssl;
    int peer;          /* plaintext socketpair end feeding the adb server */
    int sfd;           /* underlying device socket */
    char *serial;
};

static int adb_tls_write_all(int fd, const void *buf, size_t len)
{
    const char *p = buf;
    while (len > 0) {
        int n = adb_write(fd, p, len);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

static int adb_tls_debug_enabled(void)
{
    static int v = -1;
    if (v < 0)
        v = getenv("ADB_TLS_DEBUG") ? 1 : 0;
    return v;
}

static void adb_tls_dbg(const char *fmt, ...)
{
    va_list ap;
    FILE *f;

    if (!adb_tls_debug_enabled())
        return;
    f = fopen("/tmp/relay.log", "a");
    if (!f)
        return;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fprintf(f, "\n");
    fclose(f);
}

static int adb_tls_ssl_write_all(SSL *ssl, int sfd, int peer,
                                 const void *buf, size_t len)
{
    const char *p = buf;
    while (len > 0) {
        int n = SSL_write(ssl, p, len > INT_MAX ? INT_MAX : (int)len);
        if (n > 0) {
            p += n;
            len -= (size_t)n;
            continue;
        }
        {
            int e = SSL_get_error(ssl, n);
            adb_tls_dbg("relay ssl_write err=%d want=%d left=%d", n, e, (int)len);
            if (e == SSL_ERROR_WANT_WRITE) {
                struct pollfd pw;
                pw.fd = sfd;
                pw.events = POLLOUT;
                poll(&pw, 1, 5000);
                continue;
            }
            if (e == SSL_ERROR_WANT_READ) {
                /* TLS 1.3 post-handshake data (e.g. NewSessionTicket) can
                 * arrive while we want to write; the write then needs us to
                 * consume it first.  Read whatever the device sent and
                 * forward it, then retry the write. */
                struct pollfd pr;
                char tmp[4096];
                int prc;
                pr.fd = sfd;
                pr.events = POLLIN;
                prc = poll(&pr, 1, 3000);
                adb_tls_dbg("relay ssl_write WANT_READ poll=%d rev=%x", prc, pr.revents);
                if (prc <= 0)
                    return -1;
                for (;;) {
                    int r = SSL_read(ssl, tmp, sizeof(tmp));
                    adb_tls_dbg("relay ssl_write drain SSL_read=%d", r);
                    if (r > 0) {
                        if (adb_tls_write_all(peer, tmp, (size_t)r))
                            return -1;
                        continue;
                    }
                    if (SSL_get_error(ssl, r) == SSL_ERROR_WANT_READ)
                        break;
                    return -1;
                }
                continue;
            }
            return -1;
        }
    }
    return 0;
}

/* Bridge the TLS channel and the plain transport end.  Blocking I/O with a
 * poll on both descriptors; when one side goes away the other is woken up
 * by shutting the underlying socket down. */
static void adb_tls_hex(const char *tag, const void *buf, int len)
{
    const unsigned char *p = buf;
    int i, n = len < 12 ? len : 12;
    char hex[64];
    for (i = 0; i < n; i++)
        snprintf(hex + i * 2, sizeof(hex) - i * 2, "%02x", p[i]);
    adb_tls_dbg("%s hex[%d]=%s", tag, len, hex);
}

static void *adb_tls_relay_thread(void *arg)
{
    struct adb_tls_relay *r = arg;
    char buf[32768];
    int skip_first_cnxn = 1;

    adb_tls_dbg("relay start sfd=%d peer=%d", r->sfd, r->peer);
    for (;;) {
        struct pollfd pf[2];
        int n;

        pf[0].fd = r->sfd;
        pf[0].events = POLLIN;
        pf[0].revents = 0;
        pf[1].fd = r->peer;
        pf[1].events = POLLIN;
        pf[1].revents = 0;
        n = poll(pf, 2, -1);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            adb_tls_dbg("relay poll err %d", errno);
            break;
        }
        if (pf[0].revents & (POLLIN | POLLHUP | POLLERR)) {
            n = SSL_read(r->ssl, buf, sizeof(buf));
            adb_tls_dbg("relay sfd->peer SSL_read=%d", n);
            adb_tls_hex("dev->srv", buf, n);
            if (n <= 0) {
                if (SSL_get_error(r->ssl, n) == SSL_ERROR_WANT_READ)
                    continue;
                break;                      /* device side gone */
            }
            if (adb_tls_write_all(r->peer, buf, (size_t)n))
                break;
            adb_tls_dbg("relay sfd->peer fwd=%d", n);
        }
        if (pf[1].revents & (POLLIN | POLLHUP | POLLERR)) {
            n = adb_read(r->peer, buf, sizeof(buf));
            adb_tls_dbg("relay peer->sfd read=%d", n);
            adb_tls_hex("srv->dev", buf, n);
            if (n <= 0)
                break;                      /* adb server side gone */
            if (skip_first_cnxn) {
                /* The legacy host answers the registration SYNC by sending
                 * its own CNXN.  Over the TLS channel the device treats any
                 * received CNXN as a new TLS-upgrade request and answers
                 * with STLS, after which it waits for another TLS handshake
                 * and stops serving.  The device already sends its own CNXN
                 * right after the TLS handshake, so this first host CNXN is
                 * redundant: drop it to keep the device in the serving
                 * state. */
                skip_first_cnxn = 0;
                if (n >= 4 && !memcmp(buf, "CNXN", 4)) {
                    adb_tls_dbg("relay dropped duplicate host CNXN");
                    continue;
                }
            }
            if (adb_tls_ssl_write_all(r->ssl, r->sfd, r->peer, buf, (size_t)n)) {
                adb_tls_dbg("relay peer->sfd write FAILED");
                break;
            }
            adb_tls_dbg("relay peer->sfd wrote=%d", n);
        }
    }

    adb_tls_dbg("relay exit");
    /* Release everything; closing our socketpair end makes the adb server
     * see EOF and clean the transport up by itself. */
    adb_shutdown(r->sfd);
    adb_close(r->sfd);
    adb_close(r->peer);
    SSL_free(r->ssl);
    free(r->serial);
    free(r);
    return NULL;
}

/*
 * Try the Android 11+ wireless-debugging TLS upgrade on an already connected
 * socket.  Returns 0 when a transport was registered (a relay thread keeps
 * the connection alive) or -1 when the peer is not a TLS debugging listener.
 * On success the caller must NOT close fd; on failure fd stays owned by the
 * caller.
 */
int adb_tls_connect_device(int fd, const char *serial, int port)
{
    EVP_PKEY *pkey;
    X509 *cert;
    SSL_CTX *ctx;
    SSL *ssl;
    amessage msg;
    int sp[2];
    adb_thread_t th;
    struct adb_tls_relay *r;
    const char *errstr = NULL;

    /* 1. introduce ourselves; the TLS listener answers with STLS */
    if (adb_tls_write_msg(fd, TLS_A_CNXN, TLS_A_VERSION, TLS_MAX_PAYLOAD,
                          NULL, 0)) {
        errstr = "write CNXN";
        goto out_err;
    }
    memset(&msg, 0, sizeof(msg));
    if (readx(fd, &msg, sizeof(msg))) {
        errstr = "read greeting";
        goto out_err;
    }
    if (msg.command != TLS_A_STLS) {
        errstr = "not a TLS listener";
        goto out_err;
    }

    /* 2. acknowledge, then run the TLS client handshake with our adb key */
    if (adb_tls_write_msg(fd, TLS_A_STLS, TLS_STLS_VERSION, 0, NULL, 0)) {
        errstr = "write STLS ack";
        goto out_err;
    }
    pkey = adb_tls_load_key();
    if (!pkey) {
        errstr = "cannot load adb private key";
        goto out_err;
    }
    cert = adb_tls_make_cert(pkey);
    if (!cert) {
        errstr = "cannot build client certificate";
        EVP_PKEY_free(pkey);
        goto out_err;
    }
    ctx = SSL_CTX_new(TLS_method());
    if (!ctx) {
        errstr = "SSL_CTX_new";
        X509_free(cert);
        EVP_PKEY_free(pkey);
        goto out_err;
    }
    SSL_CTX_set_min_proto_version(ctx, TLS1_3_VERSION);
    SSL_CTX_set_max_proto_version(ctx, TLS1_3_VERSION);
    SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, NULL);
    if (SSL_CTX_use_certificate(ctx, cert) != 1 ||
        SSL_CTX_use_PrivateKey(ctx, pkey) != 1 ||
        SSL_CTX_check_private_key(ctx) != 1) {
        errstr = "cert/key setup";
        SSL_CTX_free(ctx);
        X509_free(cert);
        EVP_PKEY_free(pkey);
        goto out_err;
    }
    X509_free(cert);
    EVP_PKEY_free(pkey);

    ssl = SSL_new(ctx);
    SSL_CTX_free(ctx);      /* SSL keeps its own reference to the context */
    if (!ssl) {
        errstr = "SSL_new";
        goto out_err;
    }
    SSL_set_fd(ssl, fd);
    if (SSL_connect(ssl) != 1) {
        errstr = "TLS handshake";
        SSL_free(ssl);
        goto out_err;
    }
    adb_tls_dbg("relay TLS handshake done");

    /* 3. hand the plaintext side to the regular adb transport layer */
    if (adb_socketpair(sp) != 0) {
        errstr = "socketpair";
        SSL_free(ssl);
        goto out_err;
    }
    if (register_socket_transport(sp[0], serial, port, 0) < 0) {
        errstr = "transport register";
        adb_close(sp[0]);
        adb_close(sp[1]);
        SSL_free(ssl);
        goto out_err;
    }

    /* 4. relay TLS <-> plaintext until either side disappears */
    r = calloc(1, sizeof(*r));
    if (!r) {
        errstr = "oom";
        adb_close(sp[1]);
        SSL_free(ssl);
        goto out_err;
    }
    r->ssl = ssl;
    r->peer = sp[1];
    r->sfd = fd;
    r->serial = strdup(serial);
    if (adb_thread_create(&th, adb_tls_relay_thread, r) != 0) {
        errstr = "thread_create";
        free(r->serial);
        free(r);
        adb_close(sp[1]);
        SSL_free(ssl);
        goto out_err;
    }
    return 0;

out_err:
    fprintf(stderr, "adb_tls_connect_device: %s\n", errstr ? errstr : "error");
    return -1;
}
