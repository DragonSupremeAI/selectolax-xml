/*
 * XML module for the Lexbor engine.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>

#include "lexbor/xml/serialize.h"

#include "lexbor/dom/interfaces/attr.h"
#include "lexbor/dom/interfaces/character_data.h"
#include "lexbor/dom/interfaces/cdata_section.h"
#include "lexbor/dom/interfaces/comment.h"
#include "lexbor/dom/interfaces/document_type.h"
#include "lexbor/dom/interfaces/element.h"
#include "lexbor/dom/interfaces/processing_instruction.h"


static lxb_status_t
lxb_xml_serialize_str_append(lexbor_str_t *str, lexbor_mraw_t *mraw,
                             const lxb_char_t *data, size_t len)
{
    return (lexbor_str_append(str, mraw, data, len) == NULL)
           ? LXB_STATUS_ERROR_MEMORY_ALLOCATION : LXB_STATUS_OK;
}

static lxb_status_t
lxb_xml_serialize_lit(lexbor_str_t *str, lexbor_mraw_t *mraw,
                      const char *lit, size_t len)
{
    return (lexbor_str_append(str, mraw, (const lxb_char_t *) lit, len) == NULL)
           ? LXB_STATUS_ERROR_MEMORY_ALLOCATION : LXB_STATUS_OK;
}

/*
 * Escape text: & < > become references. ">" is escaped so a literal "]]>"
 * can never appear in serialized output.
 */
static lxb_status_t
lxb_xml_serialize_text(lexbor_str_t *str, lexbor_mraw_t *mraw,
                       const lxb_char_t *data, size_t len)
{
    const lxb_char_t *p, *run;

    run = data;
    for (p = data; p < data + len; p++) {
        const char *rep = NULL;
        size_t rep_len = 0;

        switch (*p) {
            case '&': rep = "&amp;"; rep_len = 5; break;
            case '<': rep = "&lt;";  rep_len = 4; break;
            case '>': rep = "&gt;";  rep_len = 4; break;
            default:  continue;
        }

        if (lxb_xml_serialize_str_append(str, mraw, run,
                                         (size_t) (p - run))
            != LXB_STATUS_OK)
        {
            return LXB_STATUS_ERROR_MEMORY_ALLOCATION;
        }

        if (lxb_xml_serialize_lit(str, mraw, rep, rep_len)
            != LXB_STATUS_OK)
        {
            return LXB_STATUS_ERROR_MEMORY_ALLOCATION;
        }

        run = p + 1;
    }

    return lxb_xml_serialize_str_append(str, mraw, run,
                                        (size_t) (p - run));
}

/*
 * Escape an attribute value: & < " are references, and tab/LF/CR become
 * character references so normalized whitespace survives a reparse.
 */
static lxb_status_t
lxb_xml_serialize_attr_value(lexbor_str_t *str, lexbor_mraw_t *mraw,
                             const lxb_char_t *data, size_t len)
{
    const lxb_char_t *p, *run;

    run = data;
    for (p = data; p < data + len; p++) {
        const char *rep = NULL;
        size_t rep_len = 0;

        switch (*p) {
            case '&':  rep = "&amp;";  rep_len = 5; break;
            case '<':  rep = "&lt;";   rep_len = 4; break;
            case '"':  rep = "&quot;"; rep_len = 6; break;
            case 0x09: rep = "&#9;";   rep_len = 4; break;
            case 0x0A: rep = "&#10;";  rep_len = 5; break;
            case 0x0D: rep = "&#13;";  rep_len = 5; break;
            default:   continue;
        }

        if (lxb_xml_serialize_str_append(str, mraw, run,
                                         (size_t) (p - run))
            != LXB_STATUS_OK)
        {
            return LXB_STATUS_ERROR_MEMORY_ALLOCATION;
        }

        if (lxb_xml_serialize_lit(str, mraw, rep, rep_len)
            != LXB_STATUS_OK)
        {
            return LXB_STATUS_ERROR_MEMORY_ALLOCATION;
        }

        run = p + 1;
    }

    return lxb_xml_serialize_str_append(str, mraw, run,
                                        (size_t) (p - run));
}

static lxb_status_t
lxb_xml_serialize_attrs(lexbor_str_t *str, lexbor_mraw_t *mraw,
                        lxb_dom_element_t *element)
{
    lxb_dom_attr_t *attr;
    const lxb_char_t *name, *value;
    size_t name_len, value_len;

    for (attr = lxb_dom_element_first_attribute_noi(element);
         attr != NULL; attr = attr->next)
    {
        name = lxb_dom_attr_qualified_name(attr, &name_len);
        if (name == NULL) {
            continue;
        }

        if (lxb_xml_serialize_lit(str, mraw, " ", 1) != LXB_STATUS_OK
            || lxb_xml_serialize_str_append(str, mraw, name, name_len)
                != LXB_STATUS_OK
            || lxb_xml_serialize_lit(str, mraw, "=\"", 2) != LXB_STATUS_OK)
        {
            return LXB_STATUS_ERROR_MEMORY_ALLOCATION;
        }

        value = lxb_dom_attr_value_noi(attr, &value_len);
        if (value != NULL && value_len != 0) {
            if (lxb_xml_serialize_attr_value(str, mraw, value, value_len)
                != LXB_STATUS_OK)
            {
                return LXB_STATUS_ERROR_MEMORY_ALLOCATION;
            }
        }

        if (lxb_xml_serialize_lit(str, mraw, "\"", 1) != LXB_STATUS_OK) {
            return LXB_STATUS_ERROR_MEMORY_ALLOCATION;
        }
    }

    return LXB_STATUS_OK;
}

static lxb_status_t
lxb_xml_serialize_node(lxb_dom_node_t *node, lexbor_str_t *str,
                       lexbor_mraw_t *mraw)
{
    const lxb_char_t *name, *data;
    size_t name_len, data_len;
    lxb_dom_element_t *element;
    lxb_dom_attr_t *attr;
    lxb_dom_document_type_t *dtype;
    lxb_dom_processing_instruction_t *pi;

    switch (node->type) {
        case LXB_DOM_NODE_TYPE_ELEMENT:
            element = lxb_dom_interface_element(node);
            name = lxb_dom_element_qualified_name(element, &name_len);
            if (name == NULL) {
                return LXB_STATUS_ERROR;
            }

            if (lxb_xml_serialize_lit(str, mraw, "<", 1) != LXB_STATUS_OK
                || lxb_xml_serialize_str_append(str, mraw, name, name_len)
                    != LXB_STATUS_OK)
            {
                return LXB_STATUS_ERROR_MEMORY_ALLOCATION;
            }

            if (lxb_xml_serialize_attrs(str, mraw, element)
                != LXB_STATUS_OK)
            {
                return LXB_STATUS_ERROR_MEMORY_ALLOCATION;
            }

            if (node->first_child == NULL) {
                return lxb_xml_serialize_lit(str, mraw, "/>", 2);
            }

            return lxb_xml_serialize_lit(str, mraw, ">", 1);

        case LXB_DOM_NODE_TYPE_TEXT:
            data = lxb_dom_interface_character_data(node)->data.data;
            data_len = lxb_dom_interface_character_data(node)->data.length;

            if (data == NULL) {
                return LXB_STATUS_OK;
            }

            return lxb_xml_serialize_text(str, mraw, data, data_len);

        case LXB_DOM_NODE_TYPE_CDATA_SECTION:
            if (lxb_xml_serialize_lit(str, mraw, "<![CDATA[", 9)
                != LXB_STATUS_OK)
            {
                return LXB_STATUS_ERROR_MEMORY_ALLOCATION;
            }

            data = lxb_dom_interface_character_data(node)->data.data;
            data_len = lxb_dom_interface_character_data(node)->data.length;

            if (data != NULL && data_len != 0
                && lxb_xml_serialize_str_append(str, mraw, data, data_len)
                    != LXB_STATUS_OK)
            {
                return LXB_STATUS_ERROR_MEMORY_ALLOCATION;
            }

            return lxb_xml_serialize_lit(str, mraw, "]]>", 3);

        case LXB_DOM_NODE_TYPE_COMMENT:
            if (lxb_xml_serialize_lit(str, mraw, "<!--", 4)
                != LXB_STATUS_OK)
            {
                return LXB_STATUS_ERROR_MEMORY_ALLOCATION;
            }

            data = lxb_dom_interface_character_data(node)->data.data;
            data_len = lxb_dom_interface_character_data(node)->data.length;

            if (data != NULL && data_len != 0
                && lxb_xml_serialize_str_append(str, mraw, data, data_len)
                    != LXB_STATUS_OK)
            {
                return LXB_STATUS_ERROR_MEMORY_ALLOCATION;
            }

            return lxb_xml_serialize_lit(str, mraw, "-->", 3);

        case LXB_DOM_NODE_TYPE_PROCESSING_INSTRUCTION:
            pi = lxb_dom_interface_processing_instruction(node);
            name = lxb_dom_processing_instruction_target(pi, &name_len);

            if (lxb_xml_serialize_lit(str, mraw, "<?", 2) != LXB_STATUS_OK
                || (name != NULL
                    && lxb_xml_serialize_str_append(str, mraw, name,
                                                    name_len)
                       != LXB_STATUS_OK))
            {
                return LXB_STATUS_ERROR_MEMORY_ALLOCATION;
            }

            data = pi->char_data.data.data;
            data_len = pi->char_data.data.length;

            if (data != NULL && data_len != 0) {
                if (lxb_xml_serialize_lit(str, mraw, " ", 1)
                        != LXB_STATUS_OK
                    || lxb_xml_serialize_str_append(str, mraw, data, data_len)
                        != LXB_STATUS_OK)
                {
                    return LXB_STATUS_ERROR_MEMORY_ALLOCATION;
                }
            }

            return lxb_xml_serialize_lit(str, mraw, "?>", 2);

        case LXB_DOM_NODE_TYPE_DOCUMENT_TYPE:
            dtype = lxb_dom_interface_document_type(node);
            name = lxb_dom_document_type_name(dtype, &name_len);

            if (lxb_xml_serialize_lit(str, mraw, "<!DOCTYPE ", 10)
                != LXB_STATUS_OK
                || (name != NULL
                    && lxb_xml_serialize_str_append(str, mraw, name,
                                                    name_len)
                       != LXB_STATUS_OK))
            {
                return LXB_STATUS_ERROR_MEMORY_ALLOCATION;
            }

            if (dtype->public_id.data != NULL
                && dtype->public_id.length != 0)
            {
                if (lxb_xml_serialize_lit(str, mraw, " PUBLIC \"", 9)
                        != LXB_STATUS_OK
                    || lxb_xml_serialize_str_append(str, mraw,
                                        dtype->public_id.data,
                                        dtype->public_id.length)
                        != LXB_STATUS_OK
                    || lxb_xml_serialize_lit(str, mraw, "\"", 1)
                        != LXB_STATUS_OK)
                {
                    return LXB_STATUS_ERROR_MEMORY_ALLOCATION;
                }
            }

            if (dtype->system_id.data != NULL
                && dtype->system_id.length != 0)
            {
                if (dtype->public_id.data != NULL
                    && dtype->public_id.length != 0)
                {
                    /* PUBLIC "..." "sys": space then the quoted literal. */
                    if (lxb_xml_serialize_lit(str, mraw, " \"", 2)
                        != LXB_STATUS_OK)
                    {
                        return LXB_STATUS_ERROR_MEMORY_ALLOCATION;
                    }
                }
                else {
                    if (lxb_xml_serialize_lit(str, mraw, " SYSTEM \"", 9)
                        != LXB_STATUS_OK)
                    {
                        return LXB_STATUS_ERROR_MEMORY_ALLOCATION;
                    }
                }

                if (lxb_xml_serialize_str_append(str, mraw,
                                        dtype->system_id.data,
                                        dtype->system_id.length)
                        != LXB_STATUS_OK
                    || lxb_xml_serialize_lit(str, mraw, "\"", 1)
                        != LXB_STATUS_OK)
                {
                    return LXB_STATUS_ERROR_MEMORY_ALLOCATION;
                }
            }

            return lxb_xml_serialize_lit(str, mraw, ">", 1);

        default:
            return LXB_STATUS_OK;
    }

    (void) attr;
}

/*
 * Iterative depth-first walk of `root`'s children. Elements emit their close
 * tag when the walk ascends back past them.
 */
static lxb_status_t
lxb_xml_serialize_children(lxb_dom_node_t *root, lexbor_str_t *str,
                           lexbor_mraw_t *mraw, bool close_root)
{
    lxb_dom_node_t *node;
    lxb_dom_node_t *parent;
    const lxb_char_t *name;
    size_t name_len;

    for (node = root->first_child; node != NULL; ) {
        if (lxb_xml_serialize_node(node, str, mraw) != LXB_STATUS_OK) {
            return LXB_STATUS_ERROR_MEMORY_ALLOCATION;
        }

        if (node->first_child != NULL) {
            node = node->first_child;
            continue;
        }

        /* Leaf reached: close element and climb to the next sibling. */
        for (;;) {
            /*
             * The walk is bounded at `root`: close_root decides whether its
             * own tag was opened above (tree) or not (deep/children only).
             */
            if (node == root && !close_root) {
                return LXB_STATUS_OK;
            }

            /* Leaf elements were emitted as "<name/>"; only close tags
             * that were opened with ">". */
            if (node->type == LXB_DOM_NODE_TYPE_ELEMENT
                && node->first_child != NULL)
            {
                name = lxb_dom_element_qualified_name(
                    lxb_dom_interface_element(node), &name_len);

                if (name != NULL) {
                    if (lxb_xml_serialize_lit(str, mraw, "</", 2)
                            != LXB_STATUS_OK
                        || lxb_xml_serialize_str_append(str, mraw, name,
                                                        name_len)
                            != LXB_STATUS_OK
                        || lxb_xml_serialize_lit(str, mraw, ">", 1)
                            != LXB_STATUS_OK)
                    {
                        return LXB_STATUS_ERROR_MEMORY_ALLOCATION;
                    }
                }
            }

            if (node == root) {
                return LXB_STATUS_OK;
            }

            if (node->next != NULL) {
                node = node->next;
                break;
            }

            parent = node->parent;
            if (parent == NULL) {
                return LXB_STATUS_OK;
            }
            node = parent;
        }
    }

    return LXB_STATUS_OK;
}

lxb_status_t
lxb_xml_serialize_tree_str(lxb_dom_node_t *node, lexbor_str_t *str)
{
    lexbor_mraw_t *mraw;

    if (node == NULL || str == NULL) {
        return LXB_STATUS_ERROR_WRONG_ARGS;
    }

    /* lexbor_str_t handed in is freed with the document's text arena. */
    mraw = node->owner_document->text;
    if (str->data == NULL) {
        lexbor_str_init(str, mraw, 1024);
    }

    if (lxb_xml_serialize_node(node, str, mraw) != LXB_STATUS_OK) {
        return LXB_STATUS_ERROR_MEMORY_ALLOCATION;
    }

    return lxb_xml_serialize_children(node, str, mraw, true);
}

lxb_status_t
lxb_xml_serialize_deep_str(lxb_dom_node_t *node, lexbor_str_t *str)
{
    if (node == NULL || str == NULL) {
        return LXB_STATUS_ERROR_WRONG_ARGS;
    }

    if (str->data == NULL) {
        lexbor_str_init(str, node->owner_document->text, 1024);
    }
    return lxb_xml_serialize_children(node, str,
                                      node->owner_document->text, false);
}
