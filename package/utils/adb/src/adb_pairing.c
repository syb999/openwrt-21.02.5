/*
 * adb_pairing.c - adb pair (Android 11+ wireless-debugging pairing protocol)
 * client implementation for the legacy adb 5.0.2 tree.
 *
 * Implements the host side of the AOSP pairing protocol as shipped in
 * platform-tools 30.0.5 (pairing_auth / pairing_connection / tls), but in
 * plain C on top of OpenSSL 1.1.1 (TLS 1.3 + SPAKE2 extracted from
 * BoringSSL + AES-128-GCM via EVP).
 *
 * Flow:
 *   TCP connect -> TLS1.3 (self-signed X509 made from the adb RSA key,
 *   both peers verify blindly) -> export_keying_material("adb-label", 64)
 *   -> pswd = code || exported -> SPAKE2 msg exchange (32B each)
 *   -> AES-128-GCM key = HKDF-SHA256(spake2 key, info
 *      "adb pairing_auth aes-128-gcm key") -> encrypted PeerInfo exchange:
 *      host sends ADB_RSA_PUB_KEY (adbkey.pub contents), device replies
 *      ADB_DEVICE_GUID. On success the device has stored our public key and
 *      the legacy `adb connect` will authenticate against it.
 *
 * License: Apache-2.0 (port of AOSP code); SPAKE2 parts MIT (BoringSSL).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netdb.h>
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
#include <openssl/hmac.h>

#include "spake_port.h"
#include "spake_internal.h"

/* ------------------------- constants ---------------------------------- */
#define ADB_PAIR_VERSION      1
#define ADB_PAIR_TYPE_SPAKE2  0
#define ADB_PAIR_TYPE_PEERINFO 1
#define ADB_PAIR_MAX_PAYLOAD  (8192 * 2)

#define PEERINFO_ADB_RSA_PUB_KEY 0
#define PEERINFO_ADB_DEVICE_GUID 1

#define ADB_KEY_FILE     "adbkey"
#define ADB_KEY_PUB_FILE "adbkey.pub"
#define ANDROID_PATH     ".android"

#define EXPORTED_KEY_SIZE 64
#define AES_GCM_KEY_SIZE  16
#define AES_GCM_TAG_SIZE  16
#define AES_GCM_NONCE_SIZE 12
#define CERT_LIFETIME_SEC (10 * 365 * 24 * 60 * 60)

#define HKDF_INFO "adb pairing_auth aes-128-gcm key"

#pragma pack(push, 1)
struct pairing_header {
    uint8_t version;
    uint8_t type;
    uint32_t payload; /* network order on the wire */
};
struct peer_info {
    uint8_t type;
    uint8_t data[8191];
};
#pragma pack(pop)

/* ------------------------- logging ------------------------------------ */
#define PERR(...) fprintf(stderr, "adb pair: " __VA_ARGS__)

/* ------------------------- tiny helpers -------------------------------- */
static void *xmalloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) {
        PERR("out of memory\n");
        exit(1);
    }
    return p;
}

static int write_all(int fd, const void *buf, size_t len) {
    const uint8_t *p = buf;
    while (len > 0) {
        ssize_t r = write(fd, p, len);
        if (r <= 0) {
            if (r < 0 && errno == EINTR) continue;
            return -1;
        }
        p += r;
        len -= (size_t)r;
    }
    return 0;
}

static int read_all(int fd, void *buf, size_t len) {
    uint8_t *p = buf;
    while (len > 0) {
        ssize_t r = read(fd, p, len);
        if (r <= 0) {
            if (r < 0 && errno == EINTR) continue;
            return -1;
        }
        p += r;
        len -= (size_t)r;
    }
    return 0;
}

/* ------------------------- key paths ---------------------------------- */
static int adb_key_path(char *out, size_t outlen, const char *name) {
    const char *home = getenv("HOME");
    const char *android_home = getenv("ANDROID_SDK_HOME");
    if (android_home && *android_home) {
        home = android_home;
    }
    if (!home || !*home) {
        return -1;
    }
    if (snprintf(out, outlen, "%s/%s/%s", home, ANDROID_PATH, name) >=
        (int)outlen) {
        return -1;
    }
    return 0;
}

/* load the adb private key (RSA). Returns EVP_PKEY* or NULL. */
static EVP_PKEY *adb_load_private_key(void) {
    char path[PATH_MAX];
    FILE *f;
    EVP_PKEY *pkey = NULL;

    if (adb_key_path(path, sizeof(path), ADB_KEY_FILE) < 0) {
        return NULL;
    }
    f = fopen(path, "r");
    if (!f) {
        return NULL;
    }
    pkey = PEM_read_PrivateKey(f, NULL, NULL, NULL);
    fclose(f);
    return pkey;
}

/* load adbkey.pub contents into out (nul terminated), returns length or -1 */
static int adb_load_public_key(char *out, size_t outlen) {
    char path[PATH_MAX];
    FILE *f;
    size_t n;

    if (adb_key_path(path, sizeof(path), ADB_KEY_PUB_FILE) < 0) {
        return -1;
    }
    f = fopen(path, "r");
    if (!f) {
        return -1;
    }
    n = fread(out, 1, outlen - 1, f);
    fclose(f);
    if (n == 0) {
        return -1;
    }
    out[n] = '\0';
    /* trim trailing whitespace / newline */
    while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == '\r' ||
                     out[n - 1] == ' ')) {
        out[--n] = '\0';
    }
    return (int)n;
}

/* ------------------------- X509 certificate --------------------------- */
/* Build a self-signed X509 from the adb RSA key (mirrors AOSP
 * x509_generator.cpp: CA:TRUE, keyCertSign/cRLSign/digitalSignature). */
static int x509_add_ext(X509 *cert, int nid, const char *value) {
    X509V3_CTX ctx;
    X509_EXTENSION *ex;

    X509V3_set_ctx_nodb(&ctx);
    X509V3_set_ctx(&ctx, cert, cert, NULL, NULL, 0);
    ex = X509V3_EXT_nconf_nid(NULL, &ctx, nid, (char *)value);
    if (!ex) {
        return 0;
    }
    X509_add_ext(cert, ex, -1);
    X509_EXTENSION_free(ex);
    return 1;
}

static X509 *adb_generate_cert(EVP_PKEY *pkey) {
    X509 *x509 = NULL;
    EVP_PKEY *pub = NULL;
    X509_NAME *name = NULL;
    ASN1_INTEGER *serial = NULL;
    BIGNUM *bn = NULL;
    uint8_t rand_serial[16];

    x509 = X509_new();
    if (!x509) goto err;
    X509_set_version(x509, 2); /* v3 */

    if (RAND_bytes(rand_serial, sizeof(rand_serial)) != 1) goto err;
    bn = BN_bin2bn(rand_serial, sizeof(rand_serial), NULL);
    if (!bn) goto err;
    serial = BN_to_ASN1_INTEGER(bn, NULL);
    if (!serial) goto err;
    X509_set_serialNumber(x509, serial);

    if (!X509_gmtime_adj(X509_getm_notBefore(x509), 0)) goto err;
    if (!X509_gmtime_adj(X509_getm_notAfter(x509), CERT_LIFETIME_SEC)) goto err;

    pub = EVP_PKEY_new();
    if (!pub) goto err;
    if (EVP_PKEY_set1_RSA(pub, EVP_PKEY_get0_RSA(pkey)) != 1) goto err;
    if (X509_set_pubkey(x509, pub) != 1) goto err;

    name = X509_NAME_new();
    if (!name) goto err;
    if (X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                                   (const unsigned char *)"adb", -1, -1, 0) != 1)
        goto err;
    if (X509_set_subject_name(x509, name) != 1) goto err;
    if (X509_set_issuer_name(x509, name) != 1) goto err;

    if (!x509_add_ext(x509, NID_basic_constraints, "critical,CA:TRUE"))
        goto err;
    if (!x509_add_ext(x509, NID_key_usage,
                      "critical,keyCertSign,cRLSign,digitalSignature"))
        goto err;

    if (X509_sign(x509, pkey, EVP_sha256()) <= 0) goto err;

    EVP_PKEY_free(pub);
    X509_NAME_free(name);
    ASN1_INTEGER_free(serial);
    BN_free(bn);
    return x509;

err:
    if (x509) X509_free(x509);
    if (pub) EVP_PKEY_free(pub);
    if (name) X509_NAME_free(name);
    if (serial) ASN1_INTEGER_free(serial);
    if (bn) BN_free(bn);
    return NULL;
}

static char *x509_to_pem(X509 *x509) {
    BIO *bio = BIO_new(BIO_s_mem());
    char *data = NULL;
    long len;

    if (!bio) return NULL;
    if (PEM_write_bio_X509(bio, x509) != 1) {
        BIO_free(bio);
        return NULL;
    }
    len = BIO_get_mem_data(bio, &data);
    if (len <= 0) {
        BIO_free(bio);
        return NULL;
    }
    {
        char *out = xmalloc((size_t)len + 1);
        memcpy(out, data, (size_t)len);
        out[len] = '\0';
        BIO_free(bio);
        return out;
    }
}

/* ------------------------- HKDF-SHA256 -------------------------------- */
/* RFC5869, used with 32-byte zero salt when salt==NULL (as BoringSSL HKDF) */
static int hkdf_sha256(const uint8_t *ikm, size_t ikm_len, const uint8_t *salt,
                       size_t salt_len, const uint8_t *info, size_t info_len,
                       uint8_t *out, size_t out_len) {
    uint8_t prk[EVP_MAX_MD_SIZE];
    uint8_t zeros[EVP_MAX_MD_SIZE];
    unsigned int prk_len = 0;
    uint8_t t[EVP_MAX_MD_SIZE];
    unsigned int t_len = 0;
    size_t done = 0;
    uint8_t counter = 1;
    const EVP_MD *md = EVP_sha256();
    unsigned int md_len = (unsigned int)EVP_MD_size(md);

    if (salt_len == 0) {
        memset(zeros, 0, md_len);
        salt = zeros;
        salt_len = md_len;
    }
    if (HMAC(md, salt, (int)salt_len, ikm, ikm_len, prk, &prk_len) == NULL)
        return 0;

    /* T(0) = empty */
    while (done < out_len) {
        HMAC_CTX *ctx = HMAC_CTX_new();
        size_t info_off;
        if (!ctx) return 0;
        if (HMAC_Init_ex(ctx, prk, (int)prk_len, md, NULL) != 1) {
            HMAC_CTX_free(ctx);
            return 0;
        }
        if (done > 0) { /* T(i-1) */
            if (HMAC_Update(ctx, t, t_len) != 1) {
                HMAC_CTX_free(ctx);
                return 0;
            }
        }
        for (info_off = 0; info_off < info_len; info_off++) {
            if (HMAC_Update(ctx, info + info_off, 1) != 1) {
                HMAC_CTX_free(ctx);
                return 0;
            }
        }
        if (HMAC_Update(ctx, &counter, 1) != 1) {
            HMAC_CTX_free(ctx);
            return 0;
        }
        if (HMAC_Final(ctx, t, &t_len) != 1) {
            HMAC_CTX_free(ctx);
            return 0;
        }
        HMAC_CTX_free(ctx);

        if (t_len > out_len - done) t_len = (unsigned int)(out_len - done);
        memcpy(out + done, t, t_len);
        done += t_len;
        counter++;
    }
    return 1;
}

/* ------------------------- AES-128-GCM -------------------------------- */
struct aes_gcm_ctx {
    EVP_CIPHER_CTX *e;
    EVP_CIPHER_CTX *d;
    uint64_t enc_seq;
    uint64_t dec_seq;
};

static void nonce_for_seq(uint64_t seq, uint8_t nonce[AES_GCM_NONCE_SIZE]) {
    memset(nonce, 0, AES_GCM_NONCE_SIZE);
    memcpy(nonce, &seq, sizeof(seq)); /* little-endian on the wire */
}

static struct aes_gcm_ctx *aes_gcm_new(const uint8_t *key_material,
                                       size_t key_material_len) {
    struct aes_gcm_ctx *c;
    uint8_t key[AES_GCM_KEY_SIZE];

    if (!hkdf_sha256(key_material, key_material_len, NULL, 0,
                     (const uint8_t *)HKDF_INFO, sizeof(HKDF_INFO) - 1, key,
                     sizeof(key))) {
        return NULL;
    }
    c = xmalloc(sizeof(*c));
    memset(c, 0, sizeof(*c));
    c->e = EVP_CIPHER_CTX_new();
    c->d = EVP_CIPHER_CTX_new();
    if (!c->e || !c->d) {
        return NULL;
    }
    EVP_EncryptInit_ex(c->e, EVP_aes_128_gcm(), NULL, NULL, NULL);
    EVP_CIPHER_CTX_ctrl(c->e, EVP_CTRL_GCM_SET_IVLEN, AES_GCM_NONCE_SIZE, NULL);
    EVP_EncryptInit_ex(c->e, NULL, NULL, key, NULL);
    EVP_DecryptInit_ex(c->d, EVP_aes_128_gcm(), NULL, NULL, NULL);
    EVP_CIPHER_CTX_ctrl(c->d, EVP_CTRL_GCM_SET_IVLEN, AES_GCM_NONCE_SIZE, NULL);
    EVP_DecryptInit_ex(c->d, NULL, NULL, key, NULL);
    return c;
}

static int aes_gcm_encrypt(struct aes_gcm_ctx *c, const uint8_t *in,
                           size_t in_len, uint8_t *out, size_t *out_len) {
    uint8_t nonce[AES_GCM_NONCE_SIZE];
    uint8_t tag[AES_GCM_TAG_SIZE];
    int len = 0, total = 0;

    nonce_for_seq(c->enc_seq++, nonce);
    if (EVP_EncryptInit_ex(c->e, NULL, NULL, NULL, nonce) != 1) return -1;
    if (EVP_EncryptUpdate(c->e, out, &len, in, (int)in_len) != 1) return -1;
    total = len;
    if (EVP_EncryptFinal_ex(c->e, out + total, &len) != 1) return -1;
    total += len;
    if (EVP_CIPHER_CTX_ctrl(c->e, EVP_CTRL_GCM_GET_TAG, AES_GCM_TAG_SIZE, tag) != 1)
        return -1;
    memcpy(out + total, tag, AES_GCM_TAG_SIZE);
    *out_len = (size_t)total + AES_GCM_TAG_SIZE;
    return 0;
}

static int aes_gcm_decrypt(struct aes_gcm_ctx *c, const uint8_t *in,
                           size_t in_len, uint8_t *out, size_t *out_len) {
    uint8_t nonce[AES_GCM_NONCE_SIZE];
    int len = 0, total = 0;

    if (in_len < AES_GCM_TAG_SIZE) return -1;
    nonce_for_seq(c->dec_seq++, nonce);
    if (EVP_DecryptInit_ex(c->d, NULL, NULL, NULL, nonce) != 1) return -1;
    if (EVP_DecryptUpdate(c->d, out, &len, in, (int)(in_len - AES_GCM_TAG_SIZE)) != 1)
        return -1;
    total = len;
    if (EVP_CIPHER_CTX_ctrl(c->d, EVP_CTRL_GCM_SET_TAG, AES_GCM_TAG_SIZE,
                            (void *)(in + in_len - AES_GCM_TAG_SIZE)) != 1)
        return -1;
    if (EVP_DecryptFinal_ex(c->d, out + total, &len) != 1) return -1;
    total += len;
    *out_len = (size_t)total;
    return 0;
}

/* ------------------------- TLS transport ------------------------------ */
struct tls_io {
    SSL *ssl;
    SSL_CTX *ctx;
};

static int tls_verify_ok(X509_STORE_CTX *ctx, void *opaque) {
    (void)ctx;
    (void)opaque;
    return 1; /* accept any peer certificate, as AOSP does during pairing */
}

/* TLS read/write through the socket (blocking, loop until done) */
static int tls_write_all(SSL *ssl, const void *buf, size_t len) {
    const uint8_t *p = buf;
    while (len > 0) {
        int r = SSL_write(ssl, p, (int)len);
        if (r <= 0) {
            int e = SSL_get_error(ssl, r);
            if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) continue;
            return -1;
        }
        p += r;
        len -= (size_t)r;
    }
    return 0;
}

static int tls_read_all(SSL *ssl, void *buf, size_t len) {
    uint8_t *p = buf;
    while (len > 0) {
        int r = SSL_read(ssl, p, (int)len);
        if (r <= 0) {
            int e = SSL_get_error(ssl, r);
            if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) continue;
            return -1;
        }
        p += r;
        len -= (size_t)r;
    }
    return 0;
}

static int tls_setup(struct tls_io *t, int fd, const char *cert_pem,
                     const char *key_pem) {
    BIO *cbio = NULL, *kbio = NULL;
    X509 *cert = NULL;
    EVP_PKEY *pkey = NULL;

    memset(t, 0, sizeof(*t));
    t->ctx = SSL_CTX_new(TLS_method());
    if (!t->ctx) goto err;
    SSL_CTX_set_min_proto_version(t->ctx, TLS1_3_VERSION);
    SSL_CTX_set_max_proto_version(t->ctx, TLS1_3_VERSION);

    cbio = BIO_new_mem_buf(cert_pem, -1);
    if (!cbio) goto err;
    cert = PEM_read_bio_X509(cbio, NULL, NULL, NULL);
    if (!cert) goto err;
    kbio = BIO_new_mem_buf(key_pem, -1);
    if (!kbio) goto err;
    pkey = PEM_read_bio_PrivateKey(kbio, NULL, NULL, NULL);
    if (!pkey) goto err;

    if (SSL_CTX_use_certificate(t->ctx, cert) != 1) goto err;
    if (SSL_CTX_use_PrivateKey(t->ctx, pkey) != 1) goto err;

    /* Pairing trusts the peer cert blindly (AOSP sets a verify callback
     * that always returns 1); we still ask for / send certificates. */
    SSL_CTX_set_verify(t->ctx, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT,
                       NULL);
    SSL_CTX_set_cert_verify_callback(t->ctx, tls_verify_ok, NULL);

    t->ssl = SSL_new(t->ctx);
    if (!t->ssl) goto err;
    SSL_set_fd(t->ssl, fd);
    if (SSL_connect(t->ssl) != 1) goto err;

    X509_free(cert);
    EVP_PKEY_free(pkey);
    BIO_free(cbio);
    BIO_free(kbio);
    return 0;
err:
    if (cert) X509_free(cert);
    if (pkey) EVP_PKEY_free(pkey);
    if (cbio) BIO_free(cbio);
    if (kbio) BIO_free(kbio);
    if (t->ssl) SSL_free(t->ssl);
    if (t->ctx) SSL_CTX_free(t->ctx);
    memset(t, 0, sizeof(*t));
    return -1;
}

/* ------------------------- TCP connect -------------------------------- */
static int tcp_connect(const char *host, int port) {
    struct addrinfo hints, *res = NULL, *rp;
    char portstr[16];
    int fd = -1;
    int off = 1;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    snprintf(portstr, sizeof(portstr), "%d", port);
    if (getaddrinfo(host, portstr, &hints, &res) != 0) {
        return -1;
    }
    for (rp = res; rp; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, rp->ai_addr, rp->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd >= 0) {
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &off, sizeof(off));
    }
    return fd;
}

/* ------------------------- pairing protocol --------------------------- */
struct pair_header_wire {
    uint8_t version;
    uint8_t type;
    uint32_t payload; /* network order */
} __attribute__((packed));

static int send_packet(SSL *ssl, uint8_t type, const uint8_t *payload,
                       size_t payload_len) {
    struct pair_header_wire h;
    h.version = ADB_PAIR_VERSION;
    h.type = type;
    h.payload = htonl((uint32_t)payload_len);
    if (tls_write_all(ssl, &h, sizeof(h)) < 0) return -1;
    if (payload_len > 0 && tls_write_all(ssl, payload, payload_len) < 0)
        return -1;
    return 0;
}

static int recv_packet(SSL *ssl, uint8_t *type, uint8_t **payload,
                       size_t *payload_len) {
    struct pair_header_wire h;
    uint8_t *buf;

    if (tls_read_all(ssl, &h, sizeof(h)) < 0) return -1;
    if (h.version != ADB_PAIR_VERSION) return -1;
    *payload_len = ntohl(h.payload);
    if (*payload_len == 0 || *payload_len > ADB_PAIR_MAX_PAYLOAD) return -1;
    buf = xmalloc(*payload_len);
    if (tls_read_all(ssl, buf, *payload_len) < 0) {
        free(buf);
        return -1;
    }
    *type = h.type;
    *payload = buf;
    return 0;
}

/*
 * adb_pairing_run - full pairing client.
 * host/port: device pairing endpoint. code: 6 digit pairing code.
 * Returns 0 on success (prints guid), -1 on failure.
 */
int adb_pairing_run(const char *host, int port, const char *code) {
    EVP_PKEY *pkey = NULL;
    X509 *x509 = NULL;
    char *cert_pem = NULL;
    char *key_pem = NULL;
    char pubkey[8192];
    int pubkey_len;
    BIO *kbio = NULL;
    int fd = -1;
    struct tls_io tls = {0};
    uint8_t exported[EXPORTED_KEY_SIZE];
    uint8_t *pswd = NULL;
    size_t pswd_len;
    SPAKE2_CTX *spake = NULL;
    uint8_t my_msg[SPAKE2_MAX_MSG_SIZE];
    size_t my_msg_len = 0;
    uint8_t their_msg[SPAKE2_MAX_MSG_SIZE];
    size_t their_msg_len = 0;
    uint8_t key_material[SPAKE2_MAX_KEY_SIZE];
    size_t key_material_len = 0;
    struct aes_gcm_ctx *aes = NULL;
    struct peer_info my_info, their_info;
    uint8_t *enc = NULL;
    size_t enc_len = 0;
    uint8_t *dec = NULL;
    size_t dec_len = 0;
    uint8_t ptype;
    uint8_t *ppayload = NULL;
    size_t ppayload_len = 0;
    int ret = -1;
    size_t code_len;

    static const uint8_t kClientName[] = "adb pair client";
    static const uint8_t kServerName[] = "adb pair server";

    /* 1. keys */
    pkey = adb_load_private_key();
    if (!pkey) {
        PERR("cannot read adb private key (~/.android/adbkey); run 'adb' "
             "once with a device over USB to generate it\n");
        goto out;
    }
    pubkey_len = adb_load_public_key(pubkey, sizeof(pubkey));
    if (pubkey_len <= 0) {
        PERR("cannot read ~/.android/adbkey.pub\n");
        goto out;
    }

    /* 2. self-signed cert for the TLS transport */
    x509 = adb_generate_cert(pkey);
    if (!x509) {
        PERR("failed to generate X509 certificate\n");
        goto out;
    }
    cert_pem = x509_to_pem(x509);
    if (!cert_pem) {
        PERR("failed to serialize certificate\n");
        goto out;
    }
    kbio = BIO_new(BIO_s_mem());
    if (!kbio) goto out;
    if (PEM_write_bio_PrivateKey(kbio, pkey, NULL, NULL, 0, NULL, NULL) != 1) {
        PERR("failed to serialize private key\n");
        goto out;
    }
    {
        char *kp;
        long klen = BIO_get_mem_data(kbio, &kp);
        if (klen <= 0) goto out;
        key_pem = xmalloc((size_t)klen + 1);
        memcpy(key_pem, kp, (size_t)klen);
        key_pem[klen] = '\0';
    }

    /* 3. TCP + TLS */
    fd = tcp_connect(host, port);
    if (fd < 0) {
        PERR("failed to connect to %s:%d\n", host, port);
        goto out;
    }
    if (tls_setup(&tls, fd, cert_pem, key_pem) < 0) {
        PERR("TLS handshake failed (%s)\n",
             ERR_reason_error_string(ERR_peek_last_error()));
        goto out;
    }

    /* 4. exporter */
    if (SSL_export_keying_material(tls.ssl, exported, sizeof(exported),
                                   "adb-label", sizeof("adb-label"), NULL, 0,
                                   0) != 1) {
        PERR("failed to export TLS keying material\n");
        goto out;
    }

    /* 5. pswd = code || exported */
    code_len = strlen(code);
    pswd_len = code_len + sizeof(exported);
    pswd = xmalloc(pswd_len);
    memcpy(pswd, code, code_len);
    memcpy(pswd + code_len, exported, sizeof(exported));

    /* 6. SPAKE2 exchange */
    spake = SPAKE2_CTX_new(spake2_role_alice, kClientName,
                           sizeof(kClientName), kServerName,
                           sizeof(kServerName));
    if (!spake) {
        PERR("failed to create SPAKE2 context\n");
        goto out;
    }
    if (SPAKE2_generate_msg(spake, my_msg, &my_msg_len, sizeof(my_msg), pswd,
                            pswd_len) != 1) {
        PERR("failed to generate SPAKE2 message\n");
        goto out;
    }
    if (send_packet(tls.ssl, ADB_PAIR_TYPE_SPAKE2, my_msg, my_msg_len) < 0) {
        PERR("failed to send SPAKE2 message (ssl_err=%d)\n",
             SSL_get_error(tls.ssl, -1));
        goto out;
    }
    if (recv_packet(tls.ssl, &ptype, &ppayload, &ppayload_len) < 0) {
        PERR("failed to receive SPAKE2 reply\n");
        goto out;
    }
    if (ptype != ADB_PAIR_TYPE_SPAKE2 || ppayload_len != SPAKE2_MAX_MSG_SIZE) {
        PERR("unexpected SPAKE2 reply (type=%u len=%zu)\n", ptype,
             ppayload_len);
        goto out;
    }
    memcpy(their_msg, ppayload, ppayload_len);
    their_msg_len = ppayload_len;
    free(ppayload);
    ppayload = NULL;

    if (SPAKE2_process_msg(spake, key_material, &key_material_len,
                           sizeof(key_material), their_msg, their_msg_len) != 1) {
        PERR("SPAKE2 key derivation failed (wrong pairing code?)\n");
        goto out;
    }

    /* 7. AES-128-GCM layer */
    aes = aes_gcm_new(key_material, key_material_len);
    if (!aes) {
        PERR("failed to initialise pairing cipher\n");
        goto out;
    }

    /* 8. exchange PeerInfo */
    memset(&my_info, 0, sizeof(my_info));
    my_info.type = PEERINFO_ADB_RSA_PUB_KEY;
    if ((size_t)pubkey_len >= sizeof(my_info.data)) {
        PERR("public key too large\n");
        goto out;
    }
    memcpy(my_info.data, pubkey, (size_t)pubkey_len);

    enc = xmalloc(sizeof(my_info) + AES_GCM_TAG_SIZE + 16);
    if (aes_gcm_encrypt(aes, (const uint8_t *)&my_info, sizeof(my_info), enc,
                        &enc_len) < 0) {
        PERR("failed to encrypt peer info\n");
        goto out;
    }
    if (send_packet(tls.ssl, ADB_PAIR_TYPE_PEERINFO, enc, enc_len) < 0) {
        PERR("failed to send peer info\n");
        goto out;
    }
    free(enc);
    enc = NULL;

    if (recv_packet(tls.ssl, &ptype, &ppayload, &ppayload_len) < 0) {
        PERR("failed to receive peer info\n");
        goto out;
    }
    if (ptype != ADB_PAIR_TYPE_PEERINFO) {
        PERR("unexpected peer info reply (type=%u)\n", ptype);
        goto out;
    }
    dec = xmalloc(ppayload_len);
    if (aes_gcm_decrypt(aes, ppayload, ppayload_len, dec, &dec_len) < 0 ||
        dec_len != sizeof(their_info)) {
        PERR("failed to decrypt peer info (wrong pairing code?)\n");
        goto out;
    }
    memcpy(&their_info, dec, sizeof(their_info));

    if (their_info.type != PEERINFO_ADB_DEVICE_GUID) {
        PERR("peer returned unexpected info type=%u\n", their_info.type);
        goto out;
    }
    {
        char guid[8192];
        size_t glen = strlen((const char *)their_info.data);
        if (glen >= sizeof(guid)) glen = sizeof(guid) - 1;
        memcpy(guid, their_info.data, glen);
        guid[glen] = '\0';
        printf("Successfully paired to %s:%d [guid=%s]\n", host, port, guid);
    }
    ret = 0;

out:
    if (ppayload) free(ppayload);
    if (dec) free(dec);
    if (enc) free(enc);
    if (aes) {
        if (aes->e) EVP_CIPHER_CTX_free(aes->e);
        if (aes->d) EVP_CIPHER_CTX_free(aes->d);
        free(aes);
    }
    if (spake) SPAKE2_CTX_free(spake);
    if (pswd) free(pswd);
    if (tls.ssl) SSL_free(tls.ssl);
    if (tls.ctx) SSL_CTX_free(tls.ctx);
    if (fd >= 0) close(fd);
    if (kbio) BIO_free(kbio);
    if (cert_pem) free(cert_pem);
    if (key_pem) free(key_pem);
    if (x509) X509_free(x509);
    if (pkey) EVP_PKEY_free(pkey);
    return ret;
}
