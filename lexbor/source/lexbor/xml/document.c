/*
 * XML module for the Lexbor engine.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "lexbor/xml/document.h"

#include "lexbor/dom/interfaces/comment.h"
#include "lexbor/dom/interfaces/element.h"
#include "lexbor/dom/interfaces/text.h"


/*
 * Generic DOM create_interface always builds an element, so XML documents
 * dispatch by tag id: character-data nodes need their own interface type or
 * they end up node-typed as elements named "#text"/"#comment".
 */
static lxb_dom_interface_t *
lxb_xml_interface_create(lxb_dom_document_t *document, lxb_tag_id_t tag_id,
                         lxb_ns_id_t ns)
{
    lxb_dom_node_t *node;

    switch (tag_id) {
        case LXB_TAG__TEXT:
            node = lxb_dom_interface_node(
                lxb_dom_text_interface_create(document));
            break;

        case LXB_TAG__EM_COMMENT:
            node = lxb_dom_interface_node(
                lxb_dom_comment_interface_create(document));
            break;

        default:
            node = lxb_dom_interface_node(
                lxb_dom_element_interface_create(document));
            break;
    }

    if (node == NULL) {
        return NULL;
    }

    node->local_name = tag_id;
    node->ns = ns;

    return (lxb_dom_interface_t *) node;
}


lxb_xml_document_t *
lxb_xml_document_create(void)
{
    lxb_status_t status;
    lxb_xml_document_t *document;

    document = lexbor_calloc(1, sizeof(lxb_xml_document_t));
    if (document == NULL) {
        return NULL;
    }

    /*
     * Generic DOM interfaces, no XHTML namespace, fresh tag/attr hashes:
     * element creation stays case-sensitive and produces plain
     * lxb_dom_element_t nodes.
     */
    status = lxb_dom_document_init(&document->dom_document, NULL,
                                   lxb_xml_interface_create,
                                   lxb_dom_interface_clone,
                                   lxb_dom_interface_destroy,
                                   LXB_DOM_DOCUMENT_DTYPE_XML,
                                   LXB_NS__UNDEF);
    if (status != LXB_STATUS_OK) {
        lexbor_free(document);
        return NULL;
    }

    return document;
}

void
lxb_xml_document_clean(lxb_xml_document_t *document)
{
    lxb_dom_document_clean(&document->dom_document);

    if (document->errors != NULL) {
        lexbor_array_obj_clean(document->errors);
    }

    memset(&document->first_error, 0, sizeof(document->first_error));
    document->standalone = false;
    document->decl_seen = false;
}

lxb_xml_document_t *
lxb_xml_document_destroy(lxb_xml_document_t *document)
{
    if (document == NULL) {
        return NULL;
    }

    if (document->errors != NULL) {
        lexbor_array_obj_destroy(document->errors, true);
    }

    /*
     * dom_document is the first member of lxb_xml_document_t, so its
     * address is the document itself: lxb_dom_document_destroy() frees
     * the whole struct with lexbor_free().
     */
    lxb_dom_document_destroy(&document->dom_document);

    return NULL;
}

const char *
lxb_xml_error_name(lxb_xml_error_id_t id)
{
    static const char *names[] = {
        "ok",
        "unexpected_eof",
        "invalid_char",
        "invalid_name",
        "tag_mismatch",
        "unclosed_element",
        "multiple_roots",
        "missing_root",
        "unexpected_text",
        "invalid_attr",
        "duplicate_attr",
        "unquoted_attr",
        "invalid_entity",
        "invalid_charref",
        "unbound_prefix",
        "reserved_prefix",
        "bad_comment",
        "bad_pi",
        "bad_cdata",
        "bad_doctype",
        "bad_decl",
        "unsupported_encoding",
        "content_after_root",
        "memory",
    };

    if (id >= LXB_XML_ERROR_LAST_ENTRY) {
        return "unknown";
    }

    return names[id];
}

lxb_xml_parse_error_t *
lxb_xml_document_parse_errors(lxb_xml_document_t *document, size_t *count)
{
    if (document->errors == NULL) {
        *count = 0;
        return NULL;
    }

    *count = lexbor_array_obj_length(document->errors);
    return lexbor_array_obj_get(document->errors, 0);
}
