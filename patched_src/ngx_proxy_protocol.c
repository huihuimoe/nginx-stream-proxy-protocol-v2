
/*
 * Copyright (C) Roman Arutyunyan
 * Copyright (C) Nginx, Inc.
 */


#include <ngx_config.h>
#include <ngx_core.h>


#define NGX_PROXY_PROTOCOL_AF_INET          1
#define NGX_PROXY_PROTOCOL_AF_INET6         2


#define NGX_PROXY_PROTOCOL_V2_SIG              "\x0D\x0A\x0D\x0A\x00\x0D\x0A\x51\x55\x49\x54\x0A"
#define NGX_PROXY_PROTOCOL_V2_SIG_LEN          12
#define NGX_PROXY_PROTOCOL_V2_HDR_LEN          16
#define NGX_PROXY_PROTOCOL_V2_HDR_LEN_INET \
                (NGX_PROXY_PROTOCOL_V2_HDR_LEN + (4 + 4 + 2 + 2))
#define NGX_PROXY_PROTOCOL_V2_HDR_LEN_INET6 \
                (NGX_PROXY_PROTOCOL_V2_HDR_LEN + (16 + 16 + 2 + 2))

#define NGX_PROXY_PROTOCOL_V2_CMD_PROXY        (0x20 | 0x01)

#define NGX_PROXY_PROTOCOL_V2_TRANS_STREAM     0x01

#define NGX_PROXY_PROTOCOL_V2_FAM_UNSPEC       0x00
#define NGX_PROXY_PROTOCOL_V2_FAM_INET         0x10
#define NGX_PROXY_PROTOCOL_V2_FAM_INET6        0x20

#define NGX_PROXY_PROTOCOL_V2_TYPE_ALPN             0x01
#define NGX_PROXY_PROTOCOL_V2_TYPE_AUTHORITY        0x02 # Not implemented
#define NGX_PROXY_PROTOCOL_V2_TYPE_CRC32C           0x03 # Not implemented
#define NGX_PROXY_PROTOCOL_V2_TYPE_NOOP             0x04 # Not implemented
#define NGX_PROXY_PROTOCOL_V2_TYPE_UNIQUE_ID        0x05 # Not implemented
#define NGX_PROXY_PROTOCOL_V2_TYPE_SSL              0x20
#define NGX_PROXY_PROTOCOL_V2_SUBTYPE_SSL_VERSION   0x21
#define NGX_PROXY_PROTOCOL_V2_SUBTYPE_SSL_CN        0x22
#define NGX_PROXY_PROTOCOL_V2_SUBTYPE_SSL_CIPHER    0x23
#define NGX_PROXY_PROTOCOL_V2_SUBTYPE_SSL_SIG_ALG   0x24
#define NGX_PROXY_PROTOCOL_V2_SUBTYPE_SSL_KEY_ALG   0x25
#define NGX_PROXY_PROTOCOL_V2_TYPE_NETNS            0x30 # Not implemented

#define NGX_PROXY_PROTOCOL_V2_CLIENT_SSL            0x01
#define NGX_PROXY_PROTOCOL_V2_CLIENT_CERT_CONN      0x02
#define NGX_PROXY_PROTOCOL_V2_CLIENT_CERT_SESS      0x04


#define ngx_proxy_protocol_parse_uint16(p)                                    \
    ( ((uint16_t) (p)[0] << 8)                                                \
    + (           (p)[1]) )

#define ngx_proxy_protocol_parse_uint32(p)                                    \
    ( ((uint32_t) (p)[0] << 24)                                               \
    + (           (p)[1] << 16)                                               \
    + (           (p)[2] << 8)                                                \
    + (           (p)[3]) )


typedef struct {
    u_char                                  signature[12];
    u_char                                  version_command;
    u_char                                  family_transport;
    u_char                                  len[2];
} ngx_proxy_protocol_header_t;


typedef struct {
    u_char                                  src_addr[4];
    u_char                                  dst_addr[4];
    u_char                                  src_port[2];
    u_char                                  dst_port[2];
} ngx_proxy_protocol_inet_addrs_t;


typedef struct {
    u_char                                  src_addr[16];
    u_char                                  dst_addr[16];
    u_char                                  src_port[2];
    u_char                                  dst_port[2];
} ngx_proxy_protocol_inet6_addrs_t;


typedef union {
    struct {
        uint32_t          src_addr;
        uint32_t          dst_addr;
        uint16_t          src_port;
        uint16_t          dst_port;
    } ip4;
    struct {
        uint8_t           src_addr[16];
        uint8_t           dst_addr[16];
        uint16_t          src_port;
        uint16_t          dst_port;
    } ip6;
} ngx_proxy_protocol_addrs_t;


typedef struct {
    u_char                        signature[12];
    uint8_t                       version_command;
    uint8_t                       family_transport;
    uint16_t                      len;
    ngx_proxy_protocol_addrs_t    addr;
} ngx_proxy_protocol_v2_header_t;


struct ngx_tlv_s {
    uint8_t     type;
    uint8_t     length_hi;
    uint8_t     length_lo;
    uint8_t     value[0];
} __attribute__((packed));

typedef struct ngx_tlv_s ngx_tlv_t;


#if (NGX_STREAM_SSL)
struct ngx_tlv_ssl_s {
    ngx_tlv_t   tlv;
    uint8_t     client;
    uint32_t    verify;
    uint8_t     sub_tlv[];
} __attribute__((packed));

typedef struct ngx_tlv_ssl_s ngx_tlv_ssl_t;
#endif

typedef struct {
    u_char                                  type;
    u_char                                  len[2];
} ngx_proxy_protocol_tlv_t;


typedef struct {
    u_char                                  client;
    u_char                                  verify[4];
} ngx_proxy_protocol_tlv_ssl_t;


typedef struct {
    ngx_str_t                               name;
    ngx_uint_t                              type;
} ngx_proxy_protocol_tlv_entry_t;


static u_char *ngx_proxy_protocol_read_addr(ngx_connection_t *c, u_char *p,
    u_char *last, ngx_str_t *addr);
static u_char *ngx_proxy_protocol_read_port(u_char *p, u_char *last,
    in_port_t *port, u_char sep);
static u_char *ngx_proxy_protocol_v2_read(ngx_connection_t *c, u_char *buf,
    u_char *last);
static u_char *ngx_proxy_protocol_v2_write(ngx_connection_t *c, u_char *buf,
    u_char *last);
#if (NGX_HAVE_INET6)
static void ngx_v4tov6(struct in6_addr *sin6_addr, struct sockaddr *addr);
#endif
#if (NGX_STREAM_SSL)
static u_char *ngx_copy_tlv(u_char *pos, u_char *last, u_char type,
        u_char *value, uint16_t value_len);
#endif
static ngx_int_t ngx_proxy_protocol_lookup_tlv(ngx_connection_t *c,
    ngx_str_t *tlvs, ngx_uint_t type, ngx_str_t *value);


static ngx_proxy_protocol_tlv_entry_t  ngx_proxy_protocol_tlv_entries[] = {
    { ngx_string("alpn"),       0x01 },
    { ngx_string("authority"),  0x02 },
    { ngx_string("unique_id"),  0x05 },
    { ngx_string("ssl"),        0x20 },
    { ngx_string("netns"),      0x30 },
    { ngx_null_string,          0x00 }
};


static ngx_proxy_protocol_tlv_entry_t  ngx_proxy_protocol_tlv_ssl_entries[] = {
    { ngx_string("version"),    0x21 },
    { ngx_string("cn"),         0x22 },
    { ngx_string("cipher"),     0x23 },
    { ngx_string("sig_alg"),    0x24 },
    { ngx_string("key_alg"),    0x25 },
    { ngx_null_string,          0x00 }
};


u_char *
ngx_proxy_protocol_read(ngx_connection_t *c, u_char *buf, u_char *last)
{
    size_t                 len;
    u_char                *p;
    ngx_proxy_protocol_t  *pp;

    static const u_char signature[] = "\r\n\r\n\0\r\nQUIT\n";

    p = buf;
    len = last - buf;

    if (len >= sizeof(ngx_proxy_protocol_header_t)
        && ngx_memcmp(p, signature, sizeof(signature) - 1) == 0)
    {
        return ngx_proxy_protocol_v2_read(c, buf, last);
    }

    if (len < 8 || ngx_strncmp(p, "PROXY ", 6) != 0) {
        goto invalid;
    }

    p += 6;
    len -= 6;

    if (len >= 7 && ngx_strncmp(p, "UNKNOWN", 7) == 0) {
        ngx_log_debug0(NGX_LOG_DEBUG_CORE, c->log, 0,
                       "PROXY protocol unknown protocol");
        p += 7;
        goto skip;
    }

    if (len < 5 || ngx_strncmp(p, "TCP", 3) != 0
        || (p[3] != '4' && p[3] != '6') || p[4] != ' ')
    {
        goto invalid;
    }

    p += 5;

    pp = ngx_pcalloc(c->pool, sizeof(ngx_proxy_protocol_t));
    if (pp == NULL) {
        return NULL;
    }

    p = ngx_proxy_protocol_read_addr(c, p, last, &pp->src_addr);
    if (p == NULL) {
        goto invalid;
    }

    p = ngx_proxy_protocol_read_addr(c, p, last, &pp->dst_addr);
    if (p == NULL) {
        goto invalid;
    }

    p = ngx_proxy_protocol_read_port(p, last, &pp->src_port, ' ');
    if (p == NULL) {
        goto invalid;
    }

    p = ngx_proxy_protocol_read_port(p, last, &pp->dst_port, CR);
    if (p == NULL) {
        goto invalid;
    }

    if (p == last) {
        goto invalid;
    }

    if (*p++ != LF) {
        goto invalid;
    }

    ngx_log_debug4(NGX_LOG_DEBUG_CORE, c->log, 0,
                   "PROXY protocol src: %V %d, dst: %V %d",
                   &pp->src_addr, pp->src_port, &pp->dst_addr, pp->dst_port);

    c->proxy_protocol = pp;

    return p;

skip:

    for ( /* void */ ; p < last - 1; p++) {
        if (p[0] == CR && p[1] == LF) {
            return p + 2;
        }
    }

invalid:

    for (p = buf; p < last; p++) {
        if (*p == CR || *p == LF) {
            break;
        }
    }

    ngx_log_error(NGX_LOG_ERR, c->log, 0,
                  "broken header: \"%*s\"", (size_t) (p - buf), buf);

    return NULL;
}


static u_char *
ngx_proxy_protocol_read_addr(ngx_connection_t *c, u_char *p, u_char *last,
    ngx_str_t *addr)
{
    size_t  len;
    u_char  ch, *pos;

    pos = p;

    for ( ;; ) {
        if (p == last) {
            return NULL;
        }

        ch = *p++;

        if (ch == ' ') {
            break;
        }

        if (ch != ':' && ch != '.'
            && (ch < 'a' || ch > 'f')
            && (ch < 'A' || ch > 'F')
            && (ch < '0' || ch > '9'))
        {
            return NULL;
        }
    }

    len = p - pos - 1;

    addr->data = ngx_pnalloc(c->pool, len);
    if (addr->data == NULL) {
        return NULL;
    }

    ngx_memcpy(addr->data, pos, len);
    addr->len = len;

    return p;
}


static u_char *
ngx_proxy_protocol_read_port(u_char *p, u_char *last, in_port_t *port,
    u_char sep)
{
    size_t      len;
    u_char     *pos;
    ngx_int_t   n;

    pos = p;

    for ( ;; ) {
        if (p == last) {
            return NULL;
        }

        if (*p++ == sep) {
            break;
        }
    }

    len = p - pos - 1;

    n = ngx_atoi(pos, len);
    if (n < 0 || n > 65535) {
        return NULL;
    }

    *port = (in_port_t) n;

    return p;
}


u_char *
ngx_proxy_protocol_write(ngx_connection_t *c, u_char *buf, u_char *last,
        ngx_uint_t pp_version)
{
    ngx_uint_t  port, lport;

    if (last - buf < NGX_PROXY_PROTOCOL_V1_MAX_HEADER) {
        ngx_log_error(NGX_LOG_ALERT, c->log, 0,
                      "too small buffer for PROXY protocol");
        return NULL;
    }

    if (ngx_connection_local_sockaddr(c, NULL, 0) != NGX_OK) {
        return NULL;
    }

    if (pp_version == 2) {
        return ngx_proxy_protocol_v2_write(c, buf, last);
    }

    switch (c->sockaddr->sa_family) {

    case AF_INET:
        buf = ngx_cpymem(buf, "PROXY TCP4 ", sizeof("PROXY TCP4 ") - 1);
        break;

#if (NGX_HAVE_INET6)
    case AF_INET6:
        buf = ngx_cpymem(buf, "PROXY TCP6 ", sizeof("PROXY TCP6 ") - 1);
        break;
#endif

    default:
        return ngx_cpymem(buf, "PROXY UNKNOWN" CRLF,
                          sizeof("PROXY UNKNOWN" CRLF) - 1);
    }

    buf += ngx_sock_ntop(c->sockaddr, c->socklen, buf, last - buf, 0);

    *buf++ = ' ';

    buf += ngx_sock_ntop(c->local_sockaddr, c->local_socklen, buf, last - buf,
                         0);

    port = ngx_inet_get_port(c->sockaddr);
    lport = ngx_inet_get_port(c->local_sockaddr);

    return ngx_slprintf(buf, last, " %ui %ui" CRLF, port, lport);
}


static u_char *
ngx_proxy_protocol_v2_read(ngx_connection_t *c, u_char *buf, u_char *last)
{
    u_char                             *end;
    size_t                              len;
    socklen_t                           socklen;
    ngx_uint_t                          version, command, family, transport;
    ngx_sockaddr_t                      src_sockaddr, dst_sockaddr;
    ngx_proxy_protocol_t               *pp;
    ngx_proxy_protocol_header_t        *header;
    ngx_proxy_protocol_inet_addrs_t    *in;
#if (NGX_HAVE_INET6)
    ngx_proxy_protocol_inet6_addrs_t   *in6;
#endif

    header = (ngx_proxy_protocol_header_t *) buf;

    buf += sizeof(ngx_proxy_protocol_header_t);

    version = header->version_command >> 4;

    if (version != 2) {
        ngx_log_error(NGX_LOG_ERR, c->log, 0,
                      "unknown PROXY protocol version: %ui", version);
        return NULL;
    }

    len = ngx_proxy_protocol_parse_uint16(header->len);

    if ((size_t) (last - buf) < len) {
        ngx_log_error(NGX_LOG_ERR, c->log, 0, "header is too large");
        return NULL;
    }

    end = buf + len;

    command = header->version_command & 0x0f;

    /* only PROXY is supported */
    if (command != 1) {
        ngx_log_debug1(NGX_LOG_DEBUG_CORE, c->log, 0,
                       "PROXY protocol v2 unsupported command %ui", command);
        return end;
    }

    transport = header->family_transport & 0x0f;

    /* only STREAM is supported */
    if (transport != 1) {
        ngx_log_debug1(NGX_LOG_DEBUG_CORE, c->log, 0,
                       "PROXY protocol v2 unsupported transport %ui",
                       transport);
        return end;
    }

    pp = ngx_pcalloc(c->pool, sizeof(ngx_proxy_protocol_t));
    if (pp == NULL) {
        return NULL;
    }

    family = header->family_transport >> 4;

    switch (family) {

    case NGX_PROXY_PROTOCOL_AF_INET:

        if ((size_t) (end - buf) < sizeof(ngx_proxy_protocol_inet_addrs_t)) {
            return NULL;
        }

        in = (ngx_proxy_protocol_inet_addrs_t *) buf;

        src_sockaddr.sockaddr_in.sin_family = AF_INET;
        src_sockaddr.sockaddr_in.sin_port = 0;
        ngx_memcpy(&src_sockaddr.sockaddr_in.sin_addr, in->src_addr, 4);

        dst_sockaddr.sockaddr_in.sin_family = AF_INET;
        dst_sockaddr.sockaddr_in.sin_port = 0;
        ngx_memcpy(&dst_sockaddr.sockaddr_in.sin_addr, in->dst_addr, 4);

        pp->src_port = ngx_proxy_protocol_parse_uint16(in->src_port);
        pp->dst_port = ngx_proxy_protocol_parse_uint16(in->dst_port);

        socklen = sizeof(struct sockaddr_in);

        buf += sizeof(ngx_proxy_protocol_inet_addrs_t);

        break;

#if (NGX_HAVE_INET6)

    case NGX_PROXY_PROTOCOL_AF_INET6:

        if ((size_t) (end - buf) < sizeof(ngx_proxy_protocol_inet6_addrs_t)) {
            return NULL;
        }

        in6 = (ngx_proxy_protocol_inet6_addrs_t *) buf;

        src_sockaddr.sockaddr_in6.sin6_family = AF_INET6;
        src_sockaddr.sockaddr_in6.sin6_port = 0;
        ngx_memcpy(&src_sockaddr.sockaddr_in6.sin6_addr, in6->src_addr, 16);

        dst_sockaddr.sockaddr_in6.sin6_family = AF_INET6;
        dst_sockaddr.sockaddr_in6.sin6_port = 0;
        ngx_memcpy(&dst_sockaddr.sockaddr_in6.sin6_addr, in6->dst_addr, 16);

        pp->src_port = ngx_proxy_protocol_parse_uint16(in6->src_port);
        pp->dst_port = ngx_proxy_protocol_parse_uint16(in6->dst_port);

        socklen = sizeof(struct sockaddr_in6);

        buf += sizeof(ngx_proxy_protocol_inet6_addrs_t);

        break;

#endif

    default:
        ngx_log_debug1(NGX_LOG_DEBUG_CORE, c->log, 0,
                       "PROXY protocol v2 unsupported address family %ui",
                       family);
        return end;
    }

    pp->src_addr.data = ngx_pnalloc(c->pool, NGX_SOCKADDR_STRLEN);
    if (pp->src_addr.data == NULL) {
        return NULL;
    }

    pp->src_addr.len = ngx_sock_ntop(&src_sockaddr.sockaddr, socklen,
                                     pp->src_addr.data, NGX_SOCKADDR_STRLEN, 0);

    pp->dst_addr.data = ngx_pnalloc(c->pool, NGX_SOCKADDR_STRLEN);
    if (pp->dst_addr.data == NULL) {
        return NULL;
    }

    pp->dst_addr.len = ngx_sock_ntop(&dst_sockaddr.sockaddr, socklen,
                                     pp->dst_addr.data, NGX_SOCKADDR_STRLEN, 0);

    ngx_log_debug4(NGX_LOG_DEBUG_CORE, c->log, 0,
                   "PROXY protocol v2 src: %V %d, dst: %V %d",
                   &pp->src_addr, pp->src_port, &pp->dst_addr, pp->dst_port);

    if (buf < end) {
        pp->tlvs.data = ngx_pnalloc(c->pool, end - buf);
        if (pp->tlvs.data == NULL) {
            return NULL;
        }

        ngx_memcpy(pp->tlvs.data, buf, end - buf);
        pp->tlvs.len = end - buf;
    }

    c->proxy_protocol = pp;

    return end;
}


ngx_int_t
ngx_proxy_protocol_get_tlv(ngx_connection_t *c, ngx_str_t *name,
    ngx_str_t *value)
{
    u_char                          *p;
    size_t                           n;
    uint32_t                         verify;
    ngx_str_t                        ssl, *tlvs;
    ngx_int_t                        rc, type;
    ngx_proxy_protocol_tlv_ssl_t    *tlv_ssl;
    ngx_proxy_protocol_tlv_entry_t  *te;

    if (c->proxy_protocol == NULL) {
        return NGX_DECLINED;
    }

    ngx_log_debug1(NGX_LOG_DEBUG_CORE, c->log, 0,
                   "PROXY protocol v2 get tlv \"%V\"", name);

    te = ngx_proxy_protocol_tlv_entries;
    tlvs = &c->proxy_protocol->tlvs;

    p = name->data;
    n = name->len;

    if (n >= 4 && p[0] == 's' && p[1] == 's' && p[2] == 'l' && p[3] == '_') {

        rc = ngx_proxy_protocol_lookup_tlv(c, tlvs, 0x20, &ssl);
        if (rc != NGX_OK) {
            return rc;
        }

        if (ssl.len < sizeof(ngx_proxy_protocol_tlv_ssl_t)) {
            return NGX_ERROR;
        }

        p += 4;
        n -= 4;

        if (n == 6 && ngx_strncmp(p, "verify", 6) == 0) {

            tlv_ssl = (ngx_proxy_protocol_tlv_ssl_t *) ssl.data;
            verify = ngx_proxy_protocol_parse_uint32(tlv_ssl->verify);

            value->data = ngx_pnalloc(c->pool, NGX_INT32_LEN);
            if (value->data == NULL) {
                return NGX_ERROR;
            }

            value->len = ngx_sprintf(value->data, "%uD", verify)
                         - value->data;
            return NGX_OK;
        }

        ssl.data += sizeof(ngx_proxy_protocol_tlv_ssl_t);
        ssl.len -= sizeof(ngx_proxy_protocol_tlv_ssl_t);

        te = ngx_proxy_protocol_tlv_ssl_entries;
        tlvs = &ssl;
    }

    if (n >= 2 && p[0] == '0' && p[1] == 'x') {

        type = ngx_hextoi(p + 2, n - 2);
        if (type == NGX_ERROR) {
            ngx_log_error(NGX_LOG_ERR, c->log, 0,
                          "invalid PROXY protocol TLV \"%V\"", name);
            return NGX_ERROR;
        }

        return ngx_proxy_protocol_lookup_tlv(c, tlvs, type, value);
    }

    for ( /* void */ ; te->type; te++) {
        if (te->name.len == n && ngx_strncmp(te->name.data, p, n) == 0) {
            return ngx_proxy_protocol_lookup_tlv(c, tlvs, te->type, value);
        }
    }

    ngx_log_error(NGX_LOG_ERR, c->log, 0,
                  "unknown PROXY protocol TLV \"%V\"", name);

    return NGX_DECLINED;
}


static ngx_int_t
ngx_proxy_protocol_lookup_tlv(ngx_connection_t *c, ngx_str_t *tlvs,
    ngx_uint_t type, ngx_str_t *value)
{
    u_char                    *p;
    size_t                     n, len;
    ngx_proxy_protocol_tlv_t  *tlv;

    ngx_log_debug1(NGX_LOG_DEBUG_CORE, c->log, 0,
                   "PROXY protocol v2 lookup tlv:%02xi", type);

    p = tlvs->data;
    n = tlvs->len;

    while (n) {
        if (n < sizeof(ngx_proxy_protocol_tlv_t)) {
            ngx_log_error(NGX_LOG_ERR, c->log, 0, "broken PROXY protocol TLV");
            return NGX_ERROR;
        }

        tlv = (ngx_proxy_protocol_tlv_t *) p;
        len = ngx_proxy_protocol_parse_uint16(tlv->len);

        p += sizeof(ngx_proxy_protocol_tlv_t);
        n -= sizeof(ngx_proxy_protocol_tlv_t);

        if (n < len) {
            ngx_log_error(NGX_LOG_ERR, c->log, 0, "broken PROXY protocol TLV");
            return NGX_ERROR;
        }

        if (tlv->type == type) {
            value->data = p;
            value->len = len;
            return NGX_OK;
        }

        p += len;
        n -= len;
    }

    return NGX_DECLINED;
}


static u_char *
ngx_proxy_protocol_v2_write(ngx_connection_t *c, u_char *buf, u_char *last)
{
    struct sockaddr                 *src, *dst;
    ngx_proxy_protocol_v2_header_t  *header;
#if (NGX_HAVE_INET6)
    struct in6_addr                  v6_tmp;
    ngx_int_t                        v6_used;
#endif
#if (NGX_STREAM_SSL)
    ngx_tlv_ssl_t                   *tlv;
    u_char                          *value, *pos;
    u_char                           kbuf[100];
    const unsigned char             *data;
    unsigned int                     data_len;

    X509                            *crt;
	EVP_PKEY                        *key;
	const ASN1_OBJECT               *algorithm;
    const char                      *s;

    long                             rc;
    size_t                           tlv_len;
#endif
    size_t                           len;

    header = (ngx_proxy_protocol_v2_header_t *) buf;

    header->len = 0;

    src = c->sockaddr;
    dst = c->local_sockaddr;

    len = 0;

#if (NGX_HAVE_INET6)
    v6_used = 0;
#endif

    ngx_memcpy(header->signature, NGX_PROXY_PROTOCOL_V2_SIG,
            NGX_PROXY_PROTOCOL_V2_SIG_LEN);

    header->version_command = NGX_PROXY_PROTOCOL_V2_CMD_PROXY;
    header->family_transport = NGX_PROXY_PROTOCOL_V2_TRANS_STREAM;

    /** Addrs */

    switch (src->sa_family) {

    case AF_INET:

        if (dst->sa_family == AF_INET) {

            header->addr.ip4.src_addr =
                    ((struct sockaddr_in *) src)->sin_addr.s_addr;
            header->addr.ip4.src_port = ((struct sockaddr_in *) src)->sin_port;
        }
#if (NGX_HAVE_INET6)
        else /** dst == AF_INET6 */{

            ngx_v4tov6(&v6_tmp, src);
            ngx_memcpy(header->addr.ip6.src_addr, &v6_tmp, 16);
            header->addr.ip6.src_port = ((struct sockaddr_in *) src)->sin_port;
        }
#endif
        break;

#if (NGX_HAVE_INET6)
    case AF_INET6:
        v6_used = 1;

        ngx_memcpy(header->addr.ip6.src_addr,
                &((struct sockaddr_in6 *) src)->sin6_addr, 16);
        header->addr.ip6.src_port = ((struct sockaddr_in6 *) src)->sin6_port;

        break;
#endif

    default:
        ngx_log_debug1(NGX_LOG_DEBUG_CORE, c->log, 0,
                    "PROXY protocol v2 unsupported src address family %ui",
                    src->sa_family);
        goto unspec;
    };

    switch (dst->sa_family) {
    case AF_INET:

        if (src->sa_family == AF_INET) {

            header->addr.ip4.dst_addr =
                ((struct sockaddr_in *) dst)->sin_addr.s_addr;
            header->addr.ip4.dst_port = ((struct sockaddr_in *) dst)->sin_port;
        }
#if (NGX_HAVE_INET6)
        else /** src == AF_INET6 */{

            ngx_v4tov6(&v6_tmp, dst);
            ngx_memcpy(header->addr.ip6.dst_addr, &v6_tmp, 16);
            header->addr.ip6.dst_port = ((struct sockaddr_in *) dst)->sin_port;

        }
#endif
        break;

#if (NGX_HAVE_INET6)
    case AF_INET6:
        v6_used = 1;

        ngx_memcpy(header->addr.ip6.dst_addr,
                &((struct sockaddr_in6 *) dst)->sin6_addr, 16);
        header->addr.ip6.dst_port = ((struct sockaddr_in6 *) dst)->sin6_port;

        break;
#endif

    default:
        ngx_log_debug1(NGX_LOG_DEBUG_CORE, c->log, 0,
                    "PROXY protocol v2 unsupported dest address family %ui",
                    dst->sa_family);
        goto unspec;
    }

#if (NGX_HAVE_INET6)
    if (!v6_used) {
        header->family_transport |= NGX_PROXY_PROTOCOL_V2_FAM_INET;
        len = NGX_PROXY_PROTOCOL_V2_HDR_LEN_INET;

    } else {
        header->family_transport |= NGX_PROXY_PROTOCOL_V2_FAM_INET6;
        len = NGX_PROXY_PROTOCOL_V2_HDR_LEN_INET6;

    }
#else
    header->family_transport |= NGX_PROXY_PROTOCOL_V2_FAM_INET;
    len = NGX_PROXY_PROTOCOL_V2_HDR_LEN_INET;
#endif

    /** SSL TLVs */
#if (NGX_STREAM_SSL)

    if (c->ssl != NULL) {

        data = NULL;
        data_len = 0;

        tlv = (ngx_tlv_ssl_t *) (buf + len);
        ngx_memzero(tlv, sizeof(ngx_tlv_ssl_t));

        tlv->tlv.type = NGX_PROXY_PROTOCOL_V2_TYPE_SSL;
        pos = buf + len + sizeof(ngx_tlv_ssl_t);

        tlv->client |= NGX_PROXY_PROTOCOL_V2_CLIENT_SSL;

#ifdef TLSEXT_TYPE_application_layer_protocol_negotiation
        SSL_get0_alpn_selected(c->ssl->connection, &data, &data_len);

#ifdef TLSEXT_TYPE_next_proto_neg
        if (data_len == 0) {
            SSL_get0_next_proto_negotiated(c->ssl->connection,
                    &data, &data_len);
        }
#endif

#else /* TLSEXT_TYPE_next_proto_neg */
        SSL_get0_next_proto_negotiated(c->ssl->connection, &data, &data_len);
#endif

        if (data_len) {

            pos = ngx_copy_tlv(pos, last,
                        NGX_PROXY_PROTOCOL_V2_TYPE_ALPN,
                        (u_char *) data, (uint16_t) data_len);
            if (pos == NULL) {
                return NULL;
            }
        }

        value = (u_char *) SSL_get_version(c->ssl->connection);
        if (value != NULL) {

            pos = ngx_copy_tlv(pos, last,
                    NGX_PROXY_PROTOCOL_V2_SUBTYPE_SSL_VERSION,
                    value, ngx_strlen(value));
            if (pos == NULL) {
                return NULL;
            }
        }

        crt = SSL_get_peer_certificate(c->ssl->connection);
        if (crt != NULL) {

            tlv->client |= NGX_PROXY_PROTOCOL_V2_CLIENT_CERT_SESS;

            rc = SSL_get_verify_result(c->ssl->connection);
            tlv->verify = htonl(rc);

            if (rc == X509_V_OK) {

                if (ngx_ssl_ocsp_get_status(c, &s) == NGX_OK) {
                    tlv->client |= NGX_PROXY_PROTOCOL_V2_CLIENT_CERT_CONN;
                }
            }

            X509_NAME *subject_name_value = X509_get_subject_name(crt);
            if(subject_name_value != NULL) {
                int nid = OBJ_txt2nid("CN");
                int index = X509_NAME_get_index_by_NID(subject_name_value, nid, -1);

                X509_NAME_ENTRY *subject_name_cn_entry = X509_NAME_get_entry(subject_name_value, index);
                if (subject_name_cn_entry) {
                    ASN1_STRING *subject_name_cn_data_asn1 = X509_NAME_ENTRY_get_data(subject_name_cn_entry);

                    if (subject_name_cn_data_asn1 != NULL) {
                        value = (u_char *) ASN1_STRING_get0_data(subject_name_cn_data_asn1);
                        if(value != NULL) {
                            pos = ngx_copy_tlv(pos, last,
                                        NGX_PROXY_PROTOCOL_V2_SUBTYPE_SSL_CN,
                                        value, ngx_strlen(value));
                            if (pos == NULL) {
                                return NULL;
                            }
                        }
                    }
                }
            }

            X509_free(crt);
        }


        crt = SSL_get_certificate(c->ssl->connection);
	    if (crt != NULL) {

            key = X509_get_pubkey(crt);

            /** Key */
	        if (key != NULL) {

		        switch (EVP_PKEY_base_id(key)) {
		        case EVP_PKEY_RSA:
                    value = (u_char *) "RSA";
			    break;
		        case EVP_PKEY_EC:
                    value = (u_char *) "EC";
			    break;
		        case EVP_PKEY_DSA:
                    value = (u_char *) "DSA";
			    break;
                default:
                    value = NULL;
                break;
                }

                if (value != NULL) {

                    value = ngx_snprintf(kbuf, sizeof(kbuf) - 1, "%s%d%Z",
                            value, EVP_PKEY_bits(key));

                    pos = ngx_copy_tlv(pos, last,
                                NGX_PROXY_PROTOCOL_V2_SUBTYPE_SSL_KEY_ALG,
                                kbuf, ngx_strlen(kbuf));
                }

		        EVP_PKEY_free(key);

                if (pos == NULL) {
                    return NULL;
                }
            }

            /* ALG */
            X509_ALGOR_get0(&algorithm, NULL, NULL, X509_get0_tbs_sigalg(crt));
	        value = (u_char *) OBJ_nid2sn(OBJ_obj2nid(algorithm));

            if (value != NULL) {

                pos = ngx_copy_tlv(pos, last,
                            NGX_PROXY_PROTOCOL_V2_SUBTYPE_SSL_SIG_ALG,
                            value, ngx_strlen(value));
                if (pos == NULL) {
                    return NULL;
                }
            }
        }

        value = (u_char *) SSL_get_cipher_name(c->ssl->connection);
        if (value != NULL) {

            pos = ngx_copy_tlv(pos, last,
                    NGX_PROXY_PROTOCOL_V2_SUBTYPE_SSL_CIPHER,
                    value, ngx_strlen(value));
            if (pos == NULL) {
                return NULL;
            }
        }

        tlv_len = pos - (buf + len);

        tlv->tlv.length_hi = (uint16_t) (tlv_len - sizeof(ngx_tlv_t)) >> 8;
        tlv->tlv.length_lo = (uint16_t) (tlv_len - sizeof(ngx_tlv_t)) & 0x00ff;

        len = len + tlv_len;
    }

#endif

    header->len = htons(len - NGX_PROXY_PROTOCOL_V2_HDR_LEN);
    return buf + len;

unspec:
    header->family_transport |= NGX_PROXY_PROTOCOL_V2_FAM_UNSPEC;
    header->len = 0;

    return buf + NGX_PROXY_PROTOCOL_V2_HDR_LEN;
}


#if (NGX_HAVE_INET6)
static void
ngx_v4tov6(struct in6_addr *sin6_addr, struct sockaddr *addr)
{
    static const char rfc4291[] = { 0x00, 0x00, 0x00, 0x00,
                                    0x00, 0x00, 0x00, 0x00,
                                    0x00, 0x00, 0xFF, 0xFF };

    struct in_addr tmp_addr, *sin_addr;

    sin_addr = &((struct sockaddr_in *) addr)->sin_addr;

    tmp_addr.s_addr = sin_addr->s_addr;
    ngx_memcpy(sin6_addr->s6_addr, rfc4291, sizeof(rfc4291));
    ngx_memcpy(sin6_addr->s6_addr + 12, &tmp_addr.s_addr, 4);
}
#endif


#if (NGX_STREAM_SSL)

static u_char *
ngx_copy_tlv(u_char *pos, u_char *last, u_char type,
        u_char *value, uint16_t value_len)
{
    ngx_tlv_t   *tlv;

    if (last - pos < (long) sizeof(*tlv)) {
        return NULL;
    }

    tlv = (ngx_tlv_t *) pos;

    tlv->type = type;
    tlv->length_hi = (uint16_t) value_len >> 8;
    tlv->length_lo = (uint16_t) value_len & 0x00ff;
    ngx_memcpy(tlv->value, value, value_len);

    return pos + (value_len + sizeof(*tlv));
}

#endif


