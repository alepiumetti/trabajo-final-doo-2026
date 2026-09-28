#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Genera docs/Informe_Tecnico.pdf a partir de docs/Informe_Tecnico.md.

El .md NO se modifica: la portada, el índice y los estilos se inyectan en el
HTML intermedio, que se escribe en un directorio temporal.

Uso:
    python3 docs/md2pdf.py                      # usa el motor por defecto (weasy)
    python3 docs/md2pdf.py --engine lo          # fallback: LibreOffice
    python3 docs/md2pdf.py --salida otro.pdf

Dependencias: markdown, pygments, weasyprint.
El script se re-ejecuta solo con un intérprete que tenga weasyprint, si
encuentra uno. Ver la nota de instalación al final de este archivo.
"""

import argparse
import os
import re
import subprocess
import sys
import tempfile
from html.parser import HTMLParser

# --------------------------------------------------------------------------
# Motor: si este intérprete no tiene weasyprint, buscamos uno que sí.
# --------------------------------------------------------------------------
CANDIDATOS_PYTHON = [
    os.environ.get("MD2PDF_PYTHON", ""),
    os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".venv-md2pdf", "bin", "python"),
    "/tmp/opencode/md2pdf/venv/bin/python",
]


def _reexec_con_weasyprint() -> None:
    """Si falta weasyprint, delegar el trabajo a un intérprete que lo tenga."""
    try:
        import weasyprint  # noqa: F401
        return
    except ImportError:
        pass

    for cand in CANDIDATOS_PYTHON:
        if not cand:
            continue
        cand = os.path.abspath(cand)
        if not os.path.exists(cand):
            continue
        try:
            r = subprocess.run(
                [cand, "-c", "import weasyprint"], capture_output=True, timeout=60
            )
        except (OSError, subprocess.SubprocessError):
            continue
        if r.returncode == 0:
            os.execv(cand, [cand, os.path.abspath(__file__)] + sys.argv[1:])

    sys.stderr.write(
        "No se encuentra weasyprint.\n"
        "Para instalarlo en un venv aislado (sin sudo, no toca el sistema):\n\n"
        "    python3 -m venv --without-pip /tmp/opencode/md2pdf/venv\n"
        "    curl -fsSL https://bootstrap.pypa.io/get-pip.py -o /tmp/gp.py\n"
        "    /tmp/opencode/md2pdf/venv/bin/python /tmp/gp.py\n"
        "    /tmp/opencode/md2pdf/venv/bin/python -m pip install weasyprint "
        '"markdown>=3.5" "pygments>=2.17"\n\n'
        "O usá --engine lo para generar el PDF con LibreOffice.\n"
    )
    sys.exit(1)


_reexec_con_weasyprint()

import markdown  # noqa: E402
from pygments import highlight  # noqa: E402
from pygments.formatters import HtmlFormatter  # noqa: E402
from pygments.lexers import get_lexer_by_name  # noqa: E402
from pygments.style import Style  # noqa: E402
from pygments.token import string_to_tokentype  # noqa: E402
from pygments.util import ClassNotFound  # noqa: E402

RAIZ = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# --------------------------------------------------------------------------
# Paleta de resaltado de sintaxis
#
# Pensada para impresión: pocos tonos, todos oscuros sobre fondo claro, y con
# diferencia de luminosidad suficiente para seguir distinguiéndose en
# escala de grises. No usamos el tema por defecto de Pygments (muy saturado).
# --------------------------------------------------------------------------
MAPA_TOKENS = {
    "Comment":                "#78877f italic",
    "Comment.Preproc":        "#7a4a9c",
    "Comment.PreprocFile":    "#7a4a9c",
    "Keyword":                "#0b6b62 bold",
    "Keyword.Constant":       "#0b6b62 bold",
    "Keyword.Type":           "#0b6b62",
    "Operator":               "#4a5a56",
    "Operator.Word":          "#0b6b62 bold",
    "String":                 "#9c3a2c",
    "String.Char":            "#9c3a2c",
    "String.Escape":          "#8a5300",
    "String.Other":           "#9c3a2c",
    "Number":                 "#8a5300",
    "Name":                   "#1c2422",
    "Name.Builtin":           "#0b6b62",
    "Name.Builtin.Pseudo":    "#0b6b62",
    "Name.Function":          "#1f4e79",
    "Name.Class":             "#1f4e79",
    "Name.Namespace":         "#1f4e79",
    "Name.Decorator":         "#8a5300",
    "Name.Attribute":         "#1f4e79",
    "Name.Tag":               "#1f4e79",
    "Generic":                "#5a4a7a",
    "Generic.Deleted":        "#9c3a2c",
    "Generic.Inserted":       "#0b6b62",
    "Generic.Emph":           "italic",
    "Generic.Strong":         "bold",
    "Generic.Prompt":         "#78877f",
    "Error":                  "#9c3a2c bold",
}


class EstiloEcoGrid(Style):
    """Pygments exige que las claves del dict sean tokens, no strings."""

    styles = {string_to_tokentype(k): v for k, v in MAPA_TOKENS.items()}


# --------------------------------------------------------------------------
# Hoja de estilo
#
# Objetivo: que se lea bien en pantalla Y impreso. Paginación A4, márgenes
# generosos, texto alineado a la izquierda (el documento tiene muchos términos
# técnicos y código en línea: justificado los deja más sucios), sin guionado.
# --------------------------------------------------------------------------
CSS = """
@page {
  size: A4;
  margin: 24mm 20mm 20mm 20mm;

  @top-right {
    content: string(seccion);
    font-family: "DejaVu Sans", sans-serif;
    font-size: 8pt;
    color: #93a09b;
    padding-bottom: 2mm;
  }
  @bottom-left {
    content: "EcoGrid \u2014 Informe t\u00e9cnico";
    font-family: "DejaVu Sans", sans-serif;
    font-size: 8pt;
    color: #93a09b;
  }
  @bottom-right {
    content: "p\u00e1g. " counter(page) " de " counter(pages);
    font-family: "DejaVu Sans", sans-serif;
    font-size: 8pt;
    color: #93a09b;
  }
}

/* La portada no lleva encabezado: es la última sección tocada y
   engañaría al lector. */
@page :first {
  @top-right { content: none; }
}

html {
  font-family: "DejaVu Sans", sans-serif;
  font-size: 10.5pt;
  color: #1c2422;
}

body {
  line-height: 1.55;
  text-align: left;
  hyphens: none;
}

p { margin: 0 0 0.75em; orphans: 3; widows: 3; }

/* --- Portada --------------------------------------------------------- */
h1 {
  font-size: 23pt;
  line-height: 1.2;
  color: #0f3d38;
  margin: 0 0 0.15em;
  border: none;
}

/* Subtítulo (la línea en negrita) y la descripción que siguen al h1. */
h1 + p { font-size: 11pt; color: #4d5d58; margin-bottom: 0.5em; }
h1 + p + p { color: #3a4a46; }

/* --- Índice --------------------------------------------------------- */
.indice { break-after: page; }
.indice-titulo {
  font-size: 12pt;
  font-weight: 700;
  color: #0f3d38;
  text-transform: uppercase;
  letter-spacing: 0.06em;
  margin: 0 0 0.9em;
  border-bottom: 1.5px solid #1b5e5a;
  padding-bottom: 0.25em;
}
.indice ul { list-style: none; margin: 0; padding: 0; }
.indice li { margin: 0; }
.indice a {
  /* Nada de display:flex acá: en WeasyPrint 70 un contenedor flex como
     elemento de origen hace que target-counter() devuelva 0. Con display:block
     el leader() reparte los puntos y el número queda al margen derecho. */
  display: block;
  text-decoration: none;
  color: #1c2422;
  font-size: 10pt;
  padding: 0.16em 0;
}
.indice a::after {
  content: leader(".") " " target-counter(attr(href), page);
  color: #93a09b;
  font-variant-numeric: tabular-nums;
}
.indice .nivel3 a { font-size: 9.2pt; color: #55655f; padding-left: 1.4em; }

/* --- Títulos -------------------------------------------------------- */
h2 {
  font-size: 14.5pt;
  color: #0f3d38;
  margin: 2.1em 0 0.7em;
  padding-bottom: 0.22em;
  border-bottom: 1px solid #cfdbd8;
  string-set: seccion content(text);
  break-after: avoid;
}
h3 {
  font-size: 11.5pt;
  color: #1b5e5a;
  font-style: italic;
  font-weight: 700;
  margin: 1.5em 0 0.5em;
  break-after: avoid;
}

/* --- Código --------------------------------------------------------- */
code {
  font-family: "DejaVu Sans Mono", monospace;
  font-size: 0.89em;
  background: #eef3f1;
  color: #0b4a44;
  padding: 0.5px 3.5px;
  border-radius: 2.5px;
}
pre {
  background: #f5f8f7;
  border-left: 3px solid #1b5e5a;
  border-radius: 0 2.5px 2.5px 0;
  padding: 7pt 9pt;
  margin: 0.9em 0 1.1em;
  font-size: 8.4pt;
  line-height: 1.42;
  break-inside: avoid;
  /* Sin esto el diagrama ASCII y los códigos largos se salen del margen. */
  white-space: pre-wrap;
  overflow-wrap: break-word;
}
pre code {
  background: none;
  color: inherit;
  padding: 0;
  border-radius: 0;
  font-size: inherit;
}

/* --- Tablas ------------------------------------------------------------ */
table {
  border-collapse: collapse;
  width: 100%;
  font-size: 9.3pt;
  margin: 1em 0 1.2em;
  break-inside: avoid;
}
thead { display: table-header-group; }
th {
  background: #1b5e5a;
  color: #ffffff;
  text-align: left;
  font-weight: 600;
  padding: 4.5pt 6.5pt;
  border: none;
}
td {
  padding: 3.5pt 6.5pt;
  border-bottom: 1px solid #e0e8e6;
  vertical-align: top;
}
tbody tr:nth-child(even) { background: #f7faf9; }

/* --- Listas, citas, énfasis ------------------------------------------ */
ul, ol { padding-left: 1.35em; margin: 0 0 0.85em; }
li { margin: 0.28em 0; }
strong { font-weight: 700; color: #0f3d38; }
em { font-style: italic; }
blockquote {
  border-left: 3px solid #a3c6bf;
  background: #f5f8f7;
  margin: 1.05em 0;
  padding: 7pt 12pt;
  color: #35453f;
  break-inside: avoid;
}
blockquote p:last-child { margin-bottom: 0; }
hr {
  border: none;
  border-top: 1px solid #d5dfdc;
  margin: 1.5em 0;
}
a { color: #1f4e79; }
"""


# --------------------------------------------------------------------------
# Resaltado de sintaxis
# --------------------------------------------------------------------------
ALIAS_LENGUAJE = {
    "cpp": "cpp",
    "c++": "cpp",
    "hpp": "cpp",
    "sql": "sql",
    "bash": "bash",
    "sh": "bash",
    "shell": "bash",
    "": "text",
    "text": "text",
}


def formatear_codigo(codigo: str, lenguaje: str) -> tuple:
    """Devuelve (html, nombre_del_lexer_usado)."""
    lenguaje = (lenguaje or "").strip().lower()
    lenguaje = ALIAS_LENGUAJE.get(lenguaje, lenguaje)
    try:
        lexer = get_lexer_by_name(lenguaje, stripnl=False)
    except ClassNotFound:
        return "<pre><code>" + escapar(codigo) + "</code></pre>", None
    fmt = HtmlFormatter(style=EstiloEcoGrid, cssclass="hl", nowrap=False)
    return highlight(codigo, lexer, fmt), lenguaje


def escapar(txt: str) -> str:
    return (
        txt.replace("&", "&amp;")
        .replace("<", "&lt;")
        .replace(">", "&gt;")
    )


class Resaltador(HTMLParser):
    """Sustituye cada <pre><code> del HTML de markdown por su versión
    resaltada, usando el class de lenguaje que pone fenced_code."""

    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.partes = []
        self.en_pre = False
        self.lenguaje = None
        self.buffer = []

    def handle_starttag(self, tag, attrs):
        if tag == "pre":
            self.en_pre = True
            self.lenguaje = None
            self.buffer = []
        elif self.en_pre and tag == "code":
            self.lenguaje = dict(attrs).get("class") or ""
            if self.lenguaje.startswith("language-"):
                self.lenguaje = self.lenguaje[len("language-") :]
        elif not self.en_pre:
            # Hay que reemitir la etiqueta: este parser reemplaza los <pre>,
            # pero el resto del HTML tiene que pasar intacto.
            self.partes.append("<" + tag + serializar_atributos(attrs) + ">")

    def handle_startendtag(self, tag, attrs):
        # Etiquetas autocerradas (<hr />): HTMLParser no llama a handle_endtag.
        if not self.en_pre:
            self.partes.append("<" + tag + serializar_atributos(attrs) + "/>")

    def handle_endtag(self, tag):
        if tag == "pre" and self.en_pre:
            codigo = "".join(self.buffer)
            html, _ = formatear_codigo(codigo, self.lenguaje)
            self.partes.append(html)
            self.en_pre = False
            self.lenguaje = None
            self.buffer = []
        elif not self.en_pre:
            self.partes.append("</" + tag + ">")

    def handle_data(self, data):
        if self.en_pre:
            self.buffer.append(data)
        else:
            # convert_charrefs=True ya decodificó las entidades, así que hay
            # que volver a escapar o el < de un "<" en línea rompe el HTML.
            self.partes.append(escapar(data))

    def salida(self) -> str:
        return "".join(self.partes)


def serializar_atributos(attrs) -> str:
    partes = []
    for clave, valor in attrs:
        partes.append(' {}="{}"'.format(clave, escapar(valor or "")))
    return "".join(partes)


class ExtractorEncabezados(HTMLParser):
    """Saca (nivel, id, texto) de los h2/h3 para armar el índice."""

    PERMITIDOS = {"h2", "h3"}

    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.items = []
        self.nivel = None
        self.ident = None
        self.texto = []

    def handle_starttag(self, tag, attrs):
        if tag in self.PERMITIDOS:
            self.nivel = int(tag[1])
            self.ident = dict(attrs).get("id")
            self.texto = []

    def handle_endtag(self, tag):
        if self.nivel and tag == "h" + str(self.nivel):
            txt = re.sub(r"\s+", " ", "".join(self.texto)).strip()
            if self.ident:
                self.items.append((self.nivel, self.ident, txt))
            self.nivel = None
            self.ident = None
            self.texto = []

    def handle_data(self, data):
        if self.nivel:
            self.texto.append(data)


# --------------------------------------------------------------------------
def construir_indice(items) -> str:
    if not items:
        return ""
    filas = []
    for nivel, ident, texto in items:
        filas.append(
            '      <li class="nivel{n}"><a href="#{i}">{t}</a></li>'.format(
                n=nivel, i=ident, t=escapar(texto)
            )
        )
    return (
        '\n  <div class="indice">\n'
        '    <div class="indice-titulo">Contenido</div>\n'
        "    <ul>\n" + "\n".join(filas) + "\n    </ul>\n  </div>\n"
    )


def plantilla_html(indice: str, cuerpo: str, titulo: str) -> str:
    css_pygments = HtmlFormatter(style=EstiloEcoGrid, cssclass="hl").get_style_defs(
        ".hl"
    )
    return (
        "<!DOCTYPE html>\n"
        '<html lang="es">\n<head>\n'
        '<meta charset="utf-8">\n'
        "<title>{titulo}</title>\n"
        '<meta name="generator" content="docs/md2pdf.py">\n'
        "<style>\n{css}\n{css_hl}\n</style>\n</head>\n<body>\n"
        "{cuerpo}\n</body>\n</html>\n"
    ).format(
        titulo=escapar(titulo),
        css=CSS,
        css_hl=css_pygments,
        cuerpo=cuerpo,
    )


def md_a_html(md: str) -> str:
    cuerpo = markdown.markdown(
        md,
        extensions=["tables", "fenced_code", "sane_lists", "toc", "attr_list"],
        extension_configs={"toc": {"toc_depth": "2-3"}},
    )
    # Resaltado de los bloques de código.
    r = Resaltador()
    r.feed(cuerpo)
    cuerpo = r.salida()

    # Índice con números de página reales (target-counter), armado desde los
    # h2/h3 del HTML ya generado.
    ext = ExtractorEncabezados()
    ext.feed(cuerpo)
    indice = construir_indice(ext.items)

    # El índice se inserta después de la primera regla horizontal, que en este
    # .md cae justo después del bloque de portada. El .md no se toca.
    m = re.search(r"<hr\s*/?>", cuerpo)
    if m and indice:
        cuerpo = cuerpo[: m.end()] + "\n" + indice + cuerpo[m.end() :]
    return cuerpo


def titulo_del_md(md: str) -> str:
    for linea in md.splitlines():
        if linea.startswith("# "):
            return linea[2:].strip()
    return "Informe"


def main() -> int:
    ap = argparse.ArgumentParser(description="Markdown -> PDF con portada e índice.")
    ap.add_argument(
        "-i", "--entrada", default=os.path.join(RAIZ, "docs", "Informe_Tecnico.md")
    )
    ap.add_argument(
        "-o", "--salida", default=os.path.join(RAIZ, "docs", "Informe_Tecnico.pdf")
    )
    ap.add_argument("--motor", choices=["weasy", "lo"], default="weasy")
    args = ap.parse_args()

    with open(args.entrada, encoding="utf-8") as fh:
        md = fh.read()

    titulo = titulo_del_md(md)
    cuerpo = md_a_html(md)
    html = plantilla_html("", cuerpo, titulo)

    if args.motor == "lo":
        return render_libreoffice(html, args.salida)

    from weasyprint import HTML

    HTML(string=html, base_url=os.path.dirname(os.path.abspath(args.entrada))).write_pdf(
        args.salida
    )
    print("PDF generado: " + args.salida)
    return 0


def render_libreoffice(html: str, salida: str) -> int:
    """Fallback. El importador de HTML de LibreOffice no soporta @page,
    así que el resultado es más plano: sin pie numerado ni encabezado."""
    tmpdir = tempfile.mkdtemp(prefix="md2pdf-")
    html_path = os.path.join(tmpdir, "informe.html")
    with open(html_path, "w", encoding="utf-8") as fh:
        fh.write(html)
    r = subprocess.run(
        [
            "soffice",
            "--headless",
            "--norestore",
            "-env:UserInstallation=file://" + tmpdir + "/lo",
            "--convert-to",
            "pdf:writer_pdf_Export",
            "--outdir",
            tmpdir,
            html_path,
        ],
        capture_output=True,
        timeout=300,
    )
    generado = os.path.join(tmpdir, "informe.pdf")
    if r.returncode != 0 or not os.path.exists(generado):
        sys.stderr.write((r.stderr or b"").decode("utf-8", "replace"))
        return 1
    import shutil

    shutil.copyfile(generado, salida)
    print("PDF generado (LibreOffice): " + salida)
    return 0


if __name__ == "__main__":
    sys.exit(main())
