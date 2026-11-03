![selectolax logo](docs/logo.png)

---

A fast HTML5 parser with CSS selectors, written in Cython, using the [Lexbor](https://github.com/lexbor/lexbor) engine.

---

[![PyPI - Version](https://img.shields.io/pypi/v/selectolax?logo=pypi&label=Pypi&logoColor=fff)](https://pypi.org/project/selectolax)
[![PyPI Total Downloads](https://static.pepy.tech/badge/selectolax)](https://pepy.tech/projects/selectolax)
[![CI](https://img.shields.io/github/actions/workflow/status/rushter/selectolax/pythonpackage.yml?branch=master&logo=githubactions&label=CI)](https://github.com/rushter/selectolax/actions/workflows/pythonpackage.yml?query=branch%3Amaster+event%3Apush)
[![GitHub License](https://img.shields.io/github/license/rushter/selectolax?logo=github&label=License)](https://github.com/rushter/selectolax/blob/master/LICENSE)

---

## Installation

From PyPI using pip:

```bash
pip install selectolax
```

If installation fails due to compilation errors, you may need to install [Cython](https://github.com/cython/cython):

```bash
pip install selectolax[cython]
```

This usually happens when you try to install an outdated version of selectolax on a newer version of Python.

Development version from GitHub:

```bash
git clone --recursive  https://github.com/rushter/selectolax
cd selectolax
pip install -r requirements_dev.txt
python setup.py install
```

How to compile selectolax while developing:

```bash
make clean
make dev
```

## Basic examples

Here are some basic examples to get you started with selectolax:

Parsing HTML and extracting text:

```python
from selectolax.lexbor import LexborHTMLParser

html = """
<h1 id="title" data-updated="20201101">Hi there</h1>
<div class="post">Lorem Ipsum is simply dummy text of the printing and typesetting industry. </div>
<div class="post">Lorem ipsum dolor sit amet, consectetur adipiscing elit.</div>
"""
tree = LexborHTMLParser(html)

print(tree.css_first('h1#title').text())
# 'Hi there'

print(tree.css_first('h1#title').attributes)
# {'id': 'title', 'data-updated': '20201101'}

print([node.text() for node in tree.css('.post')])
# ['Lorem Ipsum is simply dummy text of the printing and typesetting industry. ',
#  'Lorem ipsum dolor sit amet, consectetur adipiscing elit.']
```

### Using advanced CSS selectors

```python
from selectolax.lexbor import LexborHTMLParser

html = "<div><p id=p1><p id=p2><p id=p3><a>link</a><p id=p4><p id=p5>text<p id=p6></div>"
selector = "div > :nth-child(2n+1):not(:has(a))"

for node in LexborHTMLParser(html).css(selector):
    print(node.attributes, node.text(), node.tag)
    print(node.parent.tag)
    print(node.html)
# {'id': 'p1'}  p
# div
# <p id="p1"></p>
# {'id': 'p5'} text p
# div
# <p id="p5">text</p>
```

#### Using `lexbor-contains` CSS pseudo-class to match text

```python
from selectolax.lexbor import LexborHTMLParser
html = "<div><p>hello </p><p id='main'>lexbor is AwesOme</p></div>"
parser = LexborHTMLParser(html)
# Case-insensitive search
results = parser.css('p:lexbor-contains("awesome" i)')
# Case-sensitive search
results = parser.css('p:lexbor-contains("AwesOme")')
assert len(results) == 1
assert results[0].text() == "lexbor is AwesOme"
```

* [More examples](https://selectolax.readthedocs.io/en/latest/examples.html)

## Parsing XML

`LexborXMLParser` applies the same DOM and CSS-selector API to XML documents.
Unlike the HTML parser it is strict by default — malformed XML raises
`SelectolaxError` — and it preserves case: `Item` and `item` are different
elements, attributes keep their original spelling, and selectors match
case-sensitively.

```python
from selectolax.lexbor import LexborXMLParser

tree = LexborXMLParser('<catalog xmlns:p="urn:p"><p:item id="1">n</p:item></catalog>')
print(tree.css_first('p\\:item').attributes)  # escaped colon selects by qualified name
# {'id': '1'}
print(tree.css_first('item').text())          # local name also works
# 'n'
```

Pass `recover=True` to build a best-effort tree instead of raising; every
violation the parser repaired is then listed in `parser.errors`
(`LexborXMLParseError` records with `name`, `offset`, `line`, `col`).

External entities and DTD processing are not performed: a `<!DOCTYPE>`
declaration is preserved in the tree, but it is never fetched or expanded.
Namespace prefixes are resolved during parsing, but CSS namespace
selectors (`p|item`) are not supported — use `p\:item` or the local name.

## Simple Benchmark

* Extract title, links, scripts and a meta tag from main pages of top 754 domains. See `examples/benchmark.py` for more information.

| Package                       | Time      |
|-------------------------------|-----------|
| Beautiful Soup (html.parser)  | 61.02 sec.|
| lxml / Beautiful Soup (lxml)  | 9.09 sec. |
| html5_parser                  | 16.10 sec.|
| selectolax (Lexbor)           | 2.39 sec. |

## Links

* [Changelog](CHANGES.md)
* [selectolax API reference and examples](https://selectolax.readthedocs.io/en/latest/index.html)
* [Video introduction to web scraping using selectolax](https://youtu.be/HpRsfpPuUzE)
* [How to Scrape 7k Products with Python using selectolax and httpx](https://www.youtube.com/watch?v=XpGvq755J2U)
* [Lexbor benchmark](https://lexborisov.github.io/benchmark-html-parsers/)
* [Python benchmark](https://rushter.com/blog/python-fast-html-parser/)
* [Another Python benchmark](https://www.peterbe.com/plog/selectolax-or-pyquery)
* [Universal interface to lxml and selectolax](https://github.com/lorien/domselect)

## License

* lexbor engine — [Apache-2.0 license](https://github.com/lexbor/lexbor?tab=Apache-2.0-1-ov-file#readme)
* selectolax - [MIT](https://github.com/rushter/selectolax/blob/master/LICENSE)


## Contributors

Thanks to all the contributors of selectolax!

<a href="https://github.com/rushter/selectolax/graphs/contributors">
  <img src="https://contrib.rocks/image?repo=rushter/selectolax" />
</a>
