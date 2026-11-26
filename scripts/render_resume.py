#!/usr/bin/env python3
"""Render an explicitly selected Markdown resume with the bundled document runtime.

The source controls identity and contacts. Use the public source for publication;
private output is never selected automatically for upload.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess

from docx import Document
from docx.oxml.ns import qn
from docx.shared import Pt
from PIL import Image, ImageOps
from pypdf import PdfReader
import generate_resume as design


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--name', default='AI_Infra_Resume')
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    source = args.source.read_text()
    lines = [line.rstrip() for line in source.splitlines() if line.strip()]
    if '面试深挖自查表' in source:
        raise ValueError('Interview notes must remain separate from the resume')
    trials = []
    for index, (points, spacing) in enumerate([(11,1.28),(11,1.30),(11,1.32),(11,1.34),(11,1.26),(10.75,1.28)]):
        folder = args.out / 'qa' / ('trial-' + str(index))
        folder.mkdir(parents=True, exist_ok=True)
        docx = folder / (args.name + '.docx')
        pdf = docx.with_suffix('.pdf')
        design.build_docx(lines, docx, points, spacing, 1.25, 1.45)
        document = Document(docx)
        document.core_properties.title = lines[0].lstrip('# ')
        for paragraph in document.paragraphs:
            if paragraph.style.name == 'Resume Bullet':
                props = paragraph._p.get_or_add_pPr()
                number = props.find(qn('w:numPr'))
                if number is not None:
                    props.remove(number)
                paragraph.paragraph_format.left_indent = Pt(18)
                paragraph.paragraph_format.first_line_indent = Pt(-18)
                paragraph.paragraph_format.tab_stops.clear_all()
                paragraph.paragraph_format.tab_stops.add_tab_stop(Pt(18))
                marker = paragraph.add_run('•\t')
                paragraph._p.insert(1, marker._r)
        for name in ['Title','Heading 1','Heading 2','Heading 3']:
            props = document.styles[name]._element.get_or_add_pPr()
            for border in list(props.findall(qn('w:pBdr'))):
                props.remove(border)
        document.save(docx)
        design.export_pdf(docx, pdf)
        reader = PdfReader(pdf)
        trial = {'body_pt':points,'line_spacing':spacing,'pages':len(reader.pages)}
        trials.append(trial)
        print(trial, flush=True)
        if len(reader.pages) != 1:
            continue
        fonts = design.verify_content(reader.pages[0], lines)
        png = folder / 'page.png'
        subprocess.run([str(design.runtime_tool('pdftoppm')),'-r','180','-png','-singlefile',str(pdf),str(png.with_suffix(''))],check=True)
        with Image.open(png) as image:
            mask = ImageOps.invert(image.convert('L')).point(lambda v:255 if v>=32 else 0)
            box = mask.getbbox()
            coverage = (box[3]-box[1])/image.height
        trial['vertical_coverage'] = coverage
        if not .86 <= coverage <= .95:
            continue
        for file in [docx,pdf]:
            shutil.copyfile(file,args.out/file.name)
        shutil.copyfile(png,args.out/'resume_preview.png')
        report = {'pages':1,'full_source_text_match':True,'embedded_fonts':fonts,
                  'source_sha256':hashlib.sha256(args.source.read_bytes()).hexdigest(),
                  'pdf_sha256':hashlib.sha256(pdf.read_bytes()).hexdigest(),
                  'settings':trial,'trials':trials,'visual_review':'pending canonical render review'}
        (args.out/'layout-validation.json').write_text(json.dumps(report,ensure_ascii=False,indent=2))
        return
    raise RuntimeError('No readable one-page layout; shorten source instead of shrinking further')


if __name__ == '__main__':
    main()
