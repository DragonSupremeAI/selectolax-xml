/*
 * XML module for the Lexbor engine.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef LEXBOR_XML_DOCUMENT_H
#define LEXBOR_XML_DOCUMENT_H

#ifdef __cplusplus
extern "C" {
#endif

#include "lexbor/xml/base.h"
#include "lexbor/xml/const.h"

typedef struct {
    lxb_xml_error_id_t  id;
    size_t              offset;    /* byte offset into the input buffer */
    size_t              line;      /* 1-based line */
    size_t              col;       /* 1-based byte column */
}
lxb_xml_parse_error_t;

typedef struct lxb_xml_document {
    lxb_dom_document_t      dom_document;

    lexbor_array_obj_t     *errors;      /* lxb_xml_parse_error_t, recover mode */
    lxb_xml_parse_error_t   first_error; /* strict mode */

    bool                    recover;
    bool                    standalone;  /* xml decl: standalone="yes" */
    bool                    decl_seen;
}
lxb_xml_document_t;

/*
 * Create a new XML document. The embedded DOM document is initialised with
 * LXB_DOM_DOCUMENT_DTYPE_XML and generic DOM interfaces (no HTML element
 * wrappers, no XHTML namespace).
 */
LXB_API lxb_xml_document_t *
lxb_xml_document_create(void);

LXB_API void
lxb_xml_document_clean(lxb_xml_document_t *document);

LXB_API lxb_xml_document_t *
lxb_xml_document_destroy(lxb_xml_document_t *document);

/*
 * Parse a complete XML document. `data` must be UTF-8; a UTF-8 BOM is
 * skipped and a UTF-16 BOM is rejected. Parsing is strict: the first
 * well-formedness violation stops the parse, fills `document->first_error`
 * and returns a non-OK status.
 */
LXB_API lxb_status_t
lxb_xml_document_parse(lxb_xml_document_t *document,
                       const lxb_char_t *data, size_t size);

/*
 * Same as lxb_xml_document_parse, but with `recover` the parser produces a
 * best-effort tree instead of aborting: unmatched end tags are ignored,
 * unclosed elements are closed at EOF, malformed markup becomes text and
 * every violation is appended to `document->errors`.
 */
LXB_API lxb_status_t
lxb_xml_document_parse_ex(lxb_xml_document_t *document,
                          const lxb_char_t *data, size_t size, bool recover);

/*
 * Errors collected during a `recover` parse. Returns NULL for strict parses.
 */
LXB_API lxb_xml_parse_error_t *
lxb_xml_document_parse_errors(lxb_xml_document_t *document, size_t *count);

/*
 * Parse an XML fragment (element content) and append the resulting nodes to
 * `parent` — typically an element or a document fragment. Used by the
 * inner_html setter and by is_fragment parsing. Strict or recover semantics
 * follow `document->recover`.
 */
LXB_API lxb_status_t
lxb_xml_parse_fragment_into(lxb_dom_document_t *doc,
                            lxb_dom_node_t *parent,
                            const lxb_char_t *data, size_t size);

/*
 * Human-readable name for an error id ("tag_mismatch", ...).
 */
LXB_API const char *
lxb_xml_error_name(lxb_xml_error_id_t id);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* LEXBOR_XML_DOCUMENT_H */
