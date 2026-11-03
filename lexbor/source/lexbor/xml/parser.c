/*
 * XML module for the Lexbor engine.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Single-pass XML 1.0 parser. The scanner walks the input buffer directly and
 * builds the DOM tree via the generic lxb_dom_* constructors; there is no
 * intermediate token list. Strict mode aborts on the first well-formedness
 * violation with position info; recover mode records the violation and
 * applies a documented repair rule instead.
 */

#include <string.h>

#include "lexbor/xml/parser.h"

#include "lexbor/dom/interfaces/attr.h"
#include "lexbor/dom/interfaces/cdata_section.h"
#include "lexbor/dom/interfaces/comment.h"
#include "lexbor/dom/interfaces/document_type.h"
#include "lexbor/dom/interfaces/element.h"
#include "lexbor/dom/interfaces/node.h"
#include "lexbor/dom/interfaces/processing_instruction.h"
#include "lexbor/dom/interfaces/text.h"

#define LXB_XML_IS_S(c)                                                        \
    ((c) == 0x20 || (c) == 0x09 || (c) == 0x0A || (c) == 0x0D)

#define LXB_XML_IS_DIGIT(c) ((c) >= 0x30 && (c) <= 0x39)

#define LXB_XML_IS_ALPHA(c)                                                    \
    (((c) >= 0x41 && (c) <= 0x5A) || ((c) >= 0x61 && (c) <= 0x7A))

/*
 * Approximation of the XML 1.0 NameStartChar/NameChar productions: ASCII
 * letters, '_' and ':' start a name; digits, '.', '-' join as continuations.
 * Any byte >= 0x80 is accepted in both positions, which is permissive about
 * multi-byte UTF-8 sequences but never rejects a valid name.
 */
#define LXB_XML_IS_NAME_START(c)                                               \
    (LXB_XML_IS_ALPHA(c) || (c) == '_' || (c) == ':' || (c) >= 0x80)

#define LXB_XML_IS_NAME_CHAR(c)                                                \
    (LXB_XML_IS_ALPHA(c) || LXB_XML_IS_DIGIT(c) || (c) == '_' || (c) == ':'    \
     || (c) == '.' || (c) == '-' || (c) >= 0x80)

#define LXB_XML_NS_XML    "http://www.w3.org/XML/1998/namespace"
#define LXB_XML_NS_XMLNS  "http://www.w3.org/2000/xmlns/"


typedef struct {
    const lxb_char_t *prefix;
    size_t            prefix_len;
    const lxb_char_t *uri;
    size_t            uri_len;
    size_t            depth;      /* open-element depth at which bound */
}
lxb_xml_ns_binding_t;

typedef struct {
    lxb_xml_document_t *document;
    lxb_dom_document_t *doc;

    const lxb_char_t   *data;
    const lxb_char_t   *pos;
    const lxb_char_t   *end;

    lexbor_array_t      open;      /* lxb_dom_node_t * element stack */
    lexbor_array_obj_t  ns;        /* lxb_xml_ns_binding_t bindings stack */
    lexbor_str_t        text;      /* scratch buffer on doc->mraw */

    size_t              floor;     /* minimum element-stack depth (fragments) */

    bool                inited;
    bool                saw_root;
    bool                recover;
}
lxb_xml_ctx_t;


static lxb_status_t
lxb_xml_ctx_init(lxb_xml_ctx_t *ctx, lxb_xml_document_t *document,
                 lxb_dom_document_t *doc, const lxb_char_t *data, size_t size)
{
    memset(ctx, 0, sizeof(lxb_xml_ctx_t));

    ctx->document = document;
    ctx->doc = doc;
    ctx->data = data;
    ctx->pos = data;
    ctx->end = data + size;
    ctx->recover = document != NULL && document->recover;

    if (lexbor_array_init(&ctx->open, 32) != LXB_STATUS_OK) {
        return LXB_STATUS_ERROR_MEMORY_ALLOCATION;
    }

    if (lexbor_array_obj_init(&ctx->ns, 16, sizeof(lxb_xml_ns_binding_t))
        != LXB_STATUS_OK)
    {
        lexbor_array_destroy(&ctx->open, false);
        return LXB_STATUS_ERROR_MEMORY_ALLOCATION;
    }

    if (lexbor_str_init(&ctx->text, doc->mraw, 1024) == NULL) {
        lexbor_array_obj_destroy(&ctx->ns, false);
        lexbor_array_destroy(&ctx->open, false);
        return LXB_STATUS_ERROR_MEMORY_ALLOCATION;
    }

    ctx->inited = true;
    return LXB_STATUS_OK;
}

static void
lxb_xml_ctx_destroy(lxb_xml_ctx_t *ctx)
{
    if (!ctx->inited) {
        return;
    }

    lexbor_str_destroy(&ctx->text, ctx->doc->mraw, false);
    lexbor_array_obj_destroy(&ctx->ns, false);
    lexbor_array_destroy(&ctx->open, false);
}


/* ------------------------------------------------------------------------- */
/* Error reporting                                                           */
/* ------------------------------------------------------------------------- */

static void
lxb_xml_position(const lxb_xml_ctx_t *ctx, const lxb_char_t *at,
                 size_t *line, size_t *col)
{
    const lxb_char_t *p;

    *line = 1;
    *col = (size_t) (at - ctx->data) + 1;

    for (p = ctx->data; p < at; p++) {
        if (*p == 0x0A) {
            (*line)++;
            *col = (size_t) (at - p);
        }
        else if (*p == 0x0D && (p + 1 >= at || p[1] != 0x0A)) {
            (*line)++;
            *col = (size_t) (at - p);
        }
    }
}

/*
 * Record an error at `at`. In strict mode fills document->first_error and
 * returns LXB_STATUS_ERROR so the parse unwinds. In recover mode appends to
 * document->errors and returns LXB_STATUS_OK; the caller must still apply
 * its repair rule.
 */
static lxb_status_t
lxb_xml_error(lxb_xml_ctx_t *ctx, lxb_xml_error_id_t id, const lxb_char_t *at)
{
    lxb_xml_parse_error_t err, *slot;

    err.id = id;
    err.offset = (size_t) (at - ctx->data);
    lxb_xml_position(ctx, at, &err.line, &err.col);

    if (ctx->recover && ctx->document != NULL
        && ctx->document->errors != NULL)
    {
        slot = lexbor_array_obj_push(ctx->document->errors);
        if (slot != NULL) {
            *slot = err;
        }
        return LXB_STATUS_OK;
    }

    if (ctx->document != NULL) {
        ctx->document->first_error = err;
    }

    return LXB_STATUS_ERROR;
}


/* ------------------------------------------------------------------------- */
/* Scanning helpers                                                          */
/* ------------------------------------------------------------------------- */

static bool
lxb_xml_eof(const lxb_xml_ctx_t *ctx)
{
    return ctx->pos >= ctx->end;
}

static void
lxb_xml_skip_s(lxb_xml_ctx_t *ctx)
{
    while (ctx->pos < ctx->end && LXB_XML_IS_S(*ctx->pos)) {
        ctx->pos++;
    }
}

static bool
lxb_xml_starts_with(const lxb_xml_ctx_t *ctx, const char *lit, size_t len)
{
    return (size_t) (ctx->end - ctx->pos) >= len
           && memcmp(ctx->pos, lit, len) == 0;
}

/*
 * Scan a Name token. Returns the name span; NULL when the first byte cannot
 * start a name.
 */
static const lxb_char_t *
lxb_xml_scan_name(lxb_xml_ctx_t *ctx, size_t *name_len)
{
    const lxb_char_t *name = ctx->pos;

    if (ctx->pos >= ctx->end || !LXB_XML_IS_NAME_START(*ctx->pos)) {
        return NULL;
    }

    ctx->pos++;

    while (ctx->pos < ctx->end && LXB_XML_IS_NAME_CHAR(*ctx->pos)) {
        ctx->pos++;
    }

    *name_len = (size_t) (ctx->pos - name);
    return name;
}

/*
 * True when "<?xml" at pos is the document declaration, i.e. the next byte
 * is whitespace or '?'. "<?xml-stylesheet" is a processing instruction.
 */
static bool
lxb_xml_is_decl(const lxb_xml_ctx_t *ctx)
{
    if (!lxb_xml_starts_with(ctx, "<?xml", 5)) {
        return false;
    }

    if (ctx->pos + 5 >= ctx->end) {
        return true;
    }

    return LXB_XML_IS_S(ctx->pos[5]) || ctx->pos[5] == '?';
}


/* ------------------------------------------------------------------------- */
/* Namespace scope                                                           */
/* ------------------------------------------------------------------------- */

/*
 * Record an xmlns/xmlns:p declaration as an in-scope binding. `prefix`/`uri`
 * point into stable storage (attr values live in doc->text, which is freed
 * with the document), so bindings borrow the pointers.
 */
static lxb_status_t
lxb_xml_ns_push(lxb_xml_ctx_t *ctx, const lxb_char_t *prefix, size_t plen,
                const lxb_char_t *uri, size_t ulen)
{
    lxb_xml_ns_binding_t *bind;

    bind = lexbor_array_obj_push(&ctx->ns);
    if (bind == NULL) {
        return LXB_STATUS_ERROR_MEMORY_ALLOCATION;
    }

    bind->prefix = prefix;
    bind->prefix_len = plen;
    bind->uri = uri;
    bind->uri_len = ulen;
    bind->depth = lexbor_array_length(&ctx->open);

    return LXB_STATUS_OK;
}

/*
 * Pop every binding declared at `depth` or deeper. Called after an element at
 * stack index `depth` closes (open.length has already been reduced).
 */
static void
lxb_xml_ns_pop_to(lxb_xml_ctx_t *ctx, size_t depth)
{
    lxb_xml_ns_binding_t *bind;

    while (ctx->ns.length > 0) {
        bind = lexbor_array_obj_get(&ctx->ns, ctx->ns.length - 1);
        if (bind->depth < depth) {
            break;
        }

        lexbor_array_obj_pop(&ctx->ns);
    }
}

/*
 * Resolve a namespace prefix to a URI span, innermost scope first.
 * `prefix == NULL` asks for the default element namespace. Returns NULL when
 * unbound.
 */
static const lxb_char_t *
lxb_xml_ns_resolve(lxb_xml_ctx_t *ctx, const lxb_char_t *prefix, size_t plen,
                   size_t *uri_len)
{
    lxb_xml_ns_binding_t *bind;
    size_t i;

    if (prefix != NULL) {
        if (plen == 3 && memcmp(prefix, "xml", 3) == 0) {
            *uri_len = sizeof(LXB_XML_NS_XML) - 1;
            return (const lxb_char_t *) LXB_XML_NS_XML;
        }

        if (plen == 5 && memcmp(prefix, "xmlns", 5) == 0) {
            return NULL;
        }
    }

    for (i = ctx->ns.length; i > 0; i--) {
        bind = lexbor_array_obj_get(&ctx->ns, i - 1);

        if (prefix == NULL) {
            if (bind->prefix_len == 0) {
                *uri_len = bind->uri_len;
                return bind->uri;
            }
        }
        else if (bind->prefix_len == plen
                 && memcmp(bind->prefix, prefix, plen) == 0)
        {
            *uri_len = bind->uri_len;
            return bind->uri;
        }
    }

    return NULL;
}


/* ------------------------------------------------------------------------- */
/* Text: entity references and decoding                                      */
/* ------------------------------------------------------------------------- */

static lxb_status_t
lxb_xml_text_put(lxb_xml_ctx_t *ctx, const lxb_char_t *data, size_t len)
{
    return (lexbor_str_append(&ctx->text, ctx->doc->mraw, data, len) == NULL)
           ? LXB_STATUS_ERROR_MEMORY_ALLOCATION : LXB_STATUS_OK;
}

static lxb_status_t
lxb_xml_text_put_char(lxb_xml_ctx_t *ctx, lxb_char_t c)
{
    return (lexbor_str_append_one(&ctx->text, ctx->doc->mraw, c) == NULL)
           ? LXB_STATUS_ERROR_MEMORY_ALLOCATION : LXB_STATUS_OK;
}

static lxb_status_t
lxb_xml_text_put_cp(lxb_xml_ctx_t *ctx, uint32_t cp)
{
    uint8_t out[4];
    size_t len;

    /* XML 1.0 Char production. */
    if (!(cp == 0x09 || cp == 0x0A || cp == 0x0D
          || (cp >= 0x20 && cp <= 0xD7FF)
          || (cp >= 0xE000 && cp <= 0xFFFD)
          || (cp >= 0x10000 && cp <= 0x10FFFF)))
    {
        return lxb_xml_error(ctx, LXB_XML_ERROR_INVALID_CHARREF, ctx->pos);
    }

    if (cp < 0x80) {
        out[0] = (uint8_t) cp;
        len = 1;
    }
    else if (cp < 0x800) {
        out[0] = 0xC0 | (uint8_t) (cp >> 6);
        out[1] = 0x80 | (uint8_t) (cp & 0x3F);
        len = 2;
    }
    else if (cp < 0x10000) {
        out[0] = 0xE0 | (uint8_t) (cp >> 12);
        out[1] = 0x80 | (uint8_t) ((cp >> 6) & 0x3F);
        out[2] = 0x80 | (uint8_t) (cp & 0x3F);
        len = 3;
    }
    else {
        out[0] = 0xF0 | (uint8_t) (cp >> 18);
        out[1] = 0x80 | (uint8_t) ((cp >> 12) & 0x3F);
        out[2] = 0x80 | (uint8_t) ((cp >> 6) & 0x3F);
        out[3] = 0x80 | (uint8_t) (cp & 0x3F);
        len = 4;
    }

    return lxb_xml_text_put(ctx, out, len);
}

/*
 * Decode `&name;` / `&#N;` / `&#xN;` at ctx->pos (pointing at '&') into the
 * text buffer. ctx->pos is moved past the ';'.
 */
static lxb_status_t
lxb_xml_reference(lxb_xml_ctx_t *ctx)
{
    const lxb_char_t *ref_start = ctx->pos;
    const lxb_char_t *name;
    uint32_t cp;
    size_t len;
    bool hex;

    ctx->pos++; /* '&' */

    if (ctx->pos < ctx->end && *ctx->pos == '#') {
        ctx->pos++;
        cp = 0;
        hex = false;

        if (ctx->pos < ctx->end && (*ctx->pos == 'x' || *ctx->pos == 'X')) {
            hex = true;
            ctx->pos++;
        }

        if (ctx->pos >= ctx->end
            || !(LXB_XML_IS_DIGIT(*ctx->pos)
                 || (hex && ((*ctx->pos >= 'a' && *ctx->pos <= 'f')
                             || (*ctx->pos >= 'A' && *ctx->pos <= 'F')))))
        {
            if (lxb_xml_error(ctx, LXB_XML_ERROR_INVALID_CHARREF, ref_start)
                != LXB_STATUS_OK)
            {
                return LXB_STATUS_ERROR;
            }

            ctx->pos = ref_start + 1;
            return lxb_xml_text_put_char(ctx, '&');
        }

        while (ctx->pos < ctx->end && *ctx->pos != ';') {
            lxb_char_t c = *ctx->pos;
            uint32_t d, base = hex ? 16 : 10;

            if (LXB_XML_IS_DIGIT(c)) {
                d = (uint32_t) (c - '0');
            }
            else if (hex && c >= 'a' && c <= 'f') {
                d = (uint32_t) (c - 'a' + 10);
            }
            else if (hex && c >= 'A' && c <= 'F') {
                d = (uint32_t) (c - 'A' + 10);
            }
            else {
                break;
            }

            if (cp > (0x10FFFF - d) / base) {
                cp = 0x110000;
                while (ctx->pos < ctx->end && *ctx->pos != ';') {
                    ctx->pos++;
                }
                break;
            }

            cp = cp * base + d;
            ctx->pos++;
        }

        if (ctx->pos >= ctx->end || *ctx->pos != ';') {
            if (lxb_xml_error(ctx, LXB_XML_ERROR_INVALID_CHARREF, ref_start)
                != LXB_STATUS_OK)
            {
                return LXB_STATUS_ERROR;
            }

            ctx->pos = ref_start + 1;
            return lxb_xml_text_put_char(ctx, '&');
        }

        ctx->pos++; /* ';' */

        return lxb_xml_text_put_cp(ctx, cp);
    }

    name = lxb_xml_scan_name(ctx, &len);

    if (name == NULL || ctx->pos >= ctx->end || *ctx->pos != ';') {
        if (lxb_xml_error(ctx, LXB_XML_ERROR_INVALID_ENTITY, ref_start)
            != LXB_STATUS_OK)
        {
            return LXB_STATUS_ERROR;
        }

        ctx->pos = ref_start + 1;
        return lxb_xml_text_put_char(ctx, '&');
    }

    ctx->pos++; /* ';' */

    switch (len) {
        case 2:
            if (memcmp(name, "lt", 2) == 0) {
                return lxb_xml_text_put_char(ctx, '<');
            }
            if (memcmp(name, "gt", 2) == 0) {
                return lxb_xml_text_put_char(ctx, '>');
            }
            break;

        case 3:
            if (memcmp(name, "amp", 3) == 0) {
                return lxb_xml_text_put_char(ctx, '&');
            }
            break;

        case 4:
            if (memcmp(name, "apos", 4) == 0) {
                return lxb_xml_text_put_char(ctx, '\'');
            }
            if (memcmp(name, "quot", 4) == 0) {
                return lxb_xml_text_put_char(ctx, '"');
            }
            break;
    }

    if (lxb_xml_error(ctx, LXB_XML_ERROR_INVALID_ENTITY, ref_start)
        != LXB_STATUS_OK)
    {
        return LXB_STATUS_ERROR;
    }

    /* recover: emit the reference verbatim */
    return lxb_xml_text_put(ctx, ref_start,
                            (size_t) (ctx->pos - ref_start));
}


/* ------------------------------------------------------------------------- */
/* Node constructors                                                         */
/* ------------------------------------------------------------------------- */

static lxb_dom_node_t *
lxb_xml_parent(lxb_xml_ctx_t *ctx)
{
    if (ctx->open.length != 0) {
        return lexbor_array_get(&ctx->open, ctx->open.length - 1);
    }

    return lxb_dom_interface_node(ctx->doc);
}

static bool
lxb_xml_is_ws_only(const lxb_char_t *data, size_t len)
{
    size_t i;

    for (i = 0; i < len; i++) {
        if (!LXB_XML_IS_S(data[i])) {
            return false;
        }
    }

    return true;
}

static lxb_status_t
lxb_xml_text_flush(lxb_xml_ctx_t *ctx)
{
    lxb_dom_text_t *text;

    if (ctx->text.length == 0) {
        return LXB_STATUS_OK;
    }

    if (ctx->open.length == 0) {
        /* Character data outside the root element. Whitespace is allowed
         * between prolog/epilog constructs; anything else is an error.
         * (Fragment mode never reaches this: the seeded element keeps the
         * stack at >= 1.) */
        if (!lxb_xml_is_ws_only(ctx->text.data, ctx->text.length)) {
            if (lxb_xml_error(ctx, LXB_XML_ERROR_UNEXPECTED_TEXT, ctx->pos)
                != LXB_STATUS_OK)
            {
                return LXB_STATUS_ERROR;
            }
        }

        ctx->text.length = 0;
        return LXB_STATUS_OK;
    }

    text = lxb_dom_document_create_text_node(ctx->doc, ctx->text.data,
                                             ctx->text.length);
    if (text == NULL) {
        return LXB_STATUS_ERROR_MEMORY_ALLOCATION;
    }

    lxb_dom_node_insert_child(lxb_xml_parent(ctx),
                              lxb_dom_interface_node(text));

    ctx->text.length = 0;
    return LXB_STATUS_OK;
}

static lxb_status_t
lxb_xml_comment(lxb_xml_ctx_t *ctx)
{
    const lxb_char_t *body;
    const lxb_char_t *p;
    lxb_dom_comment_t *comment;

    ctx->pos += 4; /* "<!--" */
    body = ctx->pos;

    for (p = ctx->pos; p + 1 < ctx->end; p++) {
        if (p[0] == '-' && p[1] == '-') {
            if (p + 2 < ctx->end && p[2] == '>') {
                goto closed;
            }

            /* "--" inside a comment body. */
            if (lxb_xml_error(ctx, LXB_XML_ERROR_BAD_COMMENT, p)
                != LXB_STATUS_OK)
            {
                return LXB_STATUS_ERROR;
            }
        }
    }

    /* EOF inside a comment. */
    if (lxb_xml_error(ctx, LXB_XML_ERROR_UNEXPECTED_EOF, ctx->end)
        != LXB_STATUS_OK)
    {
        return LXB_STATUS_ERROR;
    }

    ctx->pos = ctx->end;
    return LXB_STATUS_OK;

closed:
    comment = lxb_dom_document_create_comment(ctx->doc, body,
                                              (size_t) (p - body));
    if (comment == NULL) {
        return LXB_STATUS_ERROR_MEMORY_ALLOCATION;
    }

    lxb_dom_node_insert_child(lxb_xml_parent(ctx),
                              lxb_dom_interface_node(comment));

    ctx->pos = p + 3;
    return LXB_STATUS_OK;
}

static lxb_status_t
lxb_xml_pi(lxb_xml_ctx_t *ctx)
{
    const lxb_char_t *target, *body;
    size_t target_len, body_len;
    const lxb_char_t *p;
    lxb_dom_processing_instruction_t *pi;

    ctx->pos += 2; /* "<?" */

    target = lxb_xml_scan_name(ctx, &target_len);
    if (target == NULL) {
        if (lxb_xml_error(ctx, LXB_XML_ERROR_BAD_PI, ctx->pos)
            != LXB_STATUS_OK)
        {
            return LXB_STATUS_ERROR;
        }

        /* recover: skip to "?>" or EOF */
        while (ctx->pos < ctx->end && !lxb_xml_starts_with(ctx, "?>", 2)) {
            ctx->pos++;
        }
        if (ctx->pos < ctx->end) {
            ctx->pos += 2;
        }
        return LXB_STATUS_OK;
    }

    if (target_len == 3 && (target[0] | 0x20) == 'x'
        && (target[1] | 0x20) == 'm' && (target[2] | 0x20) == 'l')
    {
        /* Target "xml" is reserved for the prolog declaration. */
        if (lxb_xml_error(ctx, LXB_XML_ERROR_BAD_PI, target)
            != LXB_STATUS_OK)
        {
            return LXB_STATUS_ERROR;
        }
    }

    if (ctx->pos < ctx->end && LXB_XML_IS_S(*ctx->pos)) {
        lxb_xml_skip_s(ctx);
    }
    body = ctx->pos;

    for (p = ctx->pos; p + 1 < ctx->end; p++) {
        if (p[0] == '?' && p[1] == '>') {
            body_len = (size_t) (p - body);
            ctx->pos = p + 2;
            goto done;
        }
    }

    /* Unterminated PI. */
    if (lxb_xml_error(ctx, LXB_XML_ERROR_UNEXPECTED_EOF, ctx->end)
        != LXB_STATUS_OK)
    {
        return LXB_STATUS_ERROR;
    }

    body_len = (size_t) (ctx->end - body);
    ctx->pos = ctx->end;

done:
    pi = lxb_dom_document_create_processing_instruction(
        ctx->doc, target, target_len, body, body_len);
    if (pi == NULL) {
        return LXB_STATUS_ERROR_MEMORY_ALLOCATION;
    }

    lxb_dom_node_insert_child(lxb_xml_parent(ctx),
                              lxb_dom_interface_node(pi));
    return LXB_STATUS_OK;
}

static lxb_status_t
lxb_xml_cdata(lxb_xml_ctx_t *ctx)
{
    const lxb_char_t *body;
    const lxb_char_t *p;
    lxb_dom_cdata_section_t *section;

    if (ctx->open.length == 0) {
        /* CDATA is only allowed inside an element. */
        if (lxb_xml_error(ctx, LXB_XML_ERROR_BAD_CDATA, ctx->pos)
            != LXB_STATUS_OK)
        {
            return LXB_STATUS_ERROR;
        }
    }

    ctx->pos += 9; /* "<![CDATA[" */
    body = ctx->pos;

    for (p = ctx->pos; p + 2 < ctx->end; p++) {
        if (p[0] == ']' && p[1] == ']' && p[2] == '>') {
            section = lxb_dom_document_create_cdata_section(
                ctx->doc, body, (size_t) (p - body));
            if (section == NULL) {
                return LXB_STATUS_ERROR_MEMORY_ALLOCATION;
            }

            lxb_dom_node_insert_child(lxb_xml_parent(ctx),
                                      lxb_dom_interface_node(section));

            ctx->pos = p + 3;
            return LXB_STATUS_OK;
        }
    }

    if (lxb_xml_error(ctx, LXB_XML_ERROR_UNEXPECTED_EOF, ctx->end)
        != LXB_STATUS_OK)
    {
        return LXB_STATUS_ERROR;
    }

    /* recover: emit the remainder as the section body */
    section = lxb_dom_document_create_cdata_section(
        ctx->doc, body, (size_t) (ctx->end - body));
    if (section == NULL) {
        return LXB_STATUS_ERROR_MEMORY_ALLOCATION;
    }

    lxb_dom_node_insert_child(lxb_xml_parent(ctx),
                              lxb_dom_interface_node(section));
    ctx->pos = ctx->end;
    return LXB_STATUS_OK;
}


/* ------------------------------------------------------------------------- */
/* DOCTYPE                                                                   */
/* ------------------------------------------------------------------------- */

static const lxb_char_t *
lxb_xml_doctype_quoted(lxb_xml_ctx_t *ctx, size_t *out_len)
{
    const lxb_char_t *value;
    lxb_char_t quote;

    if (ctx->pos >= ctx->end
        || (*ctx->pos != '"' && *ctx->pos != '\''))
    {
        return NULL;
    }

    quote = *ctx->pos++;
    value = ctx->pos;

    while (ctx->pos < ctx->end && *ctx->pos != quote) {
        ctx->pos++;
    }

    *out_len = (size_t) (ctx->pos - value);

    if (ctx->pos < ctx->end) {
        ctx->pos++; /* closing quote */
    }

    return value;
}

static lxb_status_t
lxb_xml_doctype(lxb_xml_ctx_t *ctx)
{
    const lxb_char_t *name = NULL, *pub = NULL, *sys = NULL;
    size_t name_len = 0, pub_len = 0, sys_len = 0;
    const lxb_char_t *p;
    lxb_dom_document_type_t *dtype;
    const lxb_dom_attr_data_t *name_data;
    lxb_char_t quote;

    /* pos points past "<!DOCTYPE". */
    lxb_xml_skip_s(ctx);

    name = lxb_xml_scan_name(ctx, &name_len);
    if (name == NULL) {
        if (lxb_xml_error(ctx, LXB_XML_ERROR_BAD_DOCTYPE, ctx->pos)
            != LXB_STATUS_OK)
        {
            return LXB_STATUS_ERROR;
        }
        goto skip_subset;
    }

    lxb_xml_skip_s(ctx);

    /* External ID: SYSTEM "sys" | PUBLIC "pub" "sys" */
    if (lxb_xml_starts_with(ctx, "SYSTEM", 6)
        && ctx->pos + 6 < ctx->end && LXB_XML_IS_S(ctx->pos[6]))
    {
        ctx->pos += 6;
        lxb_xml_skip_s(ctx);

        sys = lxb_xml_doctype_quoted(ctx, &sys_len);
        if (sys == NULL) {
            if (lxb_xml_error(ctx, LXB_XML_ERROR_BAD_DOCTYPE, ctx->pos)
                != LXB_STATUS_OK)
            {
                return LXB_STATUS_ERROR;
            }
        }
    }
    else if (lxb_xml_starts_with(ctx, "PUBLIC", 6)
             && ctx->pos + 6 < ctx->end && LXB_XML_IS_S(ctx->pos[6]))
    {
        ctx->pos += 6;
        lxb_xml_skip_s(ctx);

        pub = lxb_xml_doctype_quoted(ctx, &pub_len);
        if (pub == NULL) {
            if (lxb_xml_error(ctx, LXB_XML_ERROR_BAD_DOCTYPE, ctx->pos)
                != LXB_STATUS_OK)
            {
                return LXB_STATUS_ERROR;
            }
        }

        lxb_xml_skip_s(ctx);

        if (ctx->pos < ctx->end
            && (*ctx->pos == '"' || *ctx->pos == '\''))
        {
            sys = lxb_xml_doctype_quoted(ctx, &sys_len);
        }
    }

    lxb_xml_skip_s(ctx);

skip_subset:

    /*
     * Internal subset "[ ... ]": skipped, not processed. DTD entity
     * declarations are intentionally unsupported (XXE-safe by design).
     * Quoted sections and comments inside the subset are respected so a ']'
     * or '>' inside them does not terminate the scan early.
     */
    if (ctx->pos < ctx->end && *ctx->pos == '[') {
        ctx->pos++;

        for (p = ctx->pos; p < ctx->end; p++) {
            if (*p == '\'' || *p == '"') {
                quote = *p++;
                while (p < ctx->end && *p != quote) {
                    p++;
                }
                continue;
            }

            if (*p == '<' && p + 3 < ctx->end
                && p[1] == '!' && p[2] == '-' && p[3] == '-')
            {
                p += 4;
                while (p + 2 < ctx->end
                       && !(p[0] == '-' && p[1] == '-' && p[2] == '>'))
                {
                    p++;
                }
                p += 2;
                continue;
            }

            if (*p == ']') {
                ctx->pos = p + 1;
                goto subset_done;
            }
        }

        if (lxb_xml_error(ctx, LXB_XML_ERROR_BAD_DOCTYPE, ctx->end)
            != LXB_STATUS_OK)
        {
            return LXB_STATUS_ERROR;
        }

        ctx->pos = ctx->end;
        return LXB_STATUS_OK;

subset_done:
        lxb_xml_skip_s(ctx);
    }

    if (ctx->pos < ctx->end && *ctx->pos == '>') {
        ctx->pos++;
    }
    else if (lxb_xml_error(ctx, LXB_XML_ERROR_BAD_DOCTYPE, ctx->pos)
             != LXB_STATUS_OK)
    {
        return LXB_STATUS_ERROR;
    }

    if (name == NULL) {
        return LXB_STATUS_OK;
    }

    /*
     * Build the doctype node directly: lxb_dom_document_type_create() would
     * lowercase the name, but XML names are case-sensitive.
     */
    dtype = lxb_dom_document_type_interface_create(ctx->doc);
    if (dtype == NULL) {
        return LXB_STATUS_ERROR_MEMORY_ALLOCATION;
    }

    name_data = lxb_dom_attr_qualified_name_append(ctx->doc->attrs,
                                                   name, name_len);
    if (name_data == NULL) {
        return LXB_STATUS_ERROR_MEMORY_ALLOCATION;
    }

    dtype->name = (lxb_dom_attr_id_t) name_data;

    if (pub != NULL && pub_len != 0) {
        dtype->public_id.data = lxb_dom_document_create_text(ctx->doc,
                                                           pub_len + 1);
        if (dtype->public_id.data == NULL) {
            return LXB_STATUS_ERROR_MEMORY_ALLOCATION;
        }
        (void) lexbor_str_copy_to_with_null(&dtype->public_id, pub, pub_len);
    }

    if (sys != NULL && sys_len != 0) {
        dtype->system_id.data = lxb_dom_document_create_text(ctx->doc,
                                                           sys_len + 1);
        if (dtype->system_id.data == NULL) {
            return LXB_STATUS_ERROR_MEMORY_ALLOCATION;
        }
        (void) lexbor_str_copy_to_with_null(&dtype->system_id, sys, sys_len);
    }

    lxb_dom_node_insert_child(lxb_dom_interface_node(ctx->doc),
                              lxb_dom_interface_node(dtype));
    lxb_dom_document_attach_doctype(ctx->doc, dtype);

    return LXB_STATUS_OK;
}


/* ------------------------------------------------------------------------- */
/* Elements                                                                  */
/* ------------------------------------------------------------------------- */

typedef struct {
    const lxb_char_t *name;      /* qualified name, span into input */
    size_t            name_len;
    const lxb_char_t *value;     /* normalized copy in doc->text pool */
    size_t            value_len;
    bool              is_xmlns;
    bool              is_xmlns_prefix;
}
lxb_xml_attr_t;

/*
 * Parse an attribute value: quotes are required (strict), entity references
 * decoded, and tab/LF/CR normalized to spaces. The normalized value lands in
 * ctx->text.
 */
static lxb_status_t
lxb_xml_attr_value(lxb_xml_ctx_t *ctx)
{
    const lxb_char_t *start;
    const lxb_char_t *p;
    lxb_char_t quote;

    lxb_xml_skip_s(ctx);

    if (ctx->pos >= ctx->end
        || (*ctx->pos != '"' && *ctx->pos != '\''))
    {
        if (lxb_xml_error(ctx, LXB_XML_ERROR_UNQUOTED_ATTR, ctx->pos)
            != LXB_STATUS_OK)
        {
            return LXB_STATUS_ERROR;
        }

        /* recover: read to the next delimiter as the value */
        start = ctx->pos;
        while (ctx->pos < ctx->end && !LXB_XML_IS_S(*ctx->pos)
               && *ctx->pos != '>' && *ctx->pos != '/')
        {
            ctx->pos++;
        }
        return lxb_xml_text_put(ctx, start,
                                (size_t) (ctx->pos - start));
    }

    quote = *ctx->pos++;

    while (ctx->pos < ctx->end) {
        p = ctx->pos;

        while (p < ctx->end && *p != quote && *p != '&'
               && *p != '<' && !LXB_XML_IS_S(*p))
        {
            p++;
        }

        if (lxb_xml_text_put(ctx, ctx->pos, (size_t) (p - ctx->pos))
            != LXB_STATUS_OK)
        {
            return LXB_STATUS_ERROR_MEMORY_ALLOCATION;
        }
        ctx->pos = p;

        if (ctx->pos >= ctx->end) {
            break;
        }

        if (*ctx->pos == quote) {
            ctx->pos++;
            return LXB_STATUS_OK;
        }

        if (*ctx->pos == '&') {
            if (lxb_xml_reference(ctx) != LXB_STATUS_OK) {
                return LXB_STATUS_ERROR;
            }
            continue;
        }

        if (*ctx->pos == '<') {
            if (lxb_xml_error(ctx, LXB_XML_ERROR_INVALID_CHAR, ctx->pos)
                != LXB_STATUS_OK)
            {
                return LXB_STATUS_ERROR;
            }
            /* recover: keep it literal */
            if (lxb_xml_text_put_char(ctx, '<') != LXB_STATUS_OK) {
                return LXB_STATUS_ERROR_MEMORY_ALLOCATION;
            }
            ctx->pos++;
            continue;
        }

        /* Whitespace normalization: tab/LF/CR -> space. */
        if (lxb_xml_text_put_char(ctx, 0x20) != LXB_STATUS_OK) {
            return LXB_STATUS_ERROR_MEMORY_ALLOCATION;
        }

        /* CRLF collapses to a single space. */
        if (*ctx->pos == 0x0D && ctx->pos + 1 < ctx->end
            && ctx->pos[1] == 0x0A)
        {
            ctx->pos++;
        }
        ctx->pos++;
    }

    /* Unterminated value. */
    if (lxb_xml_error(ctx, LXB_XML_ERROR_UNEXPECTED_EOF, ctx->end)
        != LXB_STATUS_OK)
    {
        return LXB_STATUS_ERROR;
    }

    ctx->pos = ctx->end;
    return LXB_STATUS_OK;
}

static lxb_status_t
lxb_xml_attr_append(lxb_xml_ctx_t *ctx, lxb_dom_element_t *element,
                    const lxb_char_t *name, size_t name_len,
                    const lxb_char_t *value, size_t value_len)
{
    const lxb_char_t *colon;
    const lxb_char_t *uri;
    size_t uri_len = 0;
    lxb_dom_attr_t *attr;
    lxb_status_t status;

    attr = lxb_dom_attr_interface_create(ctx->doc);
    if (attr == NULL) {
        return LXB_STATUS_ERROR_MEMORY_ALLOCATION;
    }

    colon = (const lxb_char_t *) memchr(name, ':', name_len);

    if (colon != NULL) {
        size_t plen = (size_t) (colon - name);

        if (plen == 5 && memcmp(name, "xmlns", 5) == 0) {
            uri = (const lxb_char_t *) LXB_XML_NS_XMLNS;
            uri_len = sizeof(LXB_XML_NS_XMLNS) - 1;
        }
        else if (plen == 3 && memcmp(name, "xml", 3) == 0) {
            uri = (const lxb_char_t *) LXB_XML_NS_XML;
            uri_len = sizeof(LXB_XML_NS_XML) - 1;
        }
        else {
            uri = lxb_xml_ns_resolve(ctx, name, plen, &uri_len);

            if (uri == NULL) {
                if (lxb_xml_error(ctx, LXB_XML_ERROR_UNBOUND_PREFIX, name)
                    != LXB_STATUS_OK)
                {
                    lxb_dom_attr_interface_destroy(attr);
                    return LXB_STATUS_ERROR;
                }
                /* recover: leave the attribute without a namespace */
            }
        }

        if (uri != NULL) {
            status = lxb_dom_attr_set_name_ns(attr, uri, uri_len,
                                              name, name_len, false);
        }
        else {
            status = lxb_dom_attr_set_name(attr, name, name_len, false);
        }
    }
    else if (name_len == 5 && memcmp(name, "xmlns", 5) == 0) {
        status = lxb_dom_attr_set_name_ns(attr,
                        (const lxb_char_t *) LXB_XML_NS_XMLNS,
                        sizeof(LXB_XML_NS_XMLNS) - 1,
                        name, name_len, false);
    }
    else {
        status = lxb_dom_attr_set_name(attr, name, name_len, false);
    }

    if (status != LXB_STATUS_OK) {
        lxb_dom_attr_interface_destroy(attr);
        return status;
    }

    status = lxb_dom_attr_set_value(attr, value, value_len);
    if (status != LXB_STATUS_OK) {
        lxb_dom_attr_interface_destroy(attr);
        return status;
    }

    lxb_dom_element_attr_append(element, attr);
    return LXB_STATUS_OK;
}

/*
 * Split a qualified name into prefix and local name. No prefix yields
 * prefix_len == 0. Returns false for malformed names ("a:", ":b", "a:b:c").
 */
static bool
lxb_xml_qname_split(const lxb_char_t *name, size_t name_len,
                    const lxb_char_t **prefix, size_t *prefix_len,
                    const lxb_char_t **local, size_t *local_len)
{
    const lxb_char_t *colon =
        (const lxb_char_t *) memchr(name, ':', name_len);

    if (colon == NULL) {
        *prefix = NULL;
        *prefix_len = 0;
        *local = name;
        *local_len = name_len;
        return true;
    }

    *prefix = name;
    *prefix_len = (size_t) (colon - name);
    *local = colon + 1;
    *local_len = name_len - *prefix_len - 1;

    if (*prefix_len == 0 || *local_len == 0
        || memchr(*local, ':', *local_len) != NULL)
    {
        return false;
    }

    return true;
}

/*
 * Check a parsed attr list for duplicates (qualified-name comparison).
 * In recover mode the later duplicate is removed in place.
 */
static lxb_status_t
lxb_xml_attrs_check_dups(lxb_xml_ctx_t *ctx, lxb_xml_attr_t *attrs,
                         size_t *count)
{
    size_t i, j;

    for (i = 0; i < *count; i++) {
        for (j = i + 1; j < *count; j++) {
            if (attrs[i].name_len == attrs[j].name_len
                && memcmp(attrs[i].name, attrs[j].name,
                          attrs[i].name_len) == 0)
            {
                if (lxb_xml_error(ctx, LXB_XML_ERROR_DUPLICATE_ATTR,
                                  attrs[j].name) != LXB_STATUS_OK)
                {
                    return LXB_STATUS_ERROR;
                }

                /* recover: drop the later duplicate */
                memmove(&attrs[j], &attrs[j + 1],
                        sizeof(lxb_xml_attr_t) * (*count - j - 1));
                (*count)--;
                j--;
            }
        }
    }

    return LXB_STATUS_OK;
}

static lxb_status_t
lxb_xml_start_tag(lxb_xml_ctx_t *ctx)
{
    const lxb_char_t *name, *prefix, *local;
    size_t name_len, prefix_len, local_len, uri_len = 0;
    const lxb_char_t *uri;
    const lxb_char_t *tag_at = ctx->pos;
    lxb_dom_element_t *element;
    lxb_status_t status;
    bool empty = false;
    size_t i;

    lxb_xml_attr_t *attrs = NULL;
    size_t attrs_count = 0, attrs_cap = 0;

    ctx->pos++; /* '<' */

    name = lxb_xml_scan_name(ctx, &name_len);
    if (name == NULL) {
        if (lxb_xml_error(ctx, LXB_XML_ERROR_INVALID_NAME, tag_at)
            != LXB_STATUS_OK)
        {
            return LXB_STATUS_ERROR;
        }

        /* recover: treat '<' as literal text and rescan */
        ctx->pos = tag_at + 1;
        return lxb_xml_text_put_char(ctx, '<');
    }

    /* Attributes */
    for (;;) {
        lxb_xml_attr_t *slot;
        const lxb_char_t *aname;
        size_t aname_len;

        lxb_xml_skip_s(ctx);

        if (ctx->pos >= ctx->end) {
            if (lxb_xml_error(ctx, LXB_XML_ERROR_UNEXPECTED_EOF, ctx->pos)
                != LXB_STATUS_OK)
            {
                goto failed;
            }
            goto build;
        }

        if (*ctx->pos == '>') {
            ctx->pos++;
            goto build;
        }

        if (*ctx->pos == '/' && ctx->pos + 1 < ctx->end
            && ctx->pos[1] == '>')
        {
            empty = true;
            ctx->pos += 2;
            goto build;
        }

        aname = lxb_xml_scan_name(ctx, &aname_len);
        if (aname == NULL) {
            if (lxb_xml_error(ctx, LXB_XML_ERROR_INVALID_ATTR, ctx->pos)
                != LXB_STATUS_OK)
            {
                goto failed;
            }
            /* recover: skip the offending byte */
            ctx->pos++;
            continue;
        }

        lxb_xml_skip_s(ctx);

        if (ctx->pos >= ctx->end || *ctx->pos != '=') {
            if (lxb_xml_error(ctx, LXB_XML_ERROR_INVALID_ATTR, ctx->pos)
                != LXB_STATUS_OK)
            {
                goto failed;
            }
            /* recover: keep as an empty-valued attribute */
        }
        else {
            ctx->pos++;
        }

        ctx->text.length = 0;
        status = lxb_xml_attr_value(ctx);
        if (status != LXB_STATUS_OK) {
            goto failed;
        }

        /*
         * Stash the normalized value into the doc's text pool: the scratch
         * buffer is reused by the next attribute.
         */
        {
            size_t vlen = ctx->text.length;
            lxb_char_t *copy = lxb_dom_document_create_text(ctx->doc,
                                                            vlen ? vlen : 1);
            if (copy == NULL) {
                goto failed_oom;
            }

            if (vlen != 0) {
                memcpy(copy, ctx->text.data, vlen);
            }

            ctx->text.length = 0;

            if (attrs_count == attrs_cap) {
                attrs_cap = attrs_cap ? attrs_cap * 2 : 8;
                attrs = lexbor_realloc(attrs,
                                       attrs_cap * sizeof(lxb_xml_attr_t));
                if (attrs == NULL) {
                    goto failed_oom;
                }
            }

            slot = &attrs[attrs_count++];
            slot->name = aname;
            slot->name_len = aname_len;
            slot->value = copy;
            slot->value_len = vlen;
            slot->is_xmlns = (aname_len == 5
                              && memcmp(aname, "xmlns", 5) == 0);
            slot->is_xmlns_prefix =
                (aname_len > 6 && memcmp(aname, "xmlns:", 6) == 0);
        }
    }

build:
    status = lxb_xml_attrs_check_dups(ctx, attrs, &attrs_count);
    if (status != LXB_STATUS_OK) {
        goto failed;
    }

    /*
     * Namespace declarations are bound before the element's own name is
     * resolved. Binding depth equals the stack depth before this element
     * is pushed.
     */
    for (i = 0; i < attrs_count; i++) {
        if (attrs[i].is_xmlns) {
            status = lxb_xml_ns_push(ctx, NULL, 0,
                                     attrs[i].value, attrs[i].value_len);
            if (status != LXB_STATUS_OK) {
                goto failed;
            }
        }
        else if (attrs[i].is_xmlns_prefix) {
            status = lxb_xml_ns_push(ctx,
                                     attrs[i].name + 6,
                                     attrs[i].name_len - 6,
                                     attrs[i].value, attrs[i].value_len);
            if (status != LXB_STATUS_OK) {
                goto failed;
            }
        }
    }

    if (!lxb_xml_qname_split(name, name_len,
                             &prefix, &prefix_len, &local, &local_len))
    {
        if (lxb_xml_error(ctx, LXB_XML_ERROR_INVALID_NAME, name)
            != LXB_STATUS_OK)
        {
            goto failed;
        }

        /* recover: treat ':' literally, i.e. strip a leading one */
        prefix = NULL;
        prefix_len = 0;
        local = name;
        local_len = name_len;
    }

    uri = NULL;
    uri_len = 0;
    if (prefix != NULL) {
        if (prefix_len == 5 && memcmp(prefix, "xmlns", 5) == 0) {
            if (lxb_xml_error(ctx, LXB_XML_ERROR_RESERVED_PREFIX, name)
                != LXB_STATUS_OK)
            {
                goto failed;
            }
        }
        else {
            uri = lxb_xml_ns_resolve(ctx, prefix, prefix_len, &uri_len);
            if (uri == NULL) {
                if (lxb_xml_error(ctx, LXB_XML_ERROR_UNBOUND_PREFIX, name)
                    != LXB_STATUS_OK)
                {
                    goto failed;
                }
            }
        }
    }
    else {
        uri = lxb_xml_ns_resolve(ctx, NULL, 0, &uri_len);
        /* A missing default ns simply means "no namespace". */
    }

    element = lxb_dom_element_create(ctx->doc, local, local_len,
                                     uri, uri_len,
                                     prefix, prefix_len, NULL, 0, false);
    if (element == NULL) {
        goto failed_oom;
    }

    for (i = 0; i < attrs_count; i++) {
        status = lxb_xml_attr_append(ctx, element,
                                     attrs[i].name, attrs[i].name_len,
                                     attrs[i].value, attrs[i].value_len);
        if (status != LXB_STATUS_OK) {
            goto failed;
        }
    }

    lxb_dom_node_insert_child(lxb_xml_parent(ctx),
                              lxb_dom_interface_node(element));

    if (ctx->open.length == ctx->floor) {
        if (!ctx->saw_root) {
            lxb_dom_document_attach_element(ctx->doc, element);
            ctx->saw_root = true;
        }
        else if (ctx->floor == 0) {
            /* A second root element is malformed (fragments exempt). */
            if (lxb_xml_error(ctx, LXB_XML_ERROR_MULTIPLE_ROOTS, tag_at)
                != LXB_STATUS_OK)
            {
                goto failed;
            }
            /* recover: second top-level element is appended anyway */
        }
    }

    lexbor_free(attrs);
    attrs = NULL;

    if (empty) {
        /* Bindings declared on an empty element die with it immediately. */
        lxb_xml_ns_pop_to(ctx, ctx->open.length);
    }
    else {
        if (lexbor_array_push(&ctx->open, element) != LXB_STATUS_OK) {
            return LXB_STATUS_ERROR_MEMORY_ALLOCATION;
        }
    }

    return LXB_STATUS_OK;

failed_oom:
    status = LXB_STATUS_ERROR_MEMORY_ALLOCATION;

failed:
    lexbor_free(attrs);
    return status == LXB_STATUS_OK ? LXB_STATUS_ERROR : status;
}

static lxb_status_t
lxb_xml_end_tag(lxb_xml_ctx_t *ctx)
{
    const lxb_char_t *name;
    size_t name_len, qlen;
    const lxb_char_t *tag_at = ctx->pos;
    lxb_dom_node_t *node;
    lxb_dom_element_t *el;
    const lxb_char_t *qname;
    size_t i;

    ctx->pos += 2; /* "</" */

    name = lxb_xml_scan_name(ctx, &name_len);
    lxb_xml_skip_s(ctx);

    if (ctx->pos < ctx->end && *ctx->pos == '>') {
        ctx->pos++;
    }
    else {
        if (lxb_xml_error(ctx, LXB_XML_ERROR_INVALID_NAME,
                          name != NULL ? name : ctx->pos) != LXB_STATUS_OK)
        {
            return LXB_STATUS_ERROR;
        }

        /* recover: resync to '>' */
        while (ctx->pos < ctx->end && *ctx->pos != '>') {
            ctx->pos++;
        }
        if (ctx->pos < ctx->end) {
            ctx->pos++;
        }
    }

    if (name == NULL) {
        if (lxb_xml_error(ctx, LXB_XML_ERROR_INVALID_NAME, tag_at)
            != LXB_STATUS_OK)
        {
            return LXB_STATUS_ERROR;
        }
        return LXB_STATUS_OK;
    }

    if (ctx->open.length <= ctx->floor) {
        if (lxb_xml_error(ctx, LXB_XML_ERROR_TAG_MISMATCH, tag_at)
            != LXB_STATUS_OK)
        {
            return LXB_STATUS_ERROR;
        }
        return LXB_STATUS_OK;
    }

    node = lexbor_array_get(&ctx->open, ctx->open.length - 1);
    el = lxb_dom_interface_element(node);
    qname = lxb_dom_element_qualified_name(el, &qlen);

    if (qname != NULL && qlen == name_len
        && memcmp(qname, name, name_len) == 0)
    {
        lexbor_array_pop(&ctx->open);
        lxb_xml_ns_pop_to(ctx, ctx->open.length);
        return LXB_STATUS_OK;
    }

    if (lxb_xml_error(ctx, LXB_XML_ERROR_TAG_MISMATCH, tag_at)
        != LXB_STATUS_OK)
    {
        return LXB_STATUS_ERROR;
    }

    /* recover: find a matching ancestor; auto-close intervening elements */
    for (i = ctx->open.length; i > ctx->floor; i--) {
        node = lexbor_array_get(&ctx->open, i - 1);
        el = lxb_dom_interface_element(node);
        qname = lxb_dom_element_qualified_name(el, &qlen);

        if (qname != NULL && qlen == name_len
            && memcmp(qname, name, name_len) == 0)
        {
            ctx->open.length = i - 1;
            lxb_xml_ns_pop_to(ctx, ctx->open.length);
            return LXB_STATUS_OK;
        }
    }

    /* No match: ignore the stray end tag. */
    return LXB_STATUS_OK;
}


/* ------------------------------------------------------------------------- */
/* Content loop                                                              */
/* ------------------------------------------------------------------------- */

/*
 * Scan character data until '<' or '&', validating control characters and
 * the forbidden "]]>" sequence. Only moves ctx->pos; the caller appends the
 * scanned run to the text buffer.
 */
static lxb_status_t
lxb_xml_char_data(lxb_xml_ctx_t *ctx)
{
    const lxb_char_t *p = ctx->pos;

    while (p < ctx->end) {
        if (*p == '<' || *p == '&') {
            break;
        }

        if (*p == ']' && p + 2 < ctx->end
            && p[1] == ']' && p[2] == '>')
        {
            if (lxb_xml_error(ctx, LXB_XML_ERROR_BAD_CDATA, p)
                != LXB_STATUS_OK)
            {
                return LXB_STATUS_ERROR;
            }
        }

        /* Bare control characters other than tab/LF/CR are invalid. */
        if (*p < 0x20 && !LXB_XML_IS_S(*p)) {
            if (lxb_xml_error(ctx, LXB_XML_ERROR_INVALID_CHAR, p)
                != LXB_STATUS_OK)
            {
                return LXB_STATUS_ERROR;
            }
        }

        p++;
    }

    ctx->pos = p;
    return LXB_STATUS_OK;
}

static lxb_status_t
lxb_xml_content(lxb_xml_ctx_t *ctx)
{
    const lxb_char_t *text_start;
    lxb_status_t status;

    for (;;) {
        if (lxb_xml_eof(ctx)) {
            break;
        }

        if (*ctx->pos == '<') {
            status = lxb_xml_text_flush(ctx);
            if (status != LXB_STATUS_OK) {
                return status;
            }

            if (lxb_xml_starts_with(ctx, "<!--", 4)) {
                status = lxb_xml_comment(ctx);
            }
            else if (lxb_xml_starts_with(ctx, "<![CDATA[", 9)) {
                status = lxb_xml_cdata(ctx);
            }
            else if (lxb_xml_starts_with(ctx, "<?", 2)) {
                status = lxb_xml_pi(ctx);
            }
            else if (lxb_xml_starts_with(ctx, "</", 2)) {
                status = lxb_xml_end_tag(ctx);
            }
            else if (ctx->pos + 1 < ctx->end && ctx->pos[1] == '!') {
                /* <!DOCTYPE / <!ENTITY inside content: malformed. */
                if (lxb_xml_error(ctx, LXB_XML_ERROR_BAD_DOCTYPE, ctx->pos)
                    != LXB_STATUS_OK)
                {
                    return LXB_STATUS_ERROR;
                }
                while (ctx->pos < ctx->end && *ctx->pos != '>') {
                    ctx->pos++;
                }
                if (ctx->pos < ctx->end) {
                    ctx->pos++;
                }
                status = LXB_STATUS_OK;
            }
            else {
                status = lxb_xml_start_tag(ctx);
            }

            if (status != LXB_STATUS_OK) {
                return status;
            }
            continue;
        }

        if (*ctx->pos == '&') {
            status = lxb_xml_reference(ctx);
            if (status != LXB_STATUS_OK) {
                return status;
            }
            continue;
        }

        /* Character data run. */
        text_start = ctx->pos;
        status = lxb_xml_char_data(ctx);
        if (status != LXB_STATUS_OK) {
            return status;
        }

        if (ctx->pos > text_start) {
            status = lxb_xml_text_put(ctx, text_start,
                                      (size_t) (ctx->pos - text_start));
            if (status != LXB_STATUS_OK) {
                return LXB_STATUS_ERROR_MEMORY_ALLOCATION;
            }
        }
    }

    return lxb_xml_text_flush(ctx);
}


/* ------------------------------------------------------------------------- */
/* Prolog                                                                    */
/* ------------------------------------------------------------------------- */

/*
 * "<?xml version="1.0" encoding="UTF-8" standalone="yes"?>": parsed leniently
 * for the recognized pseudo-attributes. Only UTF-8/ASCII encodings are
 * accepted; anything else must be transcoded before parsing.
 */
static lxb_status_t
lxb_xml_decl(lxb_xml_ctx_t *ctx)
{
    const lxb_char_t *name, *value;
    size_t name_len;
    lxb_char_t quote;

    ctx->pos += 5; /* "<?xml" */

    for (;;) {
        lxb_xml_skip_s(ctx);

        if (lxb_xml_starts_with(ctx, "?>", 2)) {
            ctx->pos += 2;
            return LXB_STATUS_OK;
        }

        if (ctx->pos >= ctx->end) {
            goto bad;
        }

        name = lxb_xml_scan_name(ctx, &name_len);
        if (name == NULL) {
            goto bad;
        }

        lxb_xml_skip_s(ctx);
        if (ctx->pos >= ctx->end || *ctx->pos != '=') {
            goto bad;
        }
        ctx->pos++;
        lxb_xml_skip_s(ctx);

        if (ctx->pos >= ctx->end
            || (*ctx->pos != '"' && *ctx->pos != '\''))
        {
            goto bad;
        }
        quote = *ctx->pos++;
        value = ctx->pos;

        while (ctx->pos < ctx->end && *ctx->pos != quote) {
            ctx->pos++;
        }

        if (ctx->pos >= ctx->end) {
            goto bad;
        }

        if (name_len == 7 && memcmp(name, "version", 7) == 0) {
            /* Only XML 1.x is supported. */
            if (ctx->pos - value < 3 || value[0] != '1' || value[1] != '.') {
                goto bad;
            }
        }
        else if (name_len == 8 && memcmp(name, "encoding", 8) == 0) {
            size_t elen = (size_t) (ctx->pos - value);
            bool ok = false;

            if ((elen == 5 && (memcmp(value, "UTF-8", 5) == 0
                               || memcmp(value, "utf-8", 5) == 0
                               || memcmp(value, "UTF8", 5) == 0
                               || memcmp(value, "ascii", 5) == 0
                               || memcmp(value, "ASCII", 5) == 0))
                || (elen == 4 && memcmp(value, "UTF8", 4) == 0)
                || (elen == 7 && (memcmp(value, "us-ascii", 7) == 0
                                  || memcmp(value, "US-ASCII", 7) == 0)))
            {
                ok = true;
            }

            if (!ok) {
                if (lxb_xml_error(ctx, LXB_XML_ERROR_UNSUPPORTED_ENCODING,
                                  value) != LXB_STATUS_OK)
                {
                    return LXB_STATUS_ERROR;
                }
            }
        }
        else if (name_len == 10 && memcmp(name, "standalone", 10) == 0) {
            ctx->document->standalone =
                (ctx->pos - value == 3 && memcmp(value, "yes", 3) == 0);
        }
        /* Unknown pseudo-attrs are ignored. */

        ctx->pos++; /* closing quote */
    }

bad:
    if (lxb_xml_error(ctx, LXB_XML_ERROR_BAD_DECL, ctx->pos)
        != LXB_STATUS_OK)
    {
        return LXB_STATUS_ERROR;
    }

    /* recover: resync past "?>" or to EOF */
    while (ctx->pos < ctx->end && !lxb_xml_starts_with(ctx, "?>", 2)) {
        ctx->pos++;
    }
    if (ctx->pos < ctx->end) {
        ctx->pos += 2;
    }

    return LXB_STATUS_OK;
}

static lxb_status_t
lxb_xml_prolog_misc(lxb_xml_ctx_t *ctx, bool *saw_doctype)
{
    /*
     * Consume whitespace, comments, PIs and at most one DOCTYPE between the
     * declaration and the root element.
     */
    lxb_status_t status;

    lxb_xml_skip_s(ctx);

    while (ctx->pos < ctx->end) {
        if (lxb_xml_starts_with(ctx, "<!--", 4)) {
            status = lxb_xml_comment(ctx);
            if (status != LXB_STATUS_OK) {
                return status;
            }
            lxb_xml_skip_s(ctx);
            continue;
        }

        if (lxb_xml_starts_with(ctx, "<?", 2)) {
            status = lxb_xml_pi(ctx);
            if (status != LXB_STATUS_OK) {
                return status;
            }
            lxb_xml_skip_s(ctx);
            continue;
        }

        if (!*saw_doctype && lxb_xml_starts_with(ctx, "<!DOCTYPE", 9)
            && (ctx->pos + 9 >= ctx->end || LXB_XML_IS_S(ctx->pos[9])))
        {
            ctx->pos += 9;
            *saw_doctype = true;
            status = lxb_xml_doctype(ctx);
            if (status != LXB_STATUS_OK) {
                return status;
            }
            lxb_xml_skip_s(ctx);
            continue;
        }

        break;
    }

    return LXB_STATUS_OK;
}


/* ------------------------------------------------------------------------- */
/* Entry points                                                              */
/* ------------------------------------------------------------------------- */

lxb_status_t
lxb_xml_document_parse(lxb_xml_document_t *document,
                       const lxb_char_t *data, size_t size)
{
    return lxb_xml_document_parse_ex(document, data, size, false);
}

lxb_status_t
lxb_xml_document_parse_ex(lxb_xml_document_t *document,
                          const lxb_char_t *data, size_t size, bool recover)
{
    lxb_xml_ctx_t ctx;
    lxb_status_t status;
    bool saw_doctype = false;
    const lxb_char_t *p;

    if (document == NULL || (data == NULL && size != 0)) {
        return LXB_STATUS_ERROR_WRONG_ARGS;
    }

    document->recover = recover;
    document->decl_seen = false;
    document->standalone = false;
    memset(&document->first_error, 0, sizeof(document->first_error));

    if (recover && document->errors == NULL) {
        document->errors = lexbor_array_obj_create();
        if (document->errors == NULL
            || lexbor_array_obj_init(document->errors, 16,
                                     sizeof(lxb_xml_parse_error_t))
                   != LXB_STATUS_OK)
        {
            return LXB_STATUS_ERROR_MEMORY_ALLOCATION;
        }
    }

    if (document->errors != NULL) {
        lexbor_array_obj_clean(document->errors);
    }

    status = lxb_xml_ctx_init(&ctx, document, &document->dom_document,
                            data, size);
    if (status != LXB_STATUS_OK) {
        return status;
    }

    /* Byte order marks. */
    p = ctx.pos;
    if (size >= 3 && p[0] == 0xEF && p[1] == 0xBB && p[2] == 0xBF) {
        ctx.pos += 3;
    }
    else if (size >= 2 && ((p[0] == 0xFF && p[1] == 0xFE)
                           || (p[0] == 0xFE && p[1] == 0xFF)))
    {
        lxb_xml_error(&ctx, LXB_XML_ERROR_UNSUPPORTED_ENCODING, ctx.pos);
        status = LXB_STATUS_ERROR;
        goto done;
    }

    /* XML declaration: only valid as the very first thing. */
    if (lxb_xml_is_decl(&ctx)) {
        document->decl_seen = true;
        status = lxb_xml_decl(&ctx);
        if (status != LXB_STATUS_OK) {
            goto done;
        }
    }

    /* Misc before the root element. */
    status = lxb_xml_prolog_misc(&ctx, &saw_doctype);
    if (status != LXB_STATUS_OK) {
        goto done;
    }

    /* Root element + its content (content also covers the epilog misc). */
    status = lxb_xml_content(&ctx);
    if (status != LXB_STATUS_OK) {
        goto done;
    }

    if (ctx.open.length != 0) {
        if (lxb_xml_error(&ctx, LXB_XML_ERROR_UNCLOSED_ELEMENT, ctx.end)
            != LXB_STATUS_OK)
        {
            status = LXB_STATUS_ERROR;
            goto done;
        }
        /* recover: elements are closed by dropping the open stack. */
        ctx.open.length = 0;
        ctx.ns.length = 0;
    }

    if (!ctx.saw_root) {
        if (lxb_xml_error(&ctx, LXB_XML_ERROR_MISSING_ROOT, ctx.pos)
            != LXB_STATUS_OK)
        {
            status = LXB_STATUS_ERROR;
            goto done;
        }
    }

    status = LXB_STATUS_OK;

done:
    lxb_xml_ctx_destroy(&ctx);
    return status;
}

lxb_status_t
lxb_xml_parse_fragment_into(lxb_dom_document_t *doc,
                            lxb_dom_node_t *parent,
                            const lxb_char_t *data, size_t size)
{
    lxb_xml_ctx_t ctx;
    lxb_status_t status;

    /*
     * lxb_xml_document_t embeds the DOM document as its first member, so the
     * document pointer doubles as the wrapper for error/recover bookkeeping.
     */
    status = lxb_xml_ctx_init(&ctx, (lxb_xml_document_t *) doc, doc,
                            data, size);
    if (status != LXB_STATUS_OK) {
        return status;
    }

    /* Seed the stack with the target node; content parents there. */
    if (lexbor_array_push(&ctx.open, parent) != LXB_STATUS_OK) {
        lxb_xml_ctx_destroy(&ctx);
        return LXB_STATUS_ERROR_MEMORY_ALLOCATION;
    }

    ctx.floor = 1;
    ctx.saw_root = true; /* fragments may have any number of top-level nodes */

    status = lxb_xml_content(&ctx);

    ctx.open.length = 0;
    lxb_xml_ctx_destroy(&ctx);

    return status;
}
