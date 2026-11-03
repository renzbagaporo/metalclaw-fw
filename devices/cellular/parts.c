// SPDX-License-Identifier: Apache-2.0 OR MIT
// The property and region parts of the fake network and the GNSS fix.

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "metalclaw/value.h"

#include "gnss.h"
#include "modem.h"
#include "net.h"

enum {
    NET_PROP_PDP,
    NET_PROP_IP,
    NET_PROP_FILE_LENGTH,
    NET_PROP_CONNECTIONS,
    NET_PROP_BYTES_UP,
    NET_PROP_BYTES_DOWN,
    NET_PROP_DROPS,
    NET_PROP_MQTT_PUBLISHES,
    NET_PROP_MQTT_SEND,
    NET_PROP_COUNT,
};

enum {
    NET_REGION_HOSTS,
    NET_REGION_FILE,
    NET_REGION_REQUEST,
    NET_REGION_MQTT_LAST,
    NET_REGION_MQTT_INJECT,
    NET_REGION_END,
};

enum {
    GNSS_PROP_LATITUDE,
    GNSS_PROP_LONGITUDE,
    GNSS_PROP_ALTITUDE,
    GNSS_PROP_SPEED,
    GNSS_PROP_COURSE,
    GNSS_PROP_HDOP,
    GNSS_PROP_SATELLITES,
    GNSS_PROP_FIX,
    GNSS_PROP_POWER,
    GNSS_PROP_COUNT,
};

_Static_assert(NET_PROP_COUNT == NET_PROPERTY_COUNT, "NET_PROPERTIES and the net part disagree");
_Static_assert(NET_REGION_END == NET_REGION_COUNT, "NET_REGIONS and the net part disagree");
_Static_assert(GNSS_PROP_COUNT == GNSS_PROPERTY_COUNT, "GNSS_PROPERTIES and the gnss part disagree");

static void net_part_defaults(struct modem *m) {
    net_defaults(m->net);
}

static void net_part_clear(struct modem *m) {
    net_clear(m->net);
}

static void net_part_session(struct modem *m) {
    net_session(m->net);
}

static int net_part_get(struct modem *m, size_t property, struct mclw_value *out) {
    const struct net *net = m->net;

    switch (property) {
        case NET_PROP_PDP:
            out->v[0] = net->ses.bearer;
            return 0;
        case NET_PROP_IP:
            for (size_t i = 0; i < 4; i++) {
                out->v[i] = net->set.ip[i];
            }
            return 0;
        case NET_PROP_FILE_LENGTH:
            out->v[0] = net->set.file_length;
            return 0;
        case NET_PROP_CONNECTIONS:
            out->v[0] = net->stats.connections;
            return 0;
        case NET_PROP_BYTES_UP:
            out->v[0] = net->stats.bytes_up;
            return 0;
        case NET_PROP_BYTES_DOWN:
            out->v[0] = net->stats.bytes_down;
            return 0;
        case NET_PROP_DROPS:
            out->v[0] = net->stats.drops;
            return 0;
        case NET_PROP_MQTT_PUBLISHES:
            out->v[0] = net->stats.mqtt_publishes;
            return 0;
        default:
            return -EINVAL;
    }
}

static int net_part_set(struct modem *m, size_t property, const struct mclw_value *in) {
    struct net *net = m->net;

    switch (property) {
        case NET_PROP_IP:
            for (size_t i = 0; i < 4; i++) {
                net->set.ip[i] = (uint8_t)in->v[i];
            }
            return 0;
        case NET_PROP_FILE_LENGTH:
            net->set.file_length = (uint16_t)in->v[0];
            return 0;
        case NET_PROP_MQTT_SEND:
            if (in->v[0] != 0) {
                net_mqtt_inject(net);
            }
            return 0;
        default:
            return -EINVAL;
    }
}

static void *net_part_region(struct modem *m, size_t region) {
    struct net *net = m->net;

    switch (region) {
        case NET_REGION_HOSTS:
            return net->set.hosts;
        case NET_REGION_FILE:
            return net->set.file;
        case NET_REGION_REQUEST:
            return net->stats.request;
        case NET_REGION_MQTT_LAST:
            return net->stats.mqtt_last;
        case NET_REGION_MQTT_INJECT:
            return net->set.mqtt_inject;
        default:
            return NULL;
    }
}

const struct modem_part net_part = {
    .property_count = NET_PROPERTY_COUNT,
    .region_count = NET_REGION_COUNT,
    .defaults = net_part_defaults,
    .clear = net_part_clear,
    .session = net_part_session,
    .get = net_part_get,
    .set = net_part_set,
    .region = net_part_region,
    .written = NULL,
};

static void gnss_part_defaults(struct modem *m) {
    memset(m->gnss, 0, sizeof(*m->gnss));
    m->gnss->hdop = GNSS_DEFAULT_HDOP;
}

static void gnss_part_session(struct modem *m) {
    m->gnss->on = false;
}

static int gnss_part_get(struct modem *m, size_t property, struct mclw_value *out) {
    const struct gnss *g = m->gnss;

    switch (property) {
        case GNSS_PROP_LATITUDE:
            out->v[0] = g->latitude;
            return 0;
        case GNSS_PROP_LONGITUDE:
            out->v[0] = g->longitude;
            return 0;
        case GNSS_PROP_ALTITUDE:
            out->v[0] = g->altitude;
            return 0;
        case GNSS_PROP_SPEED:
            out->v[0] = g->speed;
            return 0;
        case GNSS_PROP_COURSE:
            out->v[0] = g->course;
            return 0;
        case GNSS_PROP_HDOP:
            out->v[0] = g->hdop;
            return 0;
        case GNSS_PROP_SATELLITES:
            out->v[0] = g->satellites;
            return 0;
        case GNSS_PROP_FIX:
            out->v[0] = g->fix;
            return 0;
        case GNSS_PROP_POWER:
            out->v[0] = g->on;
            return 0;
        default:
            return -EINVAL;
    }
}

static int gnss_part_set(struct modem *m, size_t property, const struct mclw_value *in) {
    struct gnss *g = m->gnss;
    int64_t v = in->v[0];

    switch (property) {
        case GNSS_PROP_LATITUDE:
            g->latitude = (int32_t)v;
            return 0;
        case GNSS_PROP_LONGITUDE:
            g->longitude = (int32_t)v;
            return 0;
        case GNSS_PROP_ALTITUDE:
            g->altitude = (int32_t)v;
            return 0;
        case GNSS_PROP_SPEED:
            g->speed = (uint32_t)v;
            return 0;
        case GNSS_PROP_COURSE:
            g->course = (uint16_t)v;
            return 0;
        case GNSS_PROP_HDOP:
            g->hdop = (uint16_t)v;
            return 0;
        case GNSS_PROP_SATELLITES:
            g->satellites = (uint8_t)v;
            return 0;
        case GNSS_PROP_FIX:
            g->fix = v != 0;
            return 0;
        default:
            return -EINVAL;
    }
}

const struct modem_part gnss_part = {
    .property_count = GNSS_PROPERTY_COUNT,
    .region_count = 0,
    .defaults = gnss_part_defaults,
    .clear = NULL,
    .session = gnss_part_session,
    .get = gnss_part_get,
    .set = gnss_part_set,
    .region = NULL,
    .written = NULL,
};
