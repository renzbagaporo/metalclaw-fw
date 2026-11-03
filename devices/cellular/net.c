// SPDX-License-Identifier: Apache-2.0 OR MIT

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "metalclaw/runtime.h"

#include "net.h"
#include "text.h"

#define HTTP_TERMINATOR "\r\n\r\n"
#define HTTP_TERMINATOR_LEN 4u

#define FTP_PREFIX "FTP "
#define FTP_PREFIX_LEN 4u

#define FNV_OFFSET 2166136261u
#define FNV_PRIME 16777619u

// A PUBLISH the broker forwards: fixed header, a one-byte remaining
// length, the topic length and at most a staging buffer of topic and
// payload.
#define MQTT_FORWARD_MAX (4u + NET_MQTT_LAST_SIZE)
#define MQTT_SUBACK_CODES 8u

enum {
    MQTT_PHASE_TYPE,
    MQTT_PHASE_LEN,
    MQTT_PHASE_BODY,
};

enum {
    PUB_TLEN_HI,
    PUB_TLEN_LO,
    PUB_TOPIC,
    PUB_PID_HI,
    PUB_PID_LO,
    PUB_PAYLOAD,
};

enum {
    SUB_PID_HI,
    SUB_PID_LO,
    SUB_FLEN_HI,
    SUB_FLEN_LO,
    SUB_FILTER,
    SUB_QOS,
};

void net_init(struct net *net, struct mclw_instance *inst, struct net_socket *sockets, uint8_t count,
    net_notify_t notify, void *ctx) {
    net->inst = inst;
    net->sockets = sockets;
    net->socket_count = count;
    net->notify = notify;
    net->ctx = ctx;
}

void net_defaults(struct net *net) {
    memset(&net->set, 0, sizeof(net->set));
    net->set.ip[0] = 10;
    net->set.ip[3] = 2;
}

void net_clear(struct net *net) {
    memset(&net->stats, 0, sizeof(net->stats));
}

void net_session(struct net *net) {
    memset(&net->ses, 0, sizeof(net->ses));
    memset(net->sockets, 0, net->socket_count * sizeof(net->sockets[0]));
    net->ses.mqtt_stage_owner = -1;
    net->ses.request_owner = -1;
}

static uint32_t fnv(uint32_t hash, uint8_t b) {
    return (hash ^ b) * FNV_PRIME;
}

static bool parse_ipv4(const char *p, size_t len, uint8_t ip[4]) {
    size_t part = 0;
    uint32_t value = 0;
    size_t digits = 0;

    for (size_t i = 0; i <= len; i++) {
        char c = i < len ? p[i] : '.';
        if (c >= '0' && c <= '9' && digits < 3) {
            value = value * 10u + (uint32_t)(c - '0');
            digits++;
        } else if (c == '.' && digits > 0 && value <= 255 && part < 4) {
            ip[part++] = (uint8_t)value;
            value = 0;
            digits = 0;
        } else {
            return false;
        }
    }
    return part == 4;
}

static bool name_matches(const char *a, size_t a_len, const char *b) {
    size_t i = 0;

    for (; i < a_len && b[i] != '\0'; i++) {
        char x = a[i] >= 'A' && a[i] <= 'Z' ? (char)(a[i] + 32) : a[i];
        char y = b[i] >= 'A' && b[i] <= 'Z' ? (char)(b[i] + 32) : b[i];
        if (x != y) {
            return false;
        }
    }
    return i == a_len && b[i] == '\0';
}

bool net_resolve(const struct net *net, const char *name, uint8_t ip[4]) {
    const char *hosts = net->set.hosts;
    size_t p = 0;

    if (parse_ipv4(name, strlen(name), ip)) {
        return true;
    }
    while (p < NET_HOSTS_SIZE && hosts[p] != '\0') {
        size_t end = p;
        size_t eq = NET_HOSTS_SIZE;
        while (end < NET_HOSTS_SIZE && hosts[end] != '\0' && hosts[end] != '\n') {
            if (hosts[end] == '=' && eq == NET_HOSTS_SIZE) {
                eq = end;
            }
            end++;
        }
        size_t value_end = end;
        while (value_end > p && hosts[value_end - 1] == '\r') {
            value_end--;
        }
        if (eq < value_end && name_matches(&hosts[p], eq - p, name) &&
            parse_ipv4(&hosts[eq + 1], value_end - eq - 1, ip)) {
            return true;
        }
        p = end + 1;
    }
    return false;
}

static struct net_socket *sock_at(struct net *net, uint8_t sock) {
    return &net->sockets[sock];
}

static size_t http_header(uint16_t body, struct text *t) {
    text_init(t);
    text_str(t, body != 0 ? "HTTP/1.1 200 OK\r\n" : "HTTP/1.1 404 Not Found\r\n");
    text_str(t, "Content-Type: application/octet-stream\r\nContent-Length: ");
    text_u32(t, body);
    text_str(t, "\r\nConnection: close\r\n\r\n");
    return t->len;
}

static uint32_t http_total(const struct net_socket *sock) {
    struct text t;

    return (uint32_t)http_header(sock->http_body, &t) + sock->http_body;
}

static uint32_t available(const struct net_socket *sock) {
    uint32_t n = sock->rx_len;

    if (sock->http_responding) {
        n += http_total(sock) - sock->http_off;
    }
    return n;
}

uint32_t net_available(const struct net *net, uint8_t sock) {
    return available(&net->sockets[sock]);
}

size_t net_peek_bytes(const struct net *net, uint8_t sock, uint32_t off, uint8_t *buf, size_t len) {
    const struct net_socket *s = &net->sockets[sock];
    uint32_t avail = available(s);
    size_t done = 0;

    if (off >= avail) {
        return 0;
    }
    if (len > avail - off) {
        len = avail - off;
    }
    for (; done < len && off + done < s->rx_len; done++) {
        buf[done] = s->rx[(s->rx_head + off + done) % NET_SOCK_RX];
    }
    if (done < len) {
        struct text header;
        size_t header_len = http_header(s->http_body, &header);
        size_t at = s->http_off + (off + done - s->rx_len);
        for (; done < len; done++, at++) {
            buf[done] = at < header_len ? (uint8_t)header.buf[at] : net->set.file[at - header_len];
        }
    }
    return done;
}

uint32_t net_space(const struct net *net, uint8_t sock) {
    const struct net_socket *s = &net->sockets[sock];

    if (s->state != NET_SOCK_OPEN) {
        return 0;
    }
    if (s->service == NET_SVC_ECHO || s->service == NET_SVC_MQTT) {
        return NET_SOCK_RX - s->rx_len;
    }
    return NET_SOCK_RX;
}

uint8_t net_peek(const struct net *net, uint8_t sock, uint32_t off) {
    const struct net_socket *s = &net->sockets[sock];

    if (off < s->rx_len) {
        return s->rx[(s->rx_head + off) % NET_SOCK_RX];
    }
    struct text header;
    size_t header_len = http_header(s->http_body, &header);
    size_t at = s->http_off + (off - s->rx_len);
    return at < header_len ? (uint8_t)header.buf[at] : net->set.file[at - header_len];
}

static void notify(struct net *net, uint8_t sock, enum net_notice notice) {
    net->notify(net->ctx, sock, notice);
}

void net_close(struct net *net, uint8_t sock) {
    if (net->ses.mqtt_stage_owner == (int8_t)sock) {
        net->ses.mqtt_stage_owner = -1;
    }
    if (net->ses.request_owner == (int8_t)sock) {
        net->ses.request_owner = -1;
    }
    memset(sock_at(net, sock), 0, sizeof(struct net_socket));
}

void net_abort(struct net *net, uint8_t sock) {
    net_close(net, sock);
    sock_at(net, sock)->state = NET_SOCK_REMOTE_CLOSED;
}

static void remote_close(struct net *net, uint8_t sock) {
    struct net_socket *s = sock_at(net, sock);

    s->state = NET_SOCK_REMOTE_CLOSED;
    s->closing = false;
    notify(net, sock, NET_NOTICE_CLOSED);
}

// The server's close is reported only once the front end has read
// everything.
static void close_if_drained(struct net *net, uint8_t sock) {
    struct net_socket *s = sock_at(net, sock);

    if (s->closing && s->state == NET_SOCK_OPEN && available(s) == 0) {
        remote_close(net, sock);
    }
}

static void ring_push(struct net *net, uint8_t sock, const uint8_t *data, size_t len) {
    struct net_socket *s = sock_at(net, sock);
    bool was_empty = available(s) == 0;
    size_t copied = 0;

    for (; copied < len && s->rx_len < NET_SOCK_RX; copied++) {
        s->rx[(s->rx_head + s->rx_len) % NET_SOCK_RX] = data[copied];
        s->rx_len++;
    }
    net->stats.drops += (uint32_t)(len - copied);
    if (was_empty && copied > 0) {
        notify(net, sock, NET_NOTICE_DATA);
    }
}

// Takes up to `len` bytes, copied into `buf` unless it is NULL.
static size_t take(struct net *net, uint8_t sock, uint8_t *buf, size_t len) {
    struct net_socket *s = sock_at(net, sock);
    size_t n = available(s);
    size_t done = 0;

    if (n > len) {
        n = len;
    }
    while (done < n && s->rx_len > 0) {
        size_t chunk = NET_SOCK_RX - s->rx_head;
        if (chunk > s->rx_len) {
            chunk = s->rx_len;
        }
        if (chunk > n - done) {
            chunk = n - done;
        }
        if (buf != NULL) {
            memcpy(&buf[done], &s->rx[s->rx_head], chunk);
        }
        s->rx_head = (uint16_t)((s->rx_head + chunk) % NET_SOCK_RX);
        s->rx_len = (uint16_t)(s->rx_len - chunk);
        done += chunk;
    }
    if (s->rx_len == 0) {
        s->rx_head = 0;
    }
    if (done < n && s->http_responding) {
        struct text header;
        size_t header_len = http_header(s->http_body, &header);
        if (s->http_off < header_len) {
            size_t chunk = header_len - s->http_off;
            if (chunk > n - done) {
                chunk = n - done;
            }
            if (buf != NULL) {
                memcpy(&buf[done], &header.buf[s->http_off], chunk);
            }
            s->http_off = (uint16_t)(s->http_off + chunk);
            done += chunk;
        }
        if (done < n) {
            if (buf != NULL) {
                memcpy(&buf[done], &net->set.file[s->http_off - header_len], n - done);
            }
            s->http_off = (uint16_t)(s->http_off + (n - done));
            done = n;
        }
        if (s->http_off == http_total(s)) {
            s->http_responding = false;
        }
    }
    net->stats.bytes_down += (uint32_t)done;
    close_if_drained(net, sock);
    return done;
}

size_t net_read(struct net *net, uint8_t sock, uint8_t *buf, size_t len) {
    return take(net, sock, buf, len);
}

void net_discard(struct net *net, uint8_t sock, size_t len) {
    take(net, sock, NULL, len);
}

// The request is collected until its blank line; its first line goes to
// the request region. The response is generated as it is read, from the
// file region, and the server closes once it has all been read.
static void http_byte(struct net *net, uint8_t sock, uint8_t b) {
    struct net_socket *s = sock_at(net, sock);
    struct net_session *ses = &net->ses;

    if (s->http_responding || s->closing) {
        return;
    }
    if (!s->http_started) {
        s->http_started = true;
        s->http_line_done = false;
        memset(net->stats.request, 0, sizeof(net->stats.request));
        ses->request_len = 0;
        ses->request_owner = (int8_t)sock;
    }
    if (!s->http_line_done && ses->request_owner == (int8_t)sock) {
        if (b == '\r' || b == '\n') {
            s->http_line_done = true;
        } else if (ses->request_len < NET_REQUEST_SIZE - 1) {
            net->stats.request[ses->request_len++] = (char)b;
        }
    }
    if (b == (uint8_t)HTTP_TERMINATOR[s->http_match]) {
        s->http_match++;
    } else {
        s->http_match = b == '\r' ? 1 : 0;
    }
    if (s->http_match == HTTP_TERMINATOR_LEN) {
        s->http_body = net->set.file_length;
        s->http_off = 0;
        s->http_responding = true;
        s->closing = true;
        notify(net, sock, NET_NOTICE_DATA);
    }
}

static bool subscribed(const struct net_socket *s, uint32_t hash) {
    for (uint8_t i = 0; i < s->sub_count; i++) {
        if (s->subs[i] == hash) {
            return true;
        }
    }
    return false;
}

// Forwards a QoS 0 PUBLISH to every MQTT socket subscribed to the topic.
// Filters match exactly; wildcards are not modelled.
static void mqtt_forward(struct net *net, const uint8_t *topic, size_t topic_len, const uint8_t *payload,
    size_t payload_len) {
    uint8_t packet[MQTT_FORWARD_MAX];
    uint32_t hash = FNV_OFFSET;
    size_t remaining = 2u + topic_len + payload_len;

    if (remaining > 127u || 4u + topic_len + payload_len > sizeof(packet)) {
        mclw_trace(net->inst, MCLW_TRACE_ERROR, NET_MQTT_PUBLISH, (uint16_t)remaining, -1);
        return;
    }
    for (size_t i = 0; i < topic_len; i++) {
        hash = fnv(hash, topic[i]);
    }
    packet[0] = NET_MQTT_PUBLISH << 4;
    packet[1] = (uint8_t)remaining;
    packet[2] = (uint8_t)(topic_len >> 8);
    packet[3] = (uint8_t)topic_len;
    memcpy(&packet[4], topic, topic_len);
    memcpy(&packet[4 + topic_len], payload, payload_len);
    for (uint8_t sock = 0; sock < net->socket_count; sock++) {
        const struct net_socket *s = sock_at(net, sock);
        if (s->state == NET_SOCK_OPEN && s->service == NET_SVC_MQTT && !s->closing && subscribed(s, hash)) {
            ring_push(net, sock, packet, 4u + topic_len + payload_len);
        }
    }
}

void net_mqtt_inject(struct net *net) {
    const uint8_t *inject = (const uint8_t *)net->set.mqtt_inject;
    size_t len = text_strnlen(net->set.mqtt_inject, NET_MQTT_INJECT_SIZE);
    size_t topic_len = 0;

    while (topic_len < len && inject[topic_len] != '\n') {
        topic_len++;
    }
    size_t payload_start = topic_len < len ? topic_len + 1 : len;
    mqtt_forward(net, inject, topic_len, &inject[payload_start], len - payload_start);
}

static void stage_put(struct net *net, uint8_t sock, uint8_t b) {
    struct net_session *ses = &net->ses;

    if (ses->mqtt_stage_owner != (int8_t)sock) {
        return;
    }
    if (ses->mqtt_stage_len < NET_MQTT_LAST_SIZE) {
        ses->mqtt_stage[ses->mqtt_stage_len++] = b;
    } else {
        ses->mqtt_stage_full = true;
    }
}

static void publish_payload_starts(struct net *net, uint8_t sock) {
    struct net_mqtt_rx *m = &sock_at(net, sock)->mqtt;

    if (net->ses.mqtt_stage_owner == (int8_t)sock) {
        net->ses.mqtt_topic_len = net->ses.mqtt_stage_len;
    }
    stage_put(net, sock, '\n');
    m->step = PUB_PAYLOAD;
}

static void publish_byte(struct net *net, uint8_t sock, uint8_t b) {
    struct net_mqtt_rx *m = &sock_at(net, sock)->mqtt;
    bool qos = ((m->type >> 1) & 3u) != 0;

    switch (m->step) {
        case PUB_TLEN_HI:
            m->field_len = (uint16_t)(b << 8);
            m->step = PUB_TLEN_LO;
            break;
        case PUB_TLEN_LO:
            m->field_len |= b;
            m->field_pos = 0;
            if (m->field_len != 0) {
                m->step = PUB_TOPIC;
            } else if (qos) {
                m->step = PUB_PID_HI;
            } else {
                publish_payload_starts(net, sock);
            }
            break;
        case PUB_TOPIC:
            stage_put(net, sock, b);
            if (++m->field_pos == m->field_len) {
                if (qos) {
                    m->step = PUB_PID_HI;
                } else {
                    publish_payload_starts(net, sock);
                }
            }
            break;
        case PUB_PID_HI:
            m->pid = (uint16_t)(b << 8);
            m->step = PUB_PID_LO;
            break;
        case PUB_PID_LO:
            m->pid |= b;
            publish_payload_starts(net, sock);
            break;
        default:
            stage_put(net, sock, b);
            break;
    }
}

static void subscribe_byte(struct net *net, uint8_t sock, uint8_t b) {
    struct net_socket *s = sock_at(net, sock);
    struct net_mqtt_rx *m = &s->mqtt;
    bool subscribe = (m->type >> 4) == NET_MQTT_SUBSCRIBE;

    switch (m->step) {
        case SUB_PID_HI:
            m->pid = (uint16_t)(b << 8);
            m->step = SUB_PID_LO;
            break;
        case SUB_PID_LO:
            m->pid |= b;
            m->step = SUB_FLEN_HI;
            break;
        case SUB_FLEN_HI:
            m->field_len = (uint16_t)(b << 8);
            m->step = SUB_FLEN_LO;
            break;
        case SUB_FLEN_LO:
            m->field_len |= b;
            m->field_pos = 0;
            m->hash = FNV_OFFSET;
            m->step = m->field_len != 0 ? SUB_FILTER : (subscribe ? SUB_QOS : SUB_FLEN_HI);
            break;
        case SUB_FILTER:
            m->hash = fnv(m->hash, b);
            if (++m->field_pos < m->field_len) {
                break;
            }
            if (subscribe) {
                m->step = SUB_QOS;
                break;
            }
            for (uint8_t i = 0; i < s->sub_count; i++) {
                if (s->subs[i] == m->hash) {
                    s->subs[i] = s->subs[--s->sub_count];
                    break;
                }
            }
            m->step = SUB_FLEN_HI;
            break;
        default:
            if (subscribed(s, m->hash) || s->sub_count < NET_MQTT_SUBS) {
                if (!subscribed(s, m->hash)) {
                    s->subs[s->sub_count++] = m->hash;
                }
                if (m->filters < MQTT_SUBACK_CODES) {
                    m->granted |= (uint8_t)(1u << m->filters);
                }
            }
            if (m->filters < MQTT_SUBACK_CODES) {
                m->filters++;
            }
            m->step = SUB_FLEN_HI;
            break;
    }
}

static void packet_begin(struct net *net, uint8_t sock) {
    struct net_mqtt_rx *m = &sock_at(net, sock)->mqtt;
    struct net_session *ses = &net->ses;

    m->pos = 0;
    m->step = 0;
    m->pid = 0;
    m->filters = 0;
    m->granted = 0;
    m->field_len = 0;
    m->field_pos = 0;
    m->hash = FNV_OFFSET;
    if ((m->type >> 4) == NET_MQTT_PUBLISH) {
        ses->mqtt_stage_owner = (int8_t)sock;
        ses->mqtt_stage_len = 0;
        ses->mqtt_topic_len = 0;
        ses->mqtt_stage_full = false;
    }
}

static void publish_end(struct net *net, uint8_t sock) {
    struct net_mqtt_rx *m = &sock_at(net, sock)->mqtt;
    struct net_session *ses = &net->ses;

    net->stats.mqtt_publishes++;
    if (ses->mqtt_stage_owner == (int8_t)sock) {
        memset(net->stats.mqtt_last, 0, sizeof(net->stats.mqtt_last));
        memcpy(net->stats.mqtt_last, ses->mqtt_stage, ses->mqtt_stage_len);
        ses->mqtt_stage_owner = -1;
        if (!ses->mqtt_stage_full && m->step == PUB_PAYLOAD) {
            size_t topic_len = ses->mqtt_topic_len;
            mqtt_forward(net, ses->mqtt_stage, topic_len, &ses->mqtt_stage[topic_len + 1],
                ses->mqtt_stage_len - topic_len - 1u);
        }
    }
    if (((m->type >> 1) & 3u) == 1) {
        uint8_t puback[4] = { NET_MQTT_PUBACK << 4, 2, (uint8_t)(m->pid >> 8), (uint8_t)m->pid };
        ring_push(net, sock, puback, sizeof(puback));
    }
}

static void packet_end(struct net *net, uint8_t sock) {
    struct net_socket *s = sock_at(net, sock);
    struct net_mqtt_rx *m = &s->mqtt;

    m->phase = MQTT_PHASE_TYPE;
    switch (m->type >> 4) {
        case NET_MQTT_CONNECT: {
            static const uint8_t connack[4] = { NET_MQTT_CONNACK << 4, 2, 0, 0 };
            ring_push(net, sock, connack, sizeof(connack));
            break;
        }
        case NET_MQTT_PUBLISH:
            publish_end(net, sock);
            break;
        case NET_MQTT_SUBSCRIBE: {
            uint8_t suback[4 + MQTT_SUBACK_CODES] = {
                NET_MQTT_SUBACK << 4, (uint8_t)(2 + m->filters), (uint8_t)(m->pid >> 8), (uint8_t)m->pid,
            };
            for (uint8_t i = 0; i < m->filters; i++) {
                suback[4 + i] = (m->granted & (1u << i)) != 0 ? 0 : NET_MQTT_SUBACK_FAILURE;
            }
            ring_push(net, sock, suback, 4u + m->filters);
            break;
        }
        case NET_MQTT_UNSUBSCRIBE: {
            uint8_t unsuback[4] = { NET_MQTT_UNSUBACK << 4, 2, (uint8_t)(m->pid >> 8), (uint8_t)m->pid };
            ring_push(net, sock, unsuback, sizeof(unsuback));
            break;
        }
        case NET_MQTT_PINGREQ: {
            static const uint8_t pingresp[2] = { NET_MQTT_PINGRESP << 4, 0 };
            ring_push(net, sock, pingresp, sizeof(pingresp));
            break;
        }
        case NET_MQTT_DISCONNECT:
            s->closing = true;
            close_if_drained(net, sock);
            break;
        default:
            break;
    }
}

static void mqtt_byte(struct net *net, uint8_t sock, uint8_t b) {
    struct net_socket *s = sock_at(net, sock);
    struct net_mqtt_rx *m = &s->mqtt;

    if (s->closing) {
        return;
    }
    switch (m->phase) {
        case MQTT_PHASE_TYPE:
            m->type = b;
            m->remaining = 0;
            m->shift = 0;
            m->phase = MQTT_PHASE_LEN;
            break;
        case MQTT_PHASE_LEN:
            m->remaining |= (uint32_t)(b & 0x7fu) << m->shift;
            m->shift = (uint8_t)(m->shift + 7);
            if ((b & 0x80u) != 0) {
                if (m->shift > 21) {
                    m->phase = MQTT_PHASE_TYPE;
                }
                break;
            }
            packet_begin(net, sock);
            if (m->remaining == 0) {
                packet_end(net, sock);
            } else {
                m->phase = MQTT_PHASE_BODY;
            }
            break;
        default: {
            uint8_t type = m->type >> 4;
            if (type == NET_MQTT_PUBLISH) {
                publish_byte(net, sock, b);
            } else if (type == NET_MQTT_SUBSCRIBE || type == NET_MQTT_UNSUBSCRIBE) {
                subscribe_byte(net, sock, b);
            }
            if (++m->pos == m->remaining) {
                packet_end(net, sock);
            }
            break;
        }
    }
}

static bool service_for(bool udp, uint16_t port, uint8_t *service) {
    if (port == NET_PORT_ECHO) {
        *service = NET_SVC_ECHO;
    } else if (udp || port == NET_PORT_DISCARD) {
        *service = NET_SVC_DISCARD;
    } else if (port == NET_PORT_HTTP || port == NET_PORT_HTTPS || port == NET_PORT_HTTP_ALT) {
        *service = NET_SVC_HTTP;
    } else if (port == NET_PORT_MQTT) {
        *service = NET_SVC_MQTT;
    } else {
        return false;
    }
    return true;
}

bool net_open(struct net *net, uint8_t sock, bool udp, const uint8_t ip[4], uint16_t port) {
    uint8_t service;

    if (!service_for(udp, port, &service)) {
        return false;
    }
    net_close(net, sock);
    struct net_socket *s = sock_at(net, sock);
    s->state = NET_SOCK_OPEN;
    s->service = service;
    s->udp = udp;
    s->port = port;
    memcpy(s->ip, ip, sizeof(s->ip));
    net->stats.connections++;
    return true;
}

void net_send(struct net *net, uint8_t sock, const uint8_t *data, size_t len) {
    struct net_socket *s = sock_at(net, sock);

    net->stats.bytes_up += (uint32_t)len;
    if (s->state != NET_SOCK_OPEN) {
        return;
    }
    switch (s->service) {
        case NET_SVC_ECHO:
            ring_push(net, sock, data, len);
            break;
        case NET_SVC_HTTP:
            for (size_t i = 0; i < len; i++) {
                http_byte(net, sock, data[i]);
            }
            break;
        case NET_SVC_MQTT:
            for (size_t i = 0; i < len; i++) {
                mqtt_byte(net, sock, data[i]);
            }
            break;
        default:
            break;
    }
}

static void ftp_request(struct net *net, const char *path, size_t path_len, const char *name, size_t name_len) {
    char *req = net->stats.request;
    size_t room = NET_REQUEST_SIZE - 1 - FTP_PREFIX_LEN;

    if (path_len > room) {
        path_len = room;
    }
    if (name_len > room - path_len) {
        name_len = room - path_len;
    }
    memmove(&req[FTP_PREFIX_LEN + path_len], name, name_len);
    memmove(&req[FTP_PREFIX_LEN], path, path_len);
    memcpy(req, FTP_PREFIX, FTP_PREFIX_LEN);
    memset(&req[FTP_PREFIX_LEN + path_len + name_len], 0, NET_REQUEST_SIZE - (FTP_PREFIX_LEN + path_len + name_len));
    net->ses.ftp_path_len = (uint8_t)path_len;
    net->ses.ftp_name_len = (uint8_t)name_len;
}

static bool ftp_names_valid(const struct net *net) {
    return memcmp(net->stats.request, FTP_PREFIX, FTP_PREFIX_LEN) == 0;
}

void net_ftp_name(struct net *net, const char *name) {
    char path[NET_REQUEST_SIZE];
    size_t path_len = ftp_names_valid(net) ? net->ses.ftp_path_len : 0;

    memcpy(path, &net->stats.request[FTP_PREFIX_LEN], path_len);
    ftp_request(net, path, path_len, name, strlen(name));
}

void net_ftp_path(struct net *net, const char *path) {
    char name[NET_REQUEST_SIZE];
    size_t name_len = 0;

    if (ftp_names_valid(net)) {
        name_len = net->ses.ftp_name_len;
        memcpy(name, &net->stats.request[FTP_PREFIX_LEN + net->ses.ftp_path_len], name_len);
    }
    ftp_request(net, path, strlen(path), name, name_len);
}
