/*
 * XML module for the Lexbor engine.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef LEXBOR_XML_SERIALIZE_H
#define LEXBOR_XML_SERIALIZE_H

#ifdef __cplusplus
extern "C" {
#endif

#include "lexbor/xml/base.h"

/*
 * Serialize `node` and its whole subtree to XML: elements emit `/>` when
 * empty, text and attribute values are entity-escaped, CDATA sections pass
 * through verbatim, comments/PIs/doctype are preserved.
 * (Same coverage as lxb_html_serialize_tree_str.)
 */
LXB_API lxb_status_t
lxb_xml_serialize_tree_str(lxb_dom_node_t *node, lexbor_str_t *str);

/*
 * Serialize only the children of `node` (like innerHTML, for XML).
 * (Same coverage as lxb_html_serialize_deep_str.)
 */
LXB_API lxb_status_t
lxb_xml_serialize_deep_str(lxb_dom_node_t *node, lexbor_str_t *str);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* LEXBOR_XML_SERIALIZE_H */
