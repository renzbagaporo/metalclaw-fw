// SPDX-License-Identifier: Apache-2.0 OR MIT
// Just enough JSON for the bridge's one-line messages: a parse into a
// fixed pool of nodes, and lookups by key. No allocation.

#ifndef MCLW_PORT_HOST_JSON_H
#define MCLW_PORT_HOST_JSON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum json_kind {
    JSON_NULL,
    JSON_BOOL,
    JSON_NUMBER,
    JSON_STRING,
    JSON_ARRAY,
    JSON_OBJECT,
};

// A latched request: 512 samples and its other fields.
#define JSON_NODES_MAX 1024u

struct json_node {
    uint8_t kind;
    // For a member of an object, its key (not NUL-terminated).
    const char *key;
    uint16_t key_len;
    // JSON_STRING: the characters between the quotes, escapes kept.
    const char *str;
    uint16_t str_len;
    int64_t number;
    bool boolean;
    // JSON_ARRAY, JSON_OBJECT: the first child; every node: its next sibling.
    int16_t child;
    int16_t next;
};

struct json_doc {
    struct json_node nodes[JSON_NODES_MAX];
    uint16_t count;
};

// The root is node 0; false on malformed input or too many nodes.
// Numbers are integers; a fraction or exponent fails.
bool json_parse(struct json_doc *doc, const char *text, size_t len);
// The member `key` of object node `obj`, or NULL.
const struct json_node *json_get(const struct json_doc *doc, const struct json_node *obj, const char *key);
const struct json_node *json_child(const struct json_doc *doc, const struct json_node *node);
const struct json_node *json_next(const struct json_doc *doc, const struct json_node *node);
bool json_is(const struct json_node *node, const char *str);
// `fallback` when the member is missing or of another kind.
int64_t json_int(const struct json_doc *doc, const struct json_node *obj, const char *key, int64_t fallback);
bool json_bool(const struct json_doc *doc, const struct json_node *obj, const char *key, bool fallback);

#endif
