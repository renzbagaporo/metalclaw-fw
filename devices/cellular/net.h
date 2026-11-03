// SPDX-License-Identifier: Apache-2.0 OR MIT
// The fake network behind every model of the family: the address the
// bearer gets, a hosts table, sockets, and the services that answer on
// them, chosen by destination port: echo, discard, an HTTP server serving
// the file region, and an MQTT 3.1.1 broker; plus the FTP server's record
// of what was asked for. It knows no AT dialect. A front end (a model's
// socket commands, or an IP stack behind PPP) opens, sends to, reads from
// and closes sockets by index, and hears through `notify` when a socket's
// data goes from none to some and when the server closes it.

#ifndef MCLW_DEVICES_CELLULAR_NET_H
#define MCLW_DEVICES_CELLULAR_NET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "metalclaw/runtime.h"

#define NET_HOSTS_SIZE 192
#define NET_FILE_SIZE 1024
#define NET_REQUEST_SIZE 96
#define NET_MQTT_LAST_SIZE 96
#define NET_MQTT_INJECT_SIZE 96
#define NET_SOCK_RX 256
#define NET_HOST_MAX 64
#define NET_MQTT_SUBS 2

#define NET_PORT_ECHO 7
#define NET_PORT_DISCARD 9
#define NET_PORT_HTTP 80
#define NET_PORT_HTTPS 443
#define NET_PORT_HTTP_ALT 8080
#define NET_PORT_MQTT 1883

// MQTT 3.1.1 control packet types (high nibble of the first byte).
#define NET_MQTT_CONNECT 1
#define NET_MQTT_CONNACK 2
#define NET_MQTT_PUBLISH 3
#define NET_MQTT_PUBACK 4
#define NET_MQTT_SUBSCRIBE 8
#define NET_MQTT_SUBACK 9
#define NET_MQTT_UNSUBSCRIBE 10
#define NET_MQTT_UNSUBACK 11
#define NET_MQTT_PINGREQ 12
#define NET_MQTT_PINGRESP 13
#define NET_MQTT_DISCONNECT 14
#define NET_MQTT_SUBACK_FAILURE 0x80

enum net_sock_state {
    NET_SOCK_CLOSED,
    NET_SOCK_OPEN,
    // The server or the network ended it; the front end has not closed it.
    NET_SOCK_REMOTE_CLOSED,
};

enum net_service {
    NET_SVC_ECHO,
    NET_SVC_DISCARD,
    NET_SVC_HTTP,
    NET_SVC_MQTT,
};

enum net_notice {
    // The socket's readable data went from none to some.
    NET_NOTICE_DATA,
    // The server closed the socket after everything it sent was read.
    NET_NOTICE_CLOSED,
};

typedef void (*net_notify_t)(void *ctx, uint8_t sock, enum net_notice notice);

// One MQTT 3.1.1 packet being parsed out of a socket's upstream bytes.
struct net_mqtt_rx {
    uint32_t remaining;
    uint32_t pos;
    uint32_t hash;
    uint16_t field_len;
    uint16_t field_pos;
    uint16_t pid;
    uint8_t phase;
    uint8_t step;
    uint8_t type;
    uint8_t shift;
    uint8_t filters;
    uint8_t granted;
};

// Front ends read `state`, `udp`, `port` and `ip`, and change a socket only
// through the functions below.
struct net_socket {
    uint8_t state;
    uint8_t service;
    bool udp;
    // The server closes once everything it sent has been read.
    bool closing;
    bool http_started;
    bool http_line_done;
    bool http_responding;
    uint8_t http_match;
    uint8_t sub_count;
    uint16_t rx_head;
    uint16_t rx_len;
    uint16_t http_off;
    uint16_t http_body;
    uint16_t port;
    uint8_t ip[4];
    uint32_t subs[NET_MQTT_SUBS];
    struct net_mqtt_rx mqtt;
    uint8_t rx[NET_SOCK_RX];
};

// What the user sets: kept by reset(), restored by reset(factory).
struct net_settings {
    uint16_t file_length;
    uint8_t ip[4];
    char hosts[NET_HOSTS_SIZE];
    uint8_t file[NET_FILE_SIZE];
    char mqtt_inject[NET_MQTT_INJECT_SIZE];
};

// What the network observed: kept across power cycles, cleared by reset().
struct net_stats {
    uint32_t connections;
    uint32_t bytes_up;
    uint32_t bytes_down;
    uint32_t drops;
    uint32_t mqtt_publishes;
    char request[NET_REQUEST_SIZE];
    char mqtt_last[NET_MQTT_LAST_SIZE];
};

// Cleared by every power transition, sockets included.
struct net_session {
    bool bearer;
    uint8_t ftp_path_len;
    uint8_t ftp_name_len;
    uint8_t mqtt_stage_len;
    uint8_t mqtt_topic_len;
    int8_t mqtt_stage_owner;
    int8_t request_owner;
    bool mqtt_stage_full;
    uint16_t request_len;
    uint8_t mqtt_stage[NET_MQTT_LAST_SIZE];
};

struct net {
    struct mclw_instance *inst;
    net_notify_t notify;
    void *ctx;
    struct net_socket *sockets;
    uint8_t socket_count;
    struct net_settings set;
    struct net_stats stats;
    struct net_session ses;
};

// `sockets` is the model's array of `count`, in its own state.
void net_init(struct net *net, struct mclw_instance *inst, struct net_socket *sockets, uint8_t count,
    net_notify_t notify, void *ctx);
void net_defaults(struct net *net);
void net_clear(struct net *net);
void net_session(struct net *net);

// A dotted IPv4 literal, or a `name=a.b.c.d` line of the hosts region.
bool net_resolve(const struct net *net, const char *name, uint8_t ip[4]);

// Connects a closed socket, counting the connection; false, leaving the
// socket untouched, when nothing listens on that port.
bool net_open(struct net *net, uint8_t sock, bool udp, const uint8_t ip[4], uint16_t port);
// Releases a socket in any state; its unread data is gone.
void net_close(struct net *net, uint8_t sock);
// The network dropped the connection: closed by the far end at once,
// without its pending data and without a notice.
void net_abort(struct net *net, uint8_t sock);
// Upstream bytes, counted even when the socket is not open.
void net_send(struct net *net, uint8_t sock, const uint8_t *data, size_t len);
uint32_t net_available(const struct net *net, uint8_t sock);
// The byte `off` places past the read position; `off` < available.
uint8_t net_peek(const struct net *net, uint8_t sock, uint32_t off);
// Takes up to `len` bytes the server sent, counting them. Reading the last
// byte of a closing connection closes it (NET_NOTICE_CLOSED).
size_t net_read(struct net *net, uint8_t sock, uint8_t *buf, size_t len);
// Up to `len` bytes from `off` past the read position, left in place: a
// transport that keeps sent bytes until they are acknowledged peeks them
// and discards them on the acknowledgement.
size_t net_peek_bytes(const struct net *net, uint8_t sock, uint32_t off, uint8_t *buf, size_t len);
// net_read without the copy.
void net_discard(struct net *net, uint8_t sock, size_t len);
// What the service takes before it drops: the receive window a transport
// may offer. 0 unless the socket is open.
uint32_t net_space(const struct net *net, uint8_t sock);

// Publishes the mqtt_inject region to every subscribed socket.
void net_mqtt_inject(struct net *net);

// The FTP server records "FTP <path><name>" in the request region as the
// two are set, in either order.
void net_ftp_name(struct net *net, const char *name);
void net_ftp_path(struct net *net, const char *path);

#endif
