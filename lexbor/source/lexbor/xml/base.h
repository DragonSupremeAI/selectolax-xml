/*
 * XML module for the Lexbor engine.
 *
 * A strict-by-default XML 1.0 parser that builds lexbor DOM trees
 * (lxb_dom_document_t with type LXB_DOM_DOCUMENT_DTYPE_XML), reusing the
 * core arena allocators, hash tables and namespace machinery.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef LEXBOR_XML_BASE_H
#define LEXBOR_XML_BASE_H

#ifdef __cplusplus
extern "C" {
#endif

#include "lexbor/core/base.h"
#include "lexbor/core/array.h"
#include "lexbor/core/array_obj.h"
#include "lexbor/core/str.h"
#include "lexbor/core/hash.h"
#include "lexbor/core/mraw.h"

#include "lexbor/tag/tag.h"
#include "lexbor/ns/ns.h"

#include "lexbor/dom/dom.h"

#define LXB_XML_VERSION_MAJOR 1
#define LXB_XML_VERSION_MINOR 0

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* LEXBOR_XML_BASE_H */
