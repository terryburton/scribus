#!/usr/bin/env python3
"""Manual helper to generate menu.xml entries for the Barcode Reference section.

Parses h2/h3 headings from the BWIPP reference HTML files and
cross-references against the barcode families defined in
barcodegenerator.cpp to produce only menu entries for supported
symbologies.

Usage:
    python3 doc/gen_barcode_menu.py

Output is the <area text="Barcode Reference"> block for menu.xml.
"""

import html
import os
import re

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
DOC_DIR = os.path.join(SCRIPT_DIR, "en")
BARCODE_CPP = os.path.join(
    SCRIPT_DIR, "..", "scribus", "plugins", "barcodegenerator",
    "barcodegenerator.cpp"
)
SYMBOLOGIES_HTML = os.path.join(DOC_DIR, "bwipp-symbologies.html")
GS1AI_HTML = os.path.join(DOC_DIR, "bwipp-gs1ai.html")
OPTIONS_HTML = os.path.join(DOC_DIR, "bwipp-options.html")

# Wiki HTML heading -> Scribus display name, where they differ.
WIKI_TO_DESC: dict[str, str] = {
    "Australia Post 4 State Customer Code": "AusPost 4 State Customer Code",
    "GS1 DataMatrix":                       "GS1 Data Matrix",
    "GS1 DataMatrix Rectangular":           "GS1 Data Matrix Rectangular",
    "GS1 North American Coupon Code":       "GS1 North American Coupon",
    "Interleaved 2 of 5":                   "Interleaved 2 of 5 (ITF)",
    "Mailmark":                             "Royal Mail Mailmark",
    "Japan Post 4 State Barcode":           "Japan Post 4 State Customer Code",
    "MSI Plessey":                          "MSI Modified Plessey",
    "PZN":                                  "Pharmazentralnummer (PZN)",
    "Pharmacode":                           "Pharmaceutical Binary Code",
    "Plessey":                              "Plessey UK",
    "Royal TNT Post 4 state barcode":       "Royal Dutch TPG Post KIX",
    "Two-Track Pharmacode":                 "Two-track Pharmacode",
}

# Wiki heading -> short menu label, where the heading is too verbose.
MENU_DISPLAY_NAME: dict[str, str] = {
    "Australia Post 4 State Customer Code": "Australia Post",
    "Japan Post 4 State Barcode":           "Japan Post",
    "Royal Mail 4 State Customer Code":     "Royal Mail",
    "Royal TNT Post 4 state barcode":       "Royal TNT Post",
}

# h2 families to exclude (not user-accessible in Scribus).
EXCLUDED_FAMILIES: set[str] = {
    "GS1 Application Identifier Standard Format",
    "Partial Symbols",
    "Raw Symbols",
}

Heading = tuple[int, str, str]


def parse_headings(path: str) -> list[Heading]:
    """Parse h2/h3 headings with id anchors from an HTML file."""
    with open(path, encoding="utf-8") as f:
        content = f.read()
    headings: list[Heading] = []
    for m in re.finditer(
        r'<h([23])\s+id="([^"]+)">(.+?)</h\1>', content, re.DOTALL
    ):
        title = html.unescape(re.sub(r'<[^>]+>', '', m.group(3)))
        headings.append((int(m.group(1)), m.group(2), " ".join(title.split())))
    return headings


def parse_supported_names() -> set[str]:
    """Extract barcode display names from bcNames lists in the C++ source."""
    with open(BARCODE_CPP, encoding="utf-8") as f:
        source = f.read()
    names: set[str] = set()
    for m in re.finditer(r'"([^"]+)"', source[source.find("bcNames"):]):
        name = m.group(1)
        if not name.startswith("Select a "):
            names.add(name)
    return names


def is_supported(title: str, supported: set[str]) -> bool:
    """Check whether a wiki heading corresponds to a supported barcode."""
    return title in supported or WIKI_TO_DESC.get(title) in supported


def xml_escape(text: str) -> str:
    """Escape text for use in XML attribute values."""
    return (text.replace("&", "&amp;").replace('"', "&quot;")
                .replace("<", "&lt;").replace(">", "&gt;"))


def submenuitem(text: str, file: str, *, closed: bool = True) -> str:
    """Format a <submenuitem> element."""
    escaped = xml_escape(text)
    end = "/>" if closed else ">"
    return f'\t\t<submenuitem text="{escaped}" file="{file}"{end}'


def generate_menu() -> str:
    """Generate the Barcode Reference area block for menu.xml."""
    supported = parse_supported_names()
    sym_headings = parse_headings(SYMBOLOGIES_HTML)
    opt_headings = parse_headings(OPTIONS_HTML)

    lines = ['\t<area text="Barcode Reference" file="bwipp-symbologies.html">']

    current_family: str | None = None
    for level, anchor, title in sym_headings:
        if level == 2:
            if current_family is not None:
                lines.append("\t\t</submenuitem>")
            if title in EXCLUDED_FAMILIES:
                current_family = None
                continue
            current_family = title
            ft = title if title.endswith("Symbols") else title + " Symbols"
            lines.append(submenuitem(ft, f"bwipp-symbologies.html#{anchor}", closed=False))
        elif level == 3 and current_family is not None:
            if is_supported(title, supported):
                label = MENU_DISPLAY_NAME.get(title, title)
                lines.append("\t" + submenuitem(label, f"bwipp-symbologies.html#{anchor}"))

    if current_family is not None:
        lines.append("\t\t</submenuitem>")

    lines.append(submenuitem("GS1 AI Standard Format", "bwipp-gs1ai.html"))
    lines.append(submenuitem("General Options", "bwipp-options.html", closed=False))
    for level, anchor, title in opt_headings:
        if level == 2:
            lines.append("\t" + submenuitem(title, f"bwipp-options.html#{anchor}"))
    lines.append("\t\t</submenuitem>")

    lines.append("\t</area>")
    return "\n".join(lines)


if __name__ == "__main__":
    print(generate_menu())
