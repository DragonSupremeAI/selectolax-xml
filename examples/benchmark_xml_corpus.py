"""Benchmark LexborXMLParser against the major Python XML parsers on
real-world XML files.

Unlike ``benchmark_xml.py`` (synthetic corpus), this script takes actual XML
documents — e.g. StackExchange data dumps or PubMed bulk exports — and
measures raw parse time plus a generic extraction workload.

Usage::

    # extract the Id attribute of every <row> (StackExchange dumps)
    python examples/benchmark_xml_corpus.py benchmark_data/se/writers/Posts.xml \
        --tag row --attr Id

    # extract the text of every <PMID> (PubMed bulk export)
    python examples/benchmark_xml_corpus.py benchmark_data/pubmed/pubmed24n1219.xml \
        --tag PMID

Missing parsers are skipped. xmltodict/minidom can be very slow on large
documents — exclude them with ``--parsers``.
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


def make_parsers(tag, attr):
    """Build (parse_fn, workload_fn) pairs collecting ``tag``'s ``attr``
    value, or its text content when ``attr`` is None."""
    parsers = {}

    def parse_selectolax(data):
        LexborXMLParser(data)

    def workload_selectolax(data):
        tree = LexborXMLParser(data)
        nodes = tree.css(tag)
        if attr:
            out = [n.attributes.get(attr) for n in nodes]
        else:
            out = [n.text() for n in nodes]
        return len(out), sum(len(v or "") for v in out)

    parsers["selectolax"] = (parse_selectolax, workload_selectolax)

    if lxml_etree is not None:
        def parse_lxml(data):
            lxml_etree.fromstring(data)

        def workload_lxml(data):
            root = lxml_etree.fromstring(data)
            if attr:
                out = [n.get(attr) for n in root.iter(tag)]
            else:
                out = [n.text for n in root.iter(tag)]
            return len(out), sum(len(v or "") for v in out)

        parsers["lxml"] = (parse_lxml, workload_lxml)

    def parse_elementtree(data):
        ET.fromstring(data)

    def workload_elementtree(data):
        root = ET.fromstring(data)
        if attr:
            out = [n.attrib.get(attr) for n in root.iter(tag)]
        else:
            out = [n.text for n in root.iter(tag)]
        return len(out), sum(len(v or "") for v in out)

    parsers["ElementTree"] = (parse_elementtree, workload_elementtree)

    if defused_ET is not None:
        def parse_defused(data):
            defused_ET.fromstring(data)

        parsers["defusedxml"] = (parse_defused, None)

    def parse_minidom(data):
        xml.dom.minidom.parseString(data)

    def workload_minidom(data):
        doc = xml.dom.minidom.parseString(data)
        nodes = doc.getElementsByTagName(tag)
        if attr:
            out = [n.getAttribute(attr) for n in nodes]
        else:
            out = ["".join(c.data for c in n.childNodes
                           if c.nodeType == c.TEXT_NODE) for n in nodes]
        return len(out), sum(len(v or "") for v in out)

    parsers["minidom"] = (parse_minidom, workload_minidom)

    class _Collector(xml.sax.handler.ContentHandler):
        def __init__(self):
            super().__init__()
            self.out = []
            self._buf = None

        def startElement(self, name, attrs):
            if name == tag:
                if attr:
                    self.out.append(attrs.get(attr))
                else:
                    self._buf = []

        def endElement(self, name):
            if name == tag and self._buf is not None:
                self.out.append("".join(self._buf))
                self._buf = None

        def characters(self, content):
            if self._buf is not None:
                self._buf.append(content)

    def parse_sax(data):
        xml.sax.parseString(data, _Collector())

    def workload_sax(data):
        h = _Collector()
        xml.sax.parseString(data, h)
        return len(h.out), sum(len(v or "") for v in h.out)

    parsers["sax"] = (parse_sax, workload_sax)

    def _expat(data, collect=False):
        out = []
        buf = []

        def start(name, attrs):
            if name == tag:
                if attr:
                    out.append(attrs.get(attr))
                else:
                    buf.clear()
                    buf.append(True)  # marker: inside target element

        def end(name):
            if name == tag and not attr and buf and buf[0] is True:
                out.append("".join(buf[1:]))
                buf.clear()

        def chardata(d):
            if buf and buf[0] is True:
                buf.append(d)

        p = xml.parsers.expat.ParserCreate()
        p.StartElementHandler = start
        p.EndElementHandler = end
        p.CharacterDataHandler = chardata
        p.Parse(data, True)
        return len(out), sum(len(v or "") for v in out)

    def parse_expat(data):
        p = xml.parsers.expat.ParserCreate()
        p.Parse(data, True)

    parsers["expat"] = (parse_expat, lambda d: _expat(d, True))

    if xmltodict is not None:
        def parse_xmltodict(data):
            xmltodict.parse(data)

        parsers["xmltodict"] = (parse_xmltodict, None)

    return parsers


def bench(fn, corpus, repeat):
    expected = fn(corpus[0])  # warmup + capture workload result
    samples = []
    for _ in range(repeat):
        start = time.perf_counter()
        for doc in corpus:
            fn(doc)
        samples.append(time.perf_counter() - start)
    return min(samples), statistics.median(samples), expected


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("files", nargs="+", help="XML files to benchmark")
    ap.add_argument("--tag", required=True, help="element name to extract")
    ap.add_argument("--attr", default=None,
                    help="attribute to collect (omit to collect text)")
    ap.add_argument("--repeat", type=int, default=3)
    ap.add_argument("--parsers", default=None,
                    help="comma-separated subset of parsers to run")
    args = ap.parse_args()

    corpus = [open(path, "rb").read() for path in args.files]
    total_bytes = sum(len(d) for d in corpus)
    label = "%s[@%s]" % (args.tag, args.attr) if args.attr else args.tag

    print("Corpus: %d file(s), %.2f MB total, %d repeats, workload=%s"
          % (len(corpus), total_bytes / 1e6, args.repeat, label))
    for path, data in zip(args.files, corpus):
        print("  %-60s %8.1f MB" % (path, len(data) / 1e6))
    print()
    print("%-14s | %22s | %22s | %s"
          % ("parser", "parse (best MB/s)", "workload (best MB/s)",
             "extracted"))
    print("-" * 78)

    parsers = make_parsers(args.tag, args.attr)
    if args.parsers:
        keep = set(args.parsers.split(","))
        parsers = {k: v for k, v in parsers.items() if k in keep}

    for name, (parse_fn, workload_fn) in parsers.items():
        try:
            best_p, _, _ = bench(parse_fn, corpus, args.repeat)
        except Exception as e:
            print("%-14s | parse failed: %s" % (name, e))
            continue
        parse_rate = total_bytes / best_p / 1e6

        if workload_fn is not None:
            try:
                best_w, _, result = bench(workload_fn, corpus, args.repeat)
                work_rate = total_bytes / best_w / 1e6
                work_cell = "%6.3fs  %13.1f" % (best_w, work_rate)
                n_items, total_len = result
                extracted = "%d items / %d chars" % (n_items, total_len)
            except Exception as e:
                work_cell = "failed: %s" % e
                extracted = ""
        else:
            work_cell = "n/a"
            extracted = ""

        print("%-14s | %6.3fs  %13.1f | %22s | %s"
              % (name, best_p, parse_rate, work_cell, extracted))


if __name__ == "__main__":
    main()
