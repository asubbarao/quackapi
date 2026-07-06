#!/bin/bash
# Rebuild docs/CHRONICLE.pdf from docs/CHRONICLE.md.
#
# Default pipeline is DuckDB end-to-end via the `pdf` extension
# (asubbarao/duckdb-pdf): read_text -> md_to_html -> to_pdf. Implemented in
# the shared living-doc builder:
#   ~/personal/tools/build_living_pdf.sh
#
# Legacy fallback (pandoc + headless Chrome, higher CSS fidelity):
#   docs/build_pdf.sh --chrome
set -euo pipefail
cd "$(dirname "$0")"

if [ "${1:-}" != "--chrome" ]; then
  exec "$HOME/personal/tools/build_living_pdf.sh" CHRONICLE.md CHRONICLE.pdf
fi

# ---- legacy Chrome pipeline below ----
CHROME="/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"
[ -x "$CHROME" ] || CHROME="/Applications/Chromium.app/Contents/MacOS/Chromium"

cat > /tmp/chronicle_style.css <<'CSS'
body { font-family: Georgia, 'Times New Roman', serif; font-size: 11.5pt; line-height: 1.55;
       max-width: 46em; margin: 2em auto; color: #1a1a1a; padding: 0 1.5em; }
h1 { font-size: 1.6em; border-bottom: 2px solid #333; padding-bottom: .2em; margin-top: 1.6em; }
h2 { font-size: 1.25em; margin-top: 1.4em; }
h1.title { font-size: 2em; border: none; text-align: center; margin-bottom: 0; }
p.subtitle { text-align: center; font-style: italic; color: #444; margin-top: .2em; }
p.author, p.date { text-align: center; color: #555; font-size: .95em; margin: .1em; }
table { border-collapse: collapse; width: 100%; font-size: .82em; margin: 1em 0; }
th, td { border: 1px solid #999; padding: .35em .5em; text-align: left; vertical-align: top; }
th { background: #eee; }
code { font-family: 'SF Mono', Menlo, monospace; font-size: .88em; background: #f4f4f4;
       padding: .05em .25em; border-radius: 3px; }
blockquote { border-left: 3px solid #bbb; margin-left: 0; padding-left: 1em; color: #444; }
strong { color: #000; }
@media print { body { margin: 0 auto; } h1 { page-break-before: auto; } table { page-break-inside: avoid; } }
CSS

pandoc CHRONICLE.md --standalone --css=/tmp/chronicle_style.css --embed-resources \
  --metadata-file=/dev/null -f markdown -t html -o /tmp/chronicle.html

"$CHROME" --headless --disable-gpu --no-pdf-header-footer \
  --print-to-pdf=CHRONICLE.pdf /tmp/chronicle.html 2>/dev/null

ls -la CHRONICLE.pdf
