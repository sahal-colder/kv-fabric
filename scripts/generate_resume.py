"""Build the A4 resume from docs/resume_ai_infra.md.

Uses python-docx and the bundled LibreOffice renderer on native Linux.
"""

import argparse
import difflib
import os
import re
import shutil
import subprocess
from pathlib import Path
from tempfile import TemporaryDirectory

from docx import Document
from docx.enum.style import WD_STYLE_TYPE
from docx.opc.constants import RELATIONSHIP_TYPE
from docx.oxml import OxmlElement, parse_xml
from docx.oxml.ns import nsdecls, qn
from docx.shared import Cm, Pt, RGBColor
from PIL import Image, ImageOps
from pypdf import PdfReader


DOCS = Path(__file__).resolve().parents[1] / "docs"
FILENAME = "AI_Infra_Resume"
LATIN = "Noto Sans CJK SC"
CHINESE = "Noto Sans CJK SC"
BLUE = "1F4E79"
INLINE = re.compile(
    r"(?P<bold>\*\*.+?\*\*)|(?P<italic>\*[^*]+\*)|(?P<code>`[^`]+`)"
    r"|(?P<link>https://[^\s｜]+|[\w.+-]+@[\w.-]+\.[A-Za-z]{2,})"
)


def add_runs(paragraph, content, *, bold=False, italic=False):
    position = 0
    for match in INLINE.finditer(content):
        if match.start() > position:
            run = paragraph.add_run(content[position:match.start()])
            run.bold, run.italic = bold or None, italic or None
        value = match.group()
        if match.lastgroup == "bold":
            add_runs(paragraph, value[2:-2], bold=True, italic=italic)
        elif match.lastgroup == "italic":
            add_runs(paragraph, value[1:-1], bold=bold, italic=True)
        elif match.lastgroup == "code":
            run = paragraph.add_run(value[1:-1])
            run.bold, run.italic = True, italic or None
        else:
            hyperlink = OxmlElement("w:hyperlink")
            target = value if value.startswith("https://") else f"mailto:{value}"
            relation = paragraph.part.relate_to(target, RELATIONSHIP_TYPE.HYPERLINK, is_external=True)
            hyperlink.set(qn("r:id"), relation)
            run = paragraph.add_run(value)
            run.bold, run.italic = bold or None, italic or None
            run.font.color.rgb = RGBColor.from_string(BLUE)
            hyperlink.append(run._element)
            paragraph._p.append(hyperlink)
        position = match.end()
    if position < len(content):
        run = paragraph.add_run(content[position:])
        run.bold, run.italic = bold or None, italic or None


def add_numbering(document, fmt, marker, indent):
    numbering = document.part.numbering_part.element
    abstract_id = 1 + max(int(item.get(qn("w:abstractNumId"))) for item in numbering.findall(qn("w:abstractNum")))
    abstract = parse_xml(
        f'<w:abstractNum {nsdecls("w")} w:abstractNumId="{abstract_id}">'
        '<w:multiLevelType w:val="singleLevel"/><w:lvl w:ilvl="0">'
        f'<w:start w:val="1"/><w:numFmt w:val="{fmt}"/><w:lvlText w:val="{marker}"/>'
        f'<w:lvlJc w:val="left"/><w:pPr><w:tabs><w:tab w:val="num" w:pos="{indent}"/>'
        f'</w:tabs><w:ind w:left="{indent}" w:hanging="215"/></w:pPr>'
        f'<w:rPr><w:rFonts w:ascii="{LATIN}" w:hAnsi="{LATIN}"/></w:rPr></w:lvl></w:abstractNum>'
    )
    numbering.insert_element_before(abstract, "w:num")
    return numbering.add_num(abstract_id).numId


def build_docx(lines, path, size, spacing, vertical, horizontal):
    document = Document()
    document.core_properties.title = FILENAME.replace("_", " ")
    document.core_properties.author = lines[0][2:]
    document.core_properties.comments = ""
    section = document.sections[0]
    section.page_width, section.page_height = Cm(21), Cm(29.7)
    section.top_margin = section.bottom_margin = Cm(vertical)
    section.left_margin = section.right_margin = Cm(horizontal)
    grid = section._sectPr.find(qn("w:docGrid"))
    if grid is not None:
        section._sectPr.remove(grid)

    styles = document.styles
    for name in ("Resume Intent", "Resume Contact", "Resume Metadata", "Resume Bullet", "Resume Number"):
        styles.add_style(name, WD_STYLE_TYPE.PARAGRAPH).base_style = styles["Normal"]
    for name, points, before, after, bold, keep_next in (
        ("Normal", size, 0, 2, False, False),
        ("Title", 22, 0, 1, True, True),
        ("Resume Intent", 11, 0, 1, True, True),
        ("Resume Contact", 9.5, 0, 1.5, False, False),
        ("Heading 1", 12, 4.5, 4, True, True),
        ("Heading 2", 11, 0, 1.5, True, True),
        ("Heading 3", size, 2.5, 1.5, True, True),
        ("Resume Metadata", 9.5, 0, 1.5, False, True),
        ("Resume Bullet", size, 0, 2, False, False),
        ("Resume Number", size, 1, 1.5, True, True),
    ):
        style = styles[name]
        style.font.name, style.font.size, style.font.bold = LATIN, Pt(points), bold
        style.font.italic = False
        style.font.color.rgb = RGBColor.from_string(BLUE if name == "Heading 1" else "000000")
        fonts = style._element.get_or_add_rPr().rFonts
        fonts.attrib.clear()
        for key, font in (("ascii", LATIN), ("hAnsi", LATIN), ("cs", LATIN), ("eastAsia", CHINESE)):
            fonts.set(qn(f"w:{key}"), font)
        style._element.rPr.append(parse_xml(f'<w:lang {nsdecls("w")} w:val="en-US" w:eastAsia="zh-CN"/>'))
        layout = style.paragraph_format
        # Noto CJK's font metrics make proportional Word spacing unusually tall.
        # Use an explicit baseline distance for consistent Linux/Word pagination.
        fixed = {"Title": 26, "Heading 1": 15, "Heading 2": 14,
                 "Resume Intent": 14, "Resume Contact": 12, "Resume Metadata": 12}
        layout.line_spacing = Pt(fixed.get(name, points * spacing))
        layout.space_before, layout.space_after = Pt(before), Pt(after)
        layout.keep_with_next, layout.keep_together = keep_next, True
        layout.widow_control = True

    # One point of padding keeps the rule clear of descenders.
    title_ppr = styles["Title"]._element.get_or_add_pPr()
    for border in list(title_ppr.findall(qn("w:pBdr"))):
        title_ppr.remove(border)
    styles["Title"].font.underline = False
    ppr = styles["Heading 1"]._element.get_or_add_pPr()
    ppr.insert_element_before(parse_xml(
        f'<w:pBdr {nsdecls("w")}><w:bottom w:val="single" w:sz="6" w:space="1" w:color="{BLUE}"/></w:pBdr>'
    ), "w:spacing", "w:outlineLvl")

    outer_bullet = add_numbering(document, "bullet", "•", 215)
    nested_bullet = add_numbering(document, "bullet", "•", 430)
    ordered = add_numbering(document, "decimal", "%1.", 215)
    for index, line in enumerate(lines):
        number = None
        if index < 3:
            style = ("Title", "Resume Intent", "Resume Contact")[index]
            content = line[2:] if index == 0 else line
        elif line.startswith(("## ", "### ")):
            heading, content = line.split(" ", 1)
            style = f"Heading {len(heading) - 1}"
        elif line.startswith(("- ", "   - ")):
            style, content = "Resume Bullet", line.lstrip()[2:]
            number = nested_bullet if line.startswith(" ") else outer_bullet
        elif re.match(r"\d+\. ", line):
            style, content, number = "Resume Number", line.split(" ", 1)[1], ordered
        elif line.startswith("**技术栈**"):
            style, content = "Resume Metadata", line
        elif re.fullmatch(r"\*\*[^*]+\*\*", line):
            style, content = "Heading 3", line
        else:
            style, content = "Normal", line
        paragraph = document.add_paragraph(style=style)
        if number is not None:
            num_pr = paragraph._p.get_or_add_pPr().get_or_add_numPr()
            num_pr.get_or_add_ilvl().val = 0
            num_pr.get_or_add_numId().val = number
        add_runs(paragraph, content)
    document.save(path)


def runtime_tool(name):
    compatibility = DOCS / "benchmarks/native-linux-final/document-runtime-compat.json"
    if compatibility.is_file():
        import json
        config = json.loads(compatibility.read_text())
        candidate = Path(config.get("wrapper_directory", "")) / name
        if candidate.is_file():
            return candidate
    runtime = Path.home() / ".cache/codex-runtimes/codex-primary-runtime/dependencies"
    return runtime / "bin/override" / name


def export_pdf(docx, pdf):
    soffice = Path(os.environ.get("KVF_SOFFICE", runtime_tool("soffice")))
    if not soffice.is_file():
        raise RuntimeError("Bundled LibreOffice unavailable; resolve workspace dependencies first")
    with TemporaryDirectory(prefix="kvf-lo-profile-") as profile:
        subprocess.run([
            str(soffice), "-env:UserInstallation=" + Path(profile).as_uri(),
            "--headless", "--convert-to", "pdf", "--outdir", str(pdf.parent), str(docx),
        ], check=True, timeout=90, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    emitted = pdf.parent / (docx.stem + ".pdf")
    if not emitted.is_file():
        raise RuntimeError("LibreOffice emitted no PDF")
    if emitted != pdf:
        shutil.move(emitted, pdf)


def normalized(text):
    return re.sub(r"[\s•\u200b\u00ad]+", "", text)


def verify_content(page, lines):
    expected = normalized("\n".join(
        re.sub(r"[\*`]", "", re.sub(r"^\s*(?:#{1,3}|-) ", "", line)) for line in lines
    ))
    actual = normalized(page.extract_text())
    if actual != expected:
        difference = next(
            (expected[a:b], actual[c:d])
            for tag, a, b, c, d in difflib.SequenceMatcher(None, expected, actual, autojunk=False).get_opcodes()
            if tag != "equal"
        )
        raise ValueError(f"PDF text differs from the Markdown: {difference}")
    fonts = page["/Resources"]["/Font"]
    font_names = set()
    for reference in fonts.values():
        font = reference.get_object()
        face = font.get("/DescendantFonts", [font])[0].get_object()
        descriptor = face.get("/FontDescriptor")
        if descriptor is None or not any(key in descriptor for key in ("/FontFile", "/FontFile2", "/FontFile3")):
            raise ValueError(f"PDF font is not embedded: {face['/BaseFont']}")
        font_names.add(str(face["/BaseFont"]).split("+")[-1].lstrip("/"))
    return ", ".join(sorted(font_names))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--preview", type=Path, help="Save the final 200 DPI review image to this PNG path.")
    args = parser.parse_args()
    source = (DOCS / "resume_ai_infra.md").read_text(encoding="utf-8")
    if "面试深挖自查表" in source:
        raise ValueError("The interview checklist must be outside the resume source.")
    lines = [line.rstrip() for line in source.splitlines() if line.strip()]
    with TemporaryDirectory(prefix="kv-fabric-resume-") as scratch:
        work = Path(scratch)
        docx, pdf, png = (work / f"resume.{ext}" for ext in ("docx", "pdf", "png"))
        for size in (10, 10.5, 9.5):
            for spacing, vertical, horizontal in (
                (1.20, 1.30, 1.45),
                (1.22, 1.30, 1.50), (1.26, 1.30, 1.50), (1.30, 1.25, 1.50),
                (1.18, 1.30, 1.45), (1.15, 1.20, 1.40),
            ):
                build_docx(lines, docx, size, spacing, vertical, horizontal)
                export_pdf(docx, pdf)
                reader = PdfReader(pdf)
                count = len(reader.pages)
                print(f"{size:g} pt, {spacing:.2f} line spacing: {count} page(s)", flush=True)
                if count != 1:
                    continue
                page = reader.pages[0]
                dimensions = tuple(float(value) * 25.4 / 72 for value in (page.mediabox.width, page.mediabox.height))
                if any(abs(actual - target) > 0.15 for actual, target in zip(dimensions, (210, 297))):
                    raise ValueError(f"Expected A4, got {dimensions} mm")
                subprocess.run([str(runtime_tool("pdftoppm")), "-r", "200", "-png", "-singlefile", pdf, png.with_suffix("")], check=True)
                with Image.open(png) as image:
                    ink = ImageOps.invert(image.convert("L")).point(lambda value: 255 if value >= 32 else 0)
                    box = ink.getbbox()
                    coverage = (box[3] - box[1]) / image.height
                print(f"  Vertical page coverage: {coverage:.2%}", flush=True)
                if not 0.88 <= coverage <= 0.95:
                    continue
                fonts = verify_content(page, lines)
                out = DOCS / "resume_out"
                out.mkdir(exist_ok=True)
                for file in (docx, pdf):
                    shutil.copyfile(file, out / f"{FILENAME}{file.suffix}")
                if args.preview:
                    shutil.copyfile(png, args.preview)
                print(f"A4, {vertical:.2f} cm vertical and {horizontal:.2f} cm horizontal margins")
                print(f"Embedded PDF fonts: {fonts}")
                import json
                (out / "layout-validation.json").write_text(json.dumps({
                    "pages": count, "paper_mm": dimensions, "font": CHINESE,
                    "body_pt": size, "line_spacing": spacing,
                    "body_line_spacing_pt": size * spacing, "line_spacing_rule": "exact",
                    "title_pt": 22, "section_heading_pt": 12, "contact_pt": 9.5,
                    "section_color": "#1F4E79", "section_rule_pt": 0.75,
                    "margins_cm": {"vertical": vertical, "horizontal": horizontal},
                    "ink_vertical_coverage": coverage, "embedded_fonts": fonts,
                    "source": str(DOCS / "resume_ai_infra.md"),
                    "exporter": "bundled LibreOffice; same DOCX and PDF layout",
                }, ensure_ascii=False, indent=2), encoding="utf-8")
                return
    raise RuntimeError("No layout met the one-page A4 and 88–95% coverage requirements.")


if __name__ == "__main__":
    main()
