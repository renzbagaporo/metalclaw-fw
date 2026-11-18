// SPDX-License-Identifier: Apache-2.0 OR MIT

#include <string.h>

#include "json.h"

struct parser {
    struct json_doc *doc;
    const char *p;
    const char *end;
};

static void skip_space(struct parser *ps) {
    while (ps->p < ps->end && (*ps->p == ' ' || *ps->p == '\t' || *ps->p == '\r' || *ps->p == '\n')) {
        ps->p++;
    }
}

static int new_node(struct parser *ps, uint8_t kind) {
    if (ps->doc->count >= JSON_NODES_MAX) {
        return -1;
    }
    int index = ps->doc->count++;
    struct json_node *n = &ps->doc->nodes[index];

    memset(n, 0, sizeof(*n));
    n->kind = kind;
    n->child = -1;
    n->next = -1;
    return index;
}

static bool parse_string(struct parser *ps, const char **out, uint16_t *len) {
    const char *start;

    if (ps->p >= ps->end || *ps->p != '"') {
        return false;
    }
    start = ++ps->p;
    while (ps->p < ps->end && *ps->p != '"') {
        if (*ps->p == '\\') {
            ps->p++;
        }
        ps->p++;
    }
    if (ps->p >= ps->end) {
        return false;
    }
    *out = start;
    *len = (uint16_t)(ps->p - start);
    ps->p++;
    return true;
}

static int parse_value(struct parser *ps);

static bool parse_members(struct parser *ps, int parent, bool object) {
    struct json_node *nodes = ps->doc->nodes;
    char close = object ? '}' : ']';
    int last = -1;

    ps->p++;
    skip_space(ps);
    if (ps->p < ps->end && *ps->p == close) {
        ps->p++;
        return true;
    }
    for (;;) {
        const char *key = NULL;
        uint16_t key_len = 0;
        int child;

        skip_space(ps);
        if (object) {
            if (!parse_string(ps, &key, &key_len)) {
                return false;
            }
            skip_space(ps);
            if (ps->p >= ps->end || *ps->p != ':') {
                return false;
            }
            ps->p++;
        }
        child = parse_value(ps);
        if (child < 0) {
            return false;
        }
        nodes[child].key = key;
        nodes[child].key_len = key_len;
        if (last < 0) {
            nodes[parent].child = (int16_t)child;
        } else {
            nodes[last].next = (int16_t)child;
        }
        last = child;
        skip_space(ps);
        if (ps->p < ps->end && *ps->p == ',') {
            ps->p++;
            continue;
        }
        if (ps->p < ps->end && *ps->p == close) {
            ps->p++;
            return true;
        }
        return false;
    }
}

static bool literal(struct parser *ps, const char *word) {
    size_t n = strlen(word);

    if ((size_t)(ps->end - ps->p) < n || memcmp(ps->p, word, n) != 0) {
        return false;
    }
    ps->p += n;
    return true;
}

static int parse_value(struct parser *ps) {
    int index;

    skip_space(ps);
    if (ps->p >= ps->end) {
        return -1;
    }
    switch (*ps->p) {
        case '{':
        case '[': {
            bool object = *ps->p == '{';

            index = new_node(ps, object ? JSON_OBJECT : JSON_ARRAY);
            if (index < 0 || !parse_members(ps, index, object)) {
                return -1;
            }
            return index;
        }
        case '"':
            index = new_node(ps, JSON_STRING);
            if (index < 0 || !parse_string(ps, &ps->doc->nodes[index].str, &ps->doc->nodes[index].str_len)) {
                return -1;
            }
            return index;
        case 't':
        case 'f':
            index = new_node(ps, JSON_BOOL);
            if (index < 0) {
                return -1;
            }
            ps->doc->nodes[index].boolean = *ps->p == 't';
            return literal(ps, *ps->p == 't' ? "true" : "false") ? index : -1;
        case 'n':
            index = new_node(ps, JSON_NULL);
            return index >= 0 && literal(ps, "null") ? index : -1;
        default: {
            bool negative = false;
            int64_t value = 0;
            bool digits = false;

            index = new_node(ps, JSON_NUMBER);
            if (index < 0) {
                return -1;
            }
            if (*ps->p == '-') {
                negative = true;
                ps->p++;
            }
            while (ps->p < ps->end && *ps->p >= '0' && *ps->p <= '9') {
                value = value * 10 + (*ps->p - '0');
                ps->p++;
                digits = true;
            }
            if (!digits || (ps->p < ps->end && (*ps->p == '.' || *ps->p == 'e' || *ps->p == 'E'))) {
                return -1;
            }
            ps->doc->nodes[index].number = negative ? -value : value;
            return index;
        }
    }
}

bool json_parse(struct json_doc *doc, const char *text, size_t len) {
    struct parser ps = { .doc = doc, .p = text, .end = text + len };

    doc->count = 0;
    if (parse_value(&ps) != 0) {
        return false;
    }
    skip_space(&ps);
    return ps.p == ps.end;
}

const struct json_node *json_child(const struct json_doc *doc, const struct json_node *node) {
    return node != NULL && node->child >= 0 ? &doc->nodes[node->child] : NULL;
}

const struct json_node *json_next(const struct json_doc *doc, const struct json_node *node) {
    return node != NULL && node->next >= 0 ? &doc->nodes[node->next] : NULL;
}

const struct json_node *json_get(const struct json_doc *doc, const struct json_node *obj, const char *key) {
    size_t n = strlen(key);

    if (obj == NULL || obj->kind != JSON_OBJECT) {
        return NULL;
    }
    for (const struct json_node *c = json_child(doc, obj); c != NULL; c = json_next(doc, c)) {
        if (c->key_len == n && memcmp(c->key, key, n) == 0) {
            return c;
        }
    }
    return NULL;
}

bool json_is(const struct json_node *node, const char *str) {
    size_t n = strlen(str);

    return node != NULL && node->kind == JSON_STRING && node->str_len == n && memcmp(node->str, str, n) == 0;
}

int64_t json_int(const struct json_doc *doc, const struct json_node *obj, const char *key, int64_t fallback) {
    const struct json_node *n = json_get(doc, obj, key);

    return n != NULL && n->kind == JSON_NUMBER ? n->number : fallback;
}

bool json_bool(const struct json_doc *doc, const struct json_node *obj, const char *key, bool fallback) {
    const struct json_node *n = json_get(doc, obj, key);

    return n != NULL && n->kind == JSON_BOOL ? n->boolean : fallback;
}
