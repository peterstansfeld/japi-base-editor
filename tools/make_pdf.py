#!/usr/bin/env python3
"""Build JBE_MANUAL.pdf from JBE_MANUAL.md.

Same concept as the Japi Base platform manual: a self-contained HTML file
(the Roboto Mono font base64-embedded) printed to PDF by headless Chrome, so
the whole manual is set in one clean, dyslexia-friendly monospace face and
looks identical wherever it is built. Dependency: google-chrome only -- the
Markdown is converted to HTML by the small dependency-free parser below, so no
Python packages are needed.

    python3 tools/make_pdf.py        # run from the repo root

Reads  : JBE_MANUAL.md
Writes : JBE_MANUAL.pdf  (and JBE_MANUAL.html as an intermediate, gitignored)
"""
import base64, html, os, re, subprocess

# Self-locating: TOOLS is this script's directory, ROOT the repo root above it.
TOOLS = os.path.dirname(os.path.abspath(__file__)) + "/"
ROOT  = os.path.abspath(os.path.join(TOOLS, "..")) + "/"
SRC   = ROOT + "JBE_MANUAL.md"
HTMLF = ROOT + "JBE_MANUAL.html"
PDF   = ROOT + "JBE_MANUAL.pdf"


def b64(path):
    with open(path, "rb") as f:
        return base64.b64encode(f.read()).decode()


# ---- inline Markdown: code spans, bold, italic, links ---------------------
def inline(text):
    """Convert the inline markup of a single (already-joined) text run."""
    text = html.escape(text)
    text = re.sub(r'`([^`]+)`', r'<code>\1</code>', text)
    text = re.sub(r'\*\*([^*]+?)\*\*', r'<strong>\1</strong>', text)
    text = re.sub(r'(?<![\*\w])\*([^*]+?)\*(?![\*\w])', r'<em>\1</em>', text)
    text = re.sub(r'\[([^\]]+)\]\(([^)]+)\)', r'<a href="\2">\1</a>', text)
    return text


def is_table_sep(line):
    """True for a GitHub pipe-table separator row such as |---|:--:|---|."""
    s = line.strip()
    return bool(s) and set(s) <= set("|:- ") and "-" in s


def cells(row):
    """Split a pipe-table row into trimmed cell strings."""
    s = row.strip()
    if s.startswith("|"):
        s = s[1:]
    if s.endswith("|"):
        s = s[:-1]
    return [c.strip() for c in s.split("|")]


# ---- block Markdown -> HTML ----------------------------------------------
def convert(md):
    lines = md.split("\n")
    out, i, n = [], 0, len(lines)
    first_h1_seen = False
    while i < n:
        ln = lines[i]
        s = ln.strip()

        if not s:                                   # blank line
            i += 1
            continue

        if s in ("---", "***", "___"):              # horizontal rule
            out.append("<hr>")
            i += 1
            continue

        m = re.match(r'(#{1,6})\s+(.*)', s)         # heading
        if m:
            level = len(m.group(1))
            cls = ""
            if level == 1 and not first_h1_seen:    # first H1 = cover title
                cls, first_h1_seen = ' class="title"', True
            elif level == 1:
                cls = ' class="pb"'                 # each later H1 starts a page
            out.append(f"<h{level}{cls}>{inline(m.group(2))}</h{level}>")
            i += 1
            continue

        if s.startswith("|") and i + 1 < n and is_table_sep(lines[i + 1]):
            head = cells(ln)
            i += 2
            body = []
            while i < n and lines[i].strip().startswith("|"):
                body.append(cells(lines[i]))
                i += 1
            t = ["<table><thead><tr>"]
            t += [f"<th>{inline(c)}</th>" for c in head]
            t.append("</tr></thead><tbody>")
            for row in body:
                t.append("<tr>" + "".join(f"<td>{inline(c)}</td>"
                                          for c in row) + "</tr>")
            t.append("</tbody></table>")
            out.append("".join(t))
            continue

        if re.match(r'[-*]\s+', s):                 # unordered list
            items = []
            while i < n and re.match(r'[-*]\s+', lines[i].strip()):
                items.append(re.sub(r'^[-*]\s+', '', lines[i].strip()))
                i += 1
            out.append("<ul>" + "".join(f"<li>{inline(it)}</li>"
                                        for it in items) + "</ul>")
            continue

        para = []                                   # paragraph: join soft wraps
        while i < n and lines[i].strip() and not re.match(
                r'(#{1,6}\s|[-*]\s|\|)', lines[i].strip()) \
                and lines[i].strip() not in ("---", "***", "___"):
            para.append(lines[i].strip())
            i += 1
        out.append(f"<p>{inline(' '.join(para))}</p>")
    return "\n".join(out)


# ---- assemble + print -----------------------------------------------------
face = lambda w, f: (f"@font-face{{font-family:'Roboto Mono';src:url(data:font/"
                     f"woff2;base64,{b64(TOOLS + 'fonts/' + f)}) format('woff2');"
                     f"font-weight:{w};font-style:normal;}}")
css = face(400, "RobotoMono-400.woff2") + face(700, "RobotoMono-700.woff2") + """
@page { size: A4; margin: 16mm 14mm 16mm 20mm; }
html { -webkit-print-color-adjust: exact; }
body { font-family:'Roboto Mono',monospace; color:#111; font-size:10pt;
       line-height:1.5; }
h1.title { font-weight:700; font-size:24pt; text-align:center;
           margin:40mm 0 10mm; letter-spacing:1px; }
h1 { font-weight:700; font-size:16pt; margin:18pt 0 7pt;
     border-bottom:2px solid #888; padding-bottom:3pt; }
h2 { font-weight:700; font-size:13pt; margin:14pt 0 5pt; }
h3 { font-weight:700; font-size:11pt; margin:11pt 0 4pt; color:#222; }
p  { margin:0 0 7pt; }
ul { margin:0 0 7pt 0; padding-left:7mm; }
li { margin:1pt 0; }
code { background:#f0f0f0; padding:0 2px; border-radius:2px; }
hr { border:0; border-top:1px solid #ccc; margin:9pt 0; }
table { border-collapse:collapse; margin:7pt 0; font-size:9.2pt;
        page-break-inside:avoid; }
th, td { border:1px solid #bbb; padding:3px 7px; text-align:left;
         vertical-align:top; }
th { background:#eee; font-weight:700; }
h1.pb { page-break-before: always; }
"""
body = convert(open(SRC, encoding="utf-8").read())
doc = (f"<!doctype html><html><head><meta charset='utf-8'><style>{css}</style>"
       f"</head><body>{body}</body></html>")
open(HTMLF, "w", encoding="utf-8").write(doc)
print("wrote JBE_MANUAL.html (%.1f KB)" % (len(doc) / 1024))

subprocess.run([
    "google-chrome", "--headless=new", "--disable-gpu", "--no-sandbox",
    "--no-pdf-header-footer", f"--print-to-pdf={PDF}", "file://" + HTMLF,
], check=True, timeout=180, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
print("wrote JBE_MANUAL.pdf (%.1f KB)" % (os.path.getsize(PDF) / 1024))
