#!/usr/bin/env bash
# ============================================================
# EcoGrid - render de los diagramas
#
# Convierte los .puml de este directorio a .png y .pdf, que son los
# formatos que pide el enunciado. Los .puml son la fuente de verdad:
# los .png/.pdf son artefactos de build y no se versionan.
#
# Requisitos:
#   - plantuml   (el .jar o el paquete del sistema)
#   - graphviz   (dot), que plantuml usa para los diagramas de clases
#
# Instalación rápida en Debian/Ubuntu:
#   sudo apt install default-jre graphviz
#   y bajar plantuml.jar de https://github.com/plantuml/plantuml/releases
#
# Uso:
#   ./docs/render.sh              # png + pdf
#   ./docs/render.sh png          # solo png
#   PLANTUML_JAR=/ruta/plantuml.jar ./docs/render.sh
# ============================================================
set -euo pipefail

cd "$(dirname "$0")"
FORMATO="${1:-all}"

# Cómo invocar plantuml: el .jar si se pasó, o el binario del sistema.
if [[ -n "${PLANTUML_JAR:-}" ]]; then
  PLANTUML=(java -jar "$PLANTUML_JAR")
elif command -v plantuml >/dev/null 2>&1; then
  PLANTUML=(plantuml)
else
  echo "Error: no se encontró plantuml." >&2
  echo "" >&2
  echo "  sudo apt install default-jre graphviz" >&2
  echo "  y después:  ./docs/render.sh" >&2
  echo "" >&2
  echo "Alternativa sin instalar nada: pegar el contenido del .puml en" >&2
  echo "https://www.plantuml.com/plantuml/uml/ y descargar la imagen." >&2
  exit 1
fi

if ! command -v dot >/dev/null 2>&1; then
  echo "Aviso: falta graphviz (dot). El diagrama de clases usa el layout" >&2
  echo "de Graphviz y va a salir feo o fallar. sudo apt install graphviz" >&2
fi

FUENTES=(diagrama_clases.puml diagrama_entidad_relacion.puml)

case "$FORMATO" in
  png)  FORMATOS=(-tpng)  ;;
  pdf)  FORMATOS=(-tpdf)  ;;
  all)  FORMATOS=(-tpng -tpdf) ;;
  *)    echo "Uso: $0 [all|png|pdf]" >&2; exit 2 ;;
esac

for f in "${FUENTES[@]}"; do
  for fmt in "${FORMATOS[@]}"; do
    echo "Renderizando $f $fmt ..."
    "${PLANTUML[@]}" "$fmt" "$f"
  done
done

echo ""
echo "Listo. Archivos generados en $(pwd):"
ls -1 ./*.png ./*.pdf 2>/dev/null || echo "  (ninguno)"
