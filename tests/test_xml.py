import pytest

from selectolax.lexbor import (
    LexborHTMLParser,
    LexborXMLParseError,
    LexborXMLParser,
    SelectolaxError,
)


def test_parser_basic():
    parser = LexborXMLParser("<root><a>x</a></root>")
    assert isinstance(parser, LexborXMLParser)
    assert parser.root is not None
    assert parser.root.tag == "root"

    with pytest.raises(TypeError):
        LexborXMLParser(123)


def test_serialization_roundtrip():
    xml = '<root a="1"><Item x="y">hi</Item><item/></root>'
    assert LexborXMLParser(xml).html == xml


def test_self_closing_elements():
    assert LexborXMLParser("<r><a/></r>").html == "<r><a/></r>"
    assert LexborXMLParser("<r><a>x</a></r>").html == "<r><a>x</a></r>"


def test_case_sensitive_tags():
    parser = LexborXMLParser("<Root><Item/><item/></Root>")
    assert len(parser.css("Item")) == 1
    assert len(parser.css("item")) == 1
    assert len(parser.css("ITEM")) == 0
    assert len(parser.css("ROOT")) == 0
    assert parser.tags("Item") == [parser.css_first("Item")]


def test_case_sensitive_attributes():
    parser = LexborXMLParser('<r Data="1" data="2"/>')
    root = parser.root
    assert root.attributes == {"Data": "1", "data": "2"}


def test_strict_errors():
    malformed = [
        "<a><b></a></b>",   # tag mismatch
        "<a>",              # unclosed element
        "",                 # missing root
        "<a></a><b/>",      # multiple roots
        "x<a/>",            # text before root
        "<a></a>text",      # text after root
        "<a x=1/>",         # unquoted attribute
        '<a x="1" x="2"/>',  # duplicate attribute
        "<a>&undef;</a>",   # undefined entity
        "<a><b/></c>",      # unmatched end tag
    ]
    for doc in malformed:
        with pytest.raises(SelectolaxError):
            LexborXMLParser(doc)


def test_strict_error_message():
    with pytest.raises(SelectolaxError, match="tag_mismatch"):
        LexborXMLParser("<a><b></a></b>")


def test_recover_mode():
    parser = LexborXMLParser("<a><b></a></b>", recover=True)
    assert parser.recover is True
    assert parser.html == "<a><b/></a>"
    names = [e.name for e in parser.errors]
    assert "tag_mismatch" in names
    for err in parser.errors:
        assert isinstance(err, LexborXMLParseError)
        assert err.line >= 1
        assert err.col >= 1


def test_recover_off_by_default():
    parser = LexborXMLParser("<a/>")
    assert parser.recover is False
    assert parser.errors == []


def test_xml_declaration():
    parser = LexborXMLParser('<?xml version="1.0" standalone="yes"?><r/>')
    assert parser.standalone is True
    assert parser.errors == []

    parser = LexborXMLParser('<?xml version="1.0"?><r/>')
    assert parser.standalone is False


def test_doctype_comment_pi_cdata():
    doc = "<!DOCTYPE r><!--c--><?pi data?><r><![CDATA[<raw>&amp;]]></r>"
    parser = LexborXMLParser(doc)
    assert parser.html == doc
    assert parser.root.text() == "<raw>&amp;"


def test_doctype_external_ids():
    assert (LexborXMLParser('<!DOCTYPE r SYSTEM "http://x/y"><r/>').html
            == '<!DOCTYPE r SYSTEM "http://x/y"><r/>')
    assert (LexborXMLParser('<!DOCTYPE r PUBLIC "-//P//D" "http://x/dtd"><r/>').html
            == '<!DOCTYPE r PUBLIC "-//P//D" "http://x/dtd"><r/>')


def test_entities_never_expanded_from_dtd():
    # Internal subsets are skipped entirely; entities declared there are
    # never registered, so &x; is undefined (strict) or literal (recover).
    with pytest.raises(SelectolaxError):
        LexborXMLParser('<!DOCTYPE r [<!ENTITY x "EXPANDED">]><r>&x;</r>')

    parser = LexborXMLParser(
        '<!DOCTYPE r [<!ENTITY x "EXPANDED">]><r>&x;</r>', recover=True)
    assert "EXPANDED" not in parser.root.text()
    assert parser.root.text() == "&x;"

    # External entity declarations are never fetched.
    parser = LexborXMLParser(
        '<!DOCTYPE r [<!ENTITY x SYSTEM "file:///etc/passwd">]><r>&x;</r>',
        recover=True)
    assert parser.root.text() == "&x;"


def test_cdata_content_not_parsed():
    parser = LexborXMLParser("<r><![CDATA[<b>not an element</b>]]></r>")
    assert parser.css("b") == []
    assert "<b>not an element</b>" in parser.root.text()


def test_namespaces():
    xml = '<r xmlns:x="urn:x"><x:y x:k="v">t</x:y></r>'
    parser = LexborXMLParser(xml)
    assert parser.html == xml
    # Prefixed names are preserved verbatim.
    y = parser.css_first("x\\:y")
    assert y is not None
    assert y.tag == "x:y"
    assert y.attributes["x:k"] == "v"


def test_namespace_local_name_selector():
    parser = LexborXMLParser('<r xmlns:x="urn:x"><x:y>t</x:y></r>')
    # Local name alone also matches.
    assert parser.css_first("y") is not None


def test_namespace_node_properties():
    parser = LexborXMLParser(
        '<r xmlns:x="urn:x" xmlns="urn:default"><x:y/><z/></r>')
    y = parser.css_first("y")
    assert y.tag == "x:y"
    assert y.prefix == "x"
    assert y.local_name == "y"
    assert y.namespace_uri == "urn:x"

    z = parser.css_first("z")
    assert z.tag == "z"
    assert z.prefix is None
    assert z.local_name == "z"
    assert z.namespace_uri == "urn:default"

    parser = LexborXMLParser("<r/>")
    assert parser.root.namespace_uri is None

    # HTML nodes have no namespace data.
    div = LexborHTMLParser("<div/>").css_first("div")
    assert div.namespace_uri is None
    assert div.prefix is None
    assert div.local_name == "div"


def test_entities():
    parser = LexborXMLParser("<r>a&lt;b&gt;&amp;&quot;&apos;</r>")
    assert parser.root.text() == 'a<b>&"\''


def test_char_refs():
    parser = LexborXMLParser("<r>&#65;&#x42;</r>")
    assert parser.root.text() == "AB"


def test_inner_html():
    parser = LexborXMLParser("<r><a>x</a></r>")
    assert parser.inner_html == "<a>x</a>"
    assert parser.root.inner_html == "<a>x</a>"
    assert parser.css_first("a").inner_html == "x"


def test_node_html():
    parser = LexborXMLParser("<r><a>x</a><b/></r>")
    a = parser.css_first("a")
    assert a.html == "<a>x</a>"
    assert parser.css_first("b").html == "<b/>"
    assert parser.root.html == "<r><a>x</a><b/></r>"


def test_fragment_parsing():
    parser = LexborXMLParser("<a>x</a><b/>", is_fragment=True)
    assert parser.html == "<a>x</a><b/>"


def test_inner_html_setter():
    parser = LexborXMLParser("<r><a x=\"1\">t</a></r>")
    parser.root.inner_html = "<b>inner</b>"
    assert parser.html == "<r><b>inner</b></r>"


def test_attribute_mutation():
    parser = LexborXMLParser("<r><a/></r>")
    a = parser.css_first("a")
    a.attrs["Y"] = "2"
    a.attrs["x:k"] = "v"
    assert a.attributes == {"Y": "2", "x:k": "v"}
    assert parser.html == '<r><a Y="2" x:k="v"/></r>'


def test_attribute_deletion():
    parser = LexborXMLParser('<r><a x="1" y="2"/></r>')
    a = parser.css_first("a")
    del a.attrs["x"]
    assert a.attributes == {"y": "2"}
    assert parser.html == '<r><a y="2"/></r>'


def test_text_aggregation():
    parser = LexborXMLParser("<r>a<b>c</b>d</r>")
    assert parser.text() == "acd"
    assert parser.root.text(deep=True) == "acd"
    assert parser.css_first("b").text() == "c"


def test_node_decompose():
    parser = LexborXMLParser("<r><a>t</a><b/></r>")
    parser.css_first("a").decompose()
    assert parser.html == "<r><b/></r>"


def test_escaping_on_output():
    parser = LexborXMLParser("<r a=\"&lt;\">x&lt;y</r>")
    assert parser.html == '<r a="&lt;">x&lt;y</r>'


def test_html_parser_unaffected():
    # HTML semantics unchanged: case-insensitive, lenient.
    parser = LexborHTMLParser("<DIV><SPAN></DIV>")
    assert parser.css_first("div") is not None
    assert parser.css_first("span") is not None


def test_utf8_content():
    parser = LexborXMLParser("<r>caf\u00e9 \u4e2d\u6587</r>")
    assert parser.root.text() == "caf\u00e9 \u4e2d\u6587"


def test_deep_nesting():
    doc = "<a>" * 50 + "x" + "</a>" * 50
    parser = LexborXMLParser("<r>" + doc + "</r>")
    assert len(parser.css("a")) == 50
