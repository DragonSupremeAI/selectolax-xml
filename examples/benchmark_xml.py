"""Benchmark LexborXMLParser against the major Python XML parsers.

Generates a synthetic well-formed XML corpus (no external data needed) and
measures two things per parser:

* ``parse``    -- raw parse time: build the whole tree / consume the stream.
* ``workload`` -- parse plus a realistic extraction: collect every item's
                  ``id`` attribute and every ``name`` element's text.

Parsers covered: selectolax (Lexbor), lxml.etree, xml.etree.ElementTree,
defusedxml.ElementTree, xml.dom.minidom, xml.sax, xml.parsers.expat and
xmltodict. Missing parsers are skipped with a note.

Usage::

    python examples/benchmark_xml.py [--docs N] [--items N] [--repeat N]
"""

import argparse
import statistics
import time
import xml.dom.minidom
import xml.etree.ElementTree as ET
import xml.parsers.expat
import xml.sax
import xml.sax.handler

from selectolax.lexbor import LexborXMLParser

PARSERS = {}

try:
    from lxml import etree as lxml_etree
except ImportError:
    lxml_etree = None

try:
    import defusedxml.ElementTree as defused_ET
except ImportError:
    defused_ET = None

try:
    import xmltodict
except ImportError:
    xmltodict = None


def make_document(num_items=500):
    parts = [
        '<?xml version="1.0" encoding="UTF-8"?>',
        '<catalog xmlns:p="urn:products">',
    ]
    for i in range(num_items):
        parts.append(
            '<p:item id="it-%d" p:sku="SKU-%05d" class="stock">'
            '<name lang="en">Product %d &amp; Co</name>'
            '<price currency="USD">%d.99</price>'
            '<desc>A &lt;description&gt; for product %d</desc>'
            '</p:item>' % (i, i, i, i, i)
        )
    parts.append('</catalog>')
    return ''.join(parts)


def make_corpus(num_docs, num_items):
    doc = make_document(num_items)
    return [doc for _ in range(num_docs)]


# --- selectolax ----------------------------------------------------------

def parse_selectolax(text):
    LexborXMLParser(text)


def workload_selectolax(text):
    tree = LexborXMLParser(text)
    ids = [n.attributes.get("id") for n in tree.css("item")]
    names = [n.text() for n in tree.css("name")]
    assert len(ids) == len(names)
    return len(ids)


PARSERS["selectolax"] = (parse_selectolax, workload_selectolax)


# --- lxml.etree ----------------------------------------------------------

if lxml_etree is not None:
    def parse_lxml(text):
        lxml_etree.fromstring(text.encode("utf-8"))

    def workload_lxml(text):
        root = lxml_etree.fromstring(text.encode("utf-8"))
        ids = [n.get("id") for n in root.iter("{urn:products}item")]
        names = [n.text for n in root.iter("name")]
        assert len(ids) == len(names)
        return len(ids)

    PARSERS["lxml"] = (parse_lxml, workload_lxml)


# --- xml.etree.ElementTree -----------------------------------------------

def parse_elementtree(text):
    ET.fromstring(text)


def workload_elementtree(text):
    root = ET.fromstring(text)
    ns = {"p": "urn:products"}
    ids = [n.attrib.get("id") for n in root.findall("p:item", ns)]
    names = [n.text for n in root.findall("p:item/name", ns)]
    assert len(ids) == len(names)
    return len(ids)


PARSERS["ElementTree"] = (parse_elementtree, workload_elementtree)


# --- defusedxml.ElementTree -----------------------------------------------

if defused_ET is not None:
    def parse_defused(text):
        defused_ET.fromstring(text)

    PARSERS["defusedxml"] = (parse_defused, None)


# --- xml.dom.minidom -------------------------------------------------------

def parse_minidom(text):
    xml.dom.minidom.parseString(text)


def workload_minidom(text):
    doc = xml.dom.minidom.parseString(text)
    items = doc.getElementsByTagNameNS("urn:products", "item")
    ids = [n.getAttribute("id") for n in items]
    names = doc.getElementsByTagName("name")
    texts = [n.firstChild.data if n.firstChild else "" for n in names]
    assert len(ids) == len(texts)
    return len(ids)


PARSERS["minidom"] = (parse_minidom, workload_minidom)


# --- xml.sax ---------------------------------------------------------------

class _ItemCollector(xml.sax.handler.ContentHandler):
    def __init__(self):
        super().__init__()
        self.ids = []
        self.names = []
        self._in_name = False
        self._buf = []

    def startElement(self, name, attrs):
        if name == "p:item":
            self.ids.append(attrs.get("id"))
        elif name == "name":
            self._in_name = True
            self._buf = []

    def endElement(self, name):
        if name == "name":
            self._in_name = False
            self.names.append("".join(self._buf))

    def characters(self, content):
        if self._in_name:
            self._buf.append(content)


def parse_sax(text):
    xml.sax.parseString(text.encode("utf-8"), _ItemCollector())


def workload_sax(text):
    handler = _ItemCollector()
    xml.sax.parseString(text.encode("utf-8"), handler)
    assert len(handler.ids) == len(handler.names)
    return len(handler.ids)


PARSERS["sax"] = (parse_sax, workload_sax)


# --- xml.parsers.expat -----------------------------------------------------

def parse_expat(text):
    p = xml.parsers.expat.ParserCreate(namespace_separator=" ")
    p.Parse(text, True)


def workload_expat(text):
    ids, names = [], []
    state = {"in_name": False, "buf": []}

    def start(name, attrs):
        if name.endswith(" item") or name == "item":
            ids.append(attrs.get("id"))
        if name == "name":
            state["in_name"] = True
            state["buf"] = []

    def end(name):
        if name == "name":
            state["in_name"] = False
            names.append("".join(state["buf"]))

    def chardata(data):
        if state["in_name"]:
            state["buf"].append(data)

    p = xml.parsers.expat.ParserCreate(namespace_separator=" ")
    p.StartElementHandler = start
    p.EndElementHandler = end
    p.CharacterDataHandler = chardata
    p.Parse(text, True)
    assert len(ids) == len(names)
    return len(ids)


PARSERS["expat"] = (parse_expat, workload_expat)


# --- xmltodict --------------------------------------------------------------

if xmltodict is not None:
    def parse_xmltodict(text):
        xmltodict.parse(text, process_namespaces=True)

    def workload_xmltodict(text):
        doc = xmltodict.parse(text, process_namespaces=True)
        items = doc["catalog"]["urn:products:item"]
        ids = [i.get("@id") for i in items]
        names = [i.get("name", {}).get("#text") for i in items]
        assert len(ids) == len(names)
        return len(ids)

    PARSERS["xmltodict"] = (parse_xmltodict, workload_xmltodict)


# --- timing -----------------------------------------------------------------

def _bench(fn, corpus, repeat):
    for doc in corpus:  # warmup + correctness
        fn(doc)
    samples = []
    for _ in range(repeat):
        start = time.perf_counter()
        for doc in corpus:
            fn(doc)
        samples.append(time.perf_counter() - start)
    return min(samples), statistics.median(samples)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--docs", type=int, default=10, help="number of documents")
    ap.add_argument("--items", type=int, default=500, help="items per document")
    ap.add_argument("--repeat", type=int, default=5)
    args = ap.parse_args()

    corpus = make_corpus(args.docs, args.items)
    total_bytes = sum(len(d) for d in corpus)

    print("Corpus: %d docs x %d items, %.2f MB total, %d repeats"
          % (args.docs, args.items, total_bytes / 1e6, args.repeat))
    print()
    print("%-14s | %22s | %22s" % ("parser", "parse (best MB/s)", "workload (best MB/s)"))
    print("-" * 66)

    for name, (parse_fn, workload_fn) in PARSERS.items():
        best_p, _ = _bench(parse_fn, corpus, args.repeat)
        parse_rate = total_bytes / best_p / 1e6

        if workload_fn is not None:
            best_w, _ = _bench(workload_fn, corpus, args.repeat)
            work_rate = total_bytes / best_w / 1e6
            work_cell = "%6.3fs  %7.1f" % (best_w, work_rate)
        else:
            work_cell = "n/a"

        print("%-14s | %6.3fs  %13.1f | %22s"
              % (name, best_p, parse_rate, work_cell))


if __name__ == "__main__":
    main()
