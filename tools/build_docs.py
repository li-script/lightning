#!/usr/bin/env python3
"""
Lightning Script GitBook-Style Documentation Generator
Reads Markdown chapters from docs/ and generates an interactive, searchable static documentation site.
"""

import os
import re
import sys
import shutil
import argparse
import urllib.parse
import markdown
from pathlib import Path

ROOT_DIR = Path(__file__).resolve().parent.parent
DOCS_DIR = ROOT_DIR / "docs"
EXAMPLES_DIR = DOCS_DIR / "examples"

SECTIONS = [
    {
        "title": "Getting Started",
        "items": [
            {"id": "index", "file": "INDEX.md", "title": "Overview", "out": "index.html"},
            {"id": "getting-started", "file": "1-getting-started.md", "title": "1. Getting Started", "out": "1-getting-started.html"},
            {"id": "basic-concepts", "file": "2-basic-concepts.md", "title": "2. Basic Concepts", "out": "2-basic-concepts.html"},
        ]
    },
    {
        "title": "The Language",
        "items": [
            {"id": "language-syntax", "file": "3-language-syntax.md", "title": "3. Language Syntax", "out": "3-language-syntax.html"},
            {"id": "control-flow", "file": "4-control-flow.md", "title": "4. Control Flow", "out": "4-control-flow.html"},
            {"id": "functions-and-closures", "file": "5-functions-and-closures.md", "title": "5. Functions and Closures", "out": "5-functions-and-closures.html"},
            {"id": "structs-and-classes", "file": "6-structs-and-classes.md", "title": "6. Structs and Classes", "out": "6-structs-and-classes.html"},
            {"id": "strict-tier", "file": "7-strict-tier.md", "title": "7. The Strict Tier", "out": "7-strict-tier.html"},
        ]
    },
    {
        "title": "Standard Libraries",
        "items": [
            {"id": "stdlib-builtins", "file": "8-stdlib-builtins.md", "title": "8. Built-in Functions", "out": "8-stdlib-builtins.html"},
            {"id": "stdlib-collections", "file": "9-stdlib-collections.md", "title": "9. Collections", "out": "9-stdlib-collections.html"},
            {"id": "stdlib-math-and-typed", "file": "10-stdlib-math-and-typed.md", "title": "10. Mathematics and Typed Values", "out": "10-stdlib-math-and-typed.html"},
            {"id": "stdlib-system-and-concurrency", "file": "11-stdlib-system-and-concurrency.md", "title": "11. System and Concurrency", "out": "11-stdlib-system-and-concurrency.html"},
        ]
    },
    {
        "title": "Host Interface",
        "items": [
            {"id": "embedding-api", "file": "12-embedding-api.md", "title": "12. Embedding API", "out": "12-embedding-api.html"},
            {"id": "examples", "file": None, "title": "Examples", "out": "examples.html"},
        ]
    }
]

ALL_PAGES = []
for sec in SECTIONS:
    for item in sec["items"]:
        ALL_PAGES.append({**item, "section": sec["title"]})

# Shared monochrome documentation theme used alongside the WebAssembly playground.
GITBOOK_CSS = """
:root {
  --canvas: #0a0a0b;
  --surface: #111113;
  --elevated: #18181b;
  --hover: #1f1f23;
  --border: #27272a;
  --border-strong: #3f3f46;
  --text: #f4f4f5;
  --text-secondary: #a1a1aa;
  --text-muted: #71717a;
  --ok: #4ade80;
  --warn: #fbbf24;
  --error: #f87171;
  --font-sans: Inter, -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, system-ui, sans-serif;
  --font-mono: "JetBrains Mono", "SF Mono", ui-monospace, Menlo, Consolas, monospace;
}

* { box-sizing: border-box; }

html { background: var(--canvas); }

body {
  margin: 0;
  min-height: 100vh;
  display: flex;
  background: var(--canvas);
  color: var(--text);
  font-family: var(--font-sans);
  font-size: 14px;
  line-height: 1.5;
  -webkit-font-smoothing: antialiased;
}

button,
input { font: inherit; }

button,
a { -webkit-tap-highlight-color: transparent; }

a:focus-visible,
button:focus-visible,
input:focus-visible {
  outline: 2px solid var(--text);
  outline-offset: 2px;
}

.sidebar {
  width: 260px;
  height: 100vh;
  position: sticky;
  top: 0;
  flex: 0 0 260px;
  display: flex;
  flex-direction: column;
  overflow-y: auto;
  background: var(--surface);
  border-right: 1px solid var(--border);
  z-index: 80;
}

.sidebar-header {
  min-height: 48px;
  padding: 0 16px;
  display: flex;
  align-items: center;
  border-bottom: 1px solid var(--border);
}

.sidebar-title {
  color: var(--text-muted);
  font-size: 11px;
  font-weight: 600;
  letter-spacing: .08em;
  text-transform: uppercase;
}

.search-box {
  padding: 12px 16px;
  border-bottom: 1px solid var(--border);
}

.search-input {
  width: 100%;
  height: 34px;
  padding: 0 10px;
  color: var(--text);
  background: var(--canvas);
  border: 1px solid var(--border);
  border-radius: 6px;
  outline: none;
}

.search-input::placeholder { color: var(--text-muted); }
.search-input:hover { border-color: var(--border-strong); }
.search-input:focus { border-color: var(--text-secondary); }

.nav-groups {
  flex: 1;
  padding: 12px 0 24px;
}

.nav-group + .nav-group { margin-top: 12px; }

.nav-group-title {
  padding: 8px 16px 4px;
  color: var(--text-muted);
  font-size: 11px;
  font-weight: 600;
  letter-spacing: .08em;
  text-transform: uppercase;
}

.nav-link {
  display: block;
  min-height: 32px;
  padding: 6px 16px 6px 14px;
  color: var(--text-secondary);
  border-left: 2px solid transparent;
  text-decoration: none;
}

.nav-link:hover {
  color: var(--text);
  background: var(--hover);
}

.nav-link.active {
  color: var(--text);
  border-left-color: var(--text);
  font-weight: 500;
}

.nav-link[hidden],
.nav-group[hidden] { display: none; }

.sidebar-empty {
  display: none;
  margin: 8px 16px;
  color: var(--text-muted);
  font-size: 13px;
}

.main-wrapper {
  min-width: 0;
  flex: 1;
}

.top-nav {
  height: 48px;
  padding: 0 24px;
  position: sticky;
  top: 0;
  z-index: 60;
  display: flex;
  align-items: center;
  justify-content: space-between;
  background: var(--canvas);
  border-bottom: 1px solid var(--border);
}

.top-nav-left,
.top-nav-right,
.brand-link {
  display: flex;
  align-items: center;
}

.top-nav-left { min-width: 0; gap: 12px; }
.top-nav-right { gap: 16px; }

.brand-link {
  gap: 8px;
  color: var(--text);
  font-weight: 600;
  letter-spacing: -.01em;
  text-decoration: none;
  white-space: nowrap;
}

.brand-icon {
  width: 22px;
  height: 22px;
  flex: 0 0 auto;
}

.product-area {
  color: var(--text-muted);
  white-space: nowrap;
}

.header-link,
.markdown-body a,
.chapter-btn {
  color: var(--text);
  text-decoration: underline;
  text-decoration-color: var(--border-strong);
  text-decoration-thickness: 1px;
  text-underline-offset: 3px;
}

.header-link:hover,
.markdown-body a:hover,
.chapter-btn:hover { text-decoration-color: var(--text); }

.menu-button {
  display: none;
  width: 34px;
  height: 34px;
  padding: 0;
  align-items: center;
  justify-content: center;
  color: var(--text);
  background: transparent;
  border: 1px solid var(--border);
  border-radius: 6px;
  cursor: pointer;
}

.menu-button:hover { background: var(--hover); }
.menu-button svg { width: 18px; height: 18px; }

.sidebar-backdrop { display: none; }

.content-container {
  width: 100%;
  max-width: 760px;
  margin: 0 auto;
  padding: 32px 32px 80px;
}

.markdown-body { min-width: 0; }

.markdown-body h1,
.markdown-body h2,
.markdown-body h3,
.markdown-body h4 {
  position: relative;
  color: var(--text);
  font-weight: 600;
  letter-spacing: -.01em;
  line-height: 1.25;
}

.markdown-body h1 {
  margin: 0 0 24px;
  font-size: 32px;
}

.markdown-body h2 {
  margin: 32px 0 12px;
  padding-top: 8px;
  font-size: 22px;
}

.markdown-body h3 {
  margin: 24px 0 8px;
  font-size: 17px;
}

.markdown-body h4 {
  margin: 20px 0 8px;
  font-size: 14px;
}

.heading-anchor {
  position: absolute;
  right: 100%;
  padding-right: 8px;
  color: var(--text-muted) !important;
  opacity: 0;
  text-decoration: none !important;
}

.markdown-body h1:hover .heading-anchor,
.markdown-body h2:hover .heading-anchor,
.markdown-body h3:hover .heading-anchor,
.markdown-body h4:hover .heading-anchor,
.heading-anchor:focus { opacity: 1; }

.markdown-body p,
.markdown-body ul,
.markdown-body ol {
  margin: 0 0 16px;
  color: var(--text-secondary);
}

.markdown-body ul,
.markdown-body ol { padding-left: 24px; }
.markdown-body li + li { margin-top: 4px; }
.markdown-body strong { color: var(--text); font-weight: 600; }
.markdown-body hr { margin: 32px 0; border: 0; border-top: 1px solid var(--border); }

.markdown-body blockquote {
  margin: 20px 0;
  padding: 2px 0 2px 16px;
  color: var(--text-secondary);
  border-left: 2px solid var(--border-strong);
}

.markdown-body blockquote p:last-child { margin-bottom: 0; }

.markdown-body code,
.code-block-wrapper pre {
  font-family: var(--font-mono);
  font-size: 13px;
  line-height: 1.6;
}

.markdown-body :not(pre) > code {
  padding: 2px 5px;
  color: var(--text);
  background: var(--elevated);
  border: 1px solid var(--border);
  border-radius: 4px;
}

.code-block-wrapper {
  margin: 20px 0;
  overflow: hidden;
  background: var(--surface);
  border: 1px solid var(--border);
  border-radius: 8px;
}

.code-block-header {
  min-height: 38px;
  padding: 6px 8px 6px 12px;
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 8px;
  border-bottom: 1px solid var(--border);
}

.code-lang {
  color: var(--text-muted);
  font-size: 11px;
  font-weight: 600;
  letter-spacing: .08em;
  text-transform: uppercase;
}

.code-actions { display: flex; align-items: center; gap: 8px; }

.btn-code-action {
  min-height: 26px;
  padding: 3px 8px;
  display: inline-flex;
  align-items: center;
  color: var(--text-secondary);
  background: transparent;
  border: 1px solid var(--border);
  border-radius: 6px;
  font-size: 12px;
  line-height: 1.4;
  text-decoration: none;
  cursor: pointer;
}

.btn-code-action:hover {
  color: var(--text);
  background: var(--hover);
  border-color: var(--border-strong);
}

.btn-code-copy[data-state="copied"] { color: var(--ok); }

.code-block-wrapper pre {
  margin: 0;
  padding: 16px;
  overflow-x: auto;
  color: var(--text);
  background: var(--surface);
}

.code-block-wrapper pre code {
  padding: 0;
  color: inherit;
  background: transparent;
  border: 0;
}

.markdown-body table {
  width: 100%;
  margin: 20px 0;
  border-collapse: collapse;
  color: var(--text-secondary);
  font-size: 13px;
}

.markdown-body th,
.markdown-body td {
  padding: 9px 12px;
  text-align: left;
  vertical-align: top;
  border: 1px solid var(--border);
}

.markdown-body th {
  color: var(--text);
  background: var(--surface);
  font-weight: 600;
}

.chapter-nav {
  margin-top: 48px;
  padding-top: 24px;
  display: flex;
  justify-content: space-between;
  gap: 16px;
  border-top: 1px solid var(--border);
}

.chapter-btn {
  max-width: 48%;
  display: flex;
  flex-direction: column;
  text-decoration: none;
}

.chapter-btn.next { margin-left: auto; text-align: right; }
.chapter-btn:hover .nav-title { text-decoration-color: var(--text); }

.nav-direction {
  color: var(--text-muted);
  font-size: 11px;
  letter-spacing: .08em;
  text-transform: uppercase;
}

.nav-title {
  margin-top: 4px;
  color: var(--text);
  text-decoration: underline;
  text-decoration-color: var(--border-strong);
  text-underline-offset: 3px;
}

.token.keyword { color: #ffffff; font-weight: 600; }
.token.builtin,
.token.function { color: #e4e4e7; }
.token.hook { color: #e4e4e7; font-weight: 600; }
.token.type,
.token.class-name,
.token.namespace { color: #d4d4d8; font-style: italic; }
.token.string,
.token.raw-string,
.token.template-string { color: #9cc9ff; }
.token.template-string .token.interpolation,
.token.template-string .token.interpolation .token { color: #c7d2e0; }
.token.number,
.token.boolean,
.token.constant { color: #b8c8e0; }
.token.comment { color: #52525b; font-style: italic; }
.token.attribute,
.token.attr-name { color: #71717a; }
.token.operator { color: #a1a1aa; }
.token.punctuation,
.token.interpolation-punctuation { color: #71717a; }

@media (max-width: 760px) {
  body { display: block; }

  .sidebar {
    width: min(320px, calc(100vw - 40px));
    height: calc(100vh - 48px);
    position: fixed;
    top: 48px;
    left: 0;
    transform: translateX(-100%);
    transition: transform 160ms ease;
  }

  body.menu-open { overflow: hidden; }
  body.menu-open .sidebar { transform: translateX(0); }

  .sidebar-header { display: none; }

  .sidebar-backdrop {
    position: fixed;
    inset: 48px 0 0;
    z-index: 70;
    background: rgba(10, 10, 11, .72);
  }

  body.menu-open .sidebar-backdrop { display: block; }

  .top-nav { padding: 0 12px; }
  .menu-button { display: inline-flex; }
  .top-nav-left { gap: 8px; }
  .top-nav-right { gap: 12px; }
  .product-area { font-size: 13px; }
  .header-link { font-size: 13px; }

  .content-container { padding: 24px 20px 64px; }
  .markdown-body h1 { font-size: 28px; }
  .markdown-body h2 { font-size: 20px; }
  .heading-anchor { display: none; }

  .markdown-body table {
    display: block;
    overflow-x: auto;
    white-space: nowrap;
  }

  .chapter-nav { align-items: flex-start; }
  .chapter-btn { max-width: 50%; }
}

@media (max-width: 390px) {
  .top-nav-right { gap: 8px; }
  .brand-link { gap: 6px; font-size: 13px; }
  .brand-icon { width: 20px; height: 20px; }
  .product-area,
  .header-link { font-size: 12px; }
  .content-container { padding-left: 16px; padding-right: 16px; }
  .code-block-header { align-items: flex-start; }
  .code-actions { gap: 4px; }
  .btn-code-action { padding-left: 6px; padding-right: 6px; }
}

@media (prefers-reduced-motion: reduce) {
  *,
  *::before,
  *::after {
    scroll-behavior: auto !important;
    transition-duration: .01ms !important;
    animation-duration: .01ms !important;
    animation-iteration-count: 1 !important;
  }
}
"""

GITBOOK_JS = """
document.addEventListener('DOMContentLoaded', () => {
  const body = document.body;
  const menuButton = document.getElementById('menu-button');
  const backdrop = document.querySelector('.sidebar-backdrop');
  const searchInput = document.getElementById('search-input');

  const setMenuOpen = (open) => {
    body.classList.toggle('menu-open', open);
    if (menuButton) menuButton.setAttribute('aria-expanded', String(open));
    if (open && searchInput) searchInput.focus();
  };

  if (menuButton) {
    menuButton.addEventListener('click', () => setMenuOpen(!body.classList.contains('menu-open')));
  }
  if (backdrop) backdrop.addEventListener('click', () => setMenuOpen(false));
  document.addEventListener('keydown', (event) => {
    if (event.key === 'Escape') setMenuOpen(false);
  });
  document.querySelectorAll('.nav-link').forEach((link) => {
    link.addEventListener('click', () => setMenuOpen(false));
  });

  if (searchInput) {
    searchInput.addEventListener('input', (event) => {
      const query = event.target.value.toLowerCase().trim();
      let visibleCount = 0;
      document.querySelectorAll('.nav-group').forEach((group) => {
        let groupCount = 0;
        group.querySelectorAll('.nav-link').forEach((link) => {
          const visible = link.textContent.toLowerCase().includes(query);
          link.hidden = !visible;
          if (visible) groupCount += 1;
        });
        group.hidden = groupCount === 0;
        visibleCount += groupCount;
      });
      const emptyState = document.querySelector('.sidebar-empty');
      if (emptyState) emptyState.style.display = visibleCount === 0 ? 'block' : 'none';
    });
  }

  document.querySelectorAll('.markdown-body h1[id], .markdown-body h2[id], .markdown-body h3[id], .markdown-body h4[id]').forEach((heading) => {
    const anchor = document.createElement('a');
    anchor.className = 'heading-anchor';
    anchor.href = `#${heading.id}`;
    anchor.setAttribute('aria-label', `Link to ${heading.textContent}`);
    anchor.textContent = '#';
    heading.prepend(anchor);
  });

  document.querySelectorAll('.btn-code-copy').forEach((button) => {
    button.addEventListener('click', async () => {
      const code = button.closest('.code-block-wrapper').querySelector('code').innerText;
      try {
        await navigator.clipboard.writeText(code);
      } catch (_) {
        const textArea = document.createElement('textarea');
        textArea.value = code;
        textArea.style.position = 'fixed';
        textArea.style.opacity = '0';
        document.body.appendChild(textArea);
        textArea.select();
        document.execCommand('copy');
        textArea.remove();
      }
      button.textContent = 'Copied';
      button.dataset.state = 'copied';
      window.setTimeout(() => {
        button.textContent = 'Copy';
        delete button.dataset.state;
      }, 1500);
    });
  });
});
"""

def wrap_code_blocks(html_content):
    """
    Wraps pre > code blocks with a header containing language label, copy button, and 'Run in REPL' button.
    """
    pattern = re.compile(r'<pre><code class="(?:language-)?([a-zA-Z0-9_\-]+)">([\s\S]*?)</code></pre>')

    def repl(m):
        lang = m.group(1).lower()
        code_html = m.group(2)
        raw_code = code_html.replace("&amp;", "&").replace("&lt;", "<").replace("&gt;", ">").replace("&quot;", '"').replace("&#39;", "'")
        encoded_code = urllib.parse.quote(raw_code)

        run_btn = ""
        if lang in ("lightning", "li", ""):
            run_btn = f'<a href="../index.html?code={encoded_code}" target="_blank" rel="noopener" class="btn-code-action btn-code-run" title="Open in the playground">Run in Playground</a>'

        return f'''<div class="code-block-wrapper">
  <div class="code-block-header">
    <span class="code-lang">{lang.upper() if lang else "CODE"}</span>
    <div class="code-actions">
      {run_btn}
      <button type="button" class="btn-code-action btn-code-copy">Copy</button>
    </div>
  </div>
  <pre><code class="language-{lang}">{code_html}</code></pre>
</div>'''

    return pattern.sub(repl, html_content)


def rewrite_doc_links(html_content):
    """Rewrites source-document links for the generated flat site."""
    html_content = re.sub(
        r'href="([^"#]+)\.md(#[^"]*)?"',
        lambda m: f'href="{m.group(1)}.html{m.group(2) or ""}"',
        html_content,
    )

    def example_link(m):
        filename = m.group(1)
        anchor = "example-" + re.sub(r"[^a-z0-9]+", "-", filename.lower()).strip("-")
        return f'href="examples.html#{anchor}"'

    html_content = re.sub(r'href="(?:\./)?examples/([^"#]+\.li)(?:#[^"]*)?"', example_link, html_content)
    html_content = re.sub(r'href="(?:\./)?examples/?"', 'href="examples.html"', html_content)
    return html_content


def build_sidebar(current_out):
    html = '<div class="nav-groups">\n'
    for sec in SECTIONS:
        html += '  <section class="nav-group">\n'
        html += f'    <div class="nav-group-title">{sec["title"]}</div>\n'
        for item in sec["items"]:
            active = " active" if item["out"] == current_out else ""
            current = ' aria-current="page"' if active else ""
            html += f'    <a href="{item["out"]}" class="nav-link{active}"{current}>{item["title"]}</a>\n'
        html += '  </section>\n'
    html += '</div>\n  <p class="sidebar-empty">No matching pages</p>\n'
    return html


def build_page_html(title, content_html, current_out, prev_page, next_page):
    sidebar_nav = build_sidebar(current_out)

    prev_nav = ""
    if prev_page:
        prev_nav = f'''<a href="{prev_page["out"]}" class="chapter-btn">
  <span class="nav-direction">← Previous</span>
  <span class="nav-title">{prev_page["title"]}</span>
</a>'''

    next_nav = ""
    if next_page:
        next_nav = f'''<a href="{next_page["out"]}" class="chapter-btn next">
  <span class="nav-direction">Next →</span>
  <span class="nav-title">{next_page["title"]}</span>
</a>'''

    return f"""<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>{title} — Lightning Docs</title>
  <link rel="icon" type="image/svg+xml" href="data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 256 256'%3E%3Crect width='256' height='256' rx='48' fill='%230a0a0b'/%3E%3Cpath d='M150 28 L64 150 H122 L106 228 L192 106 H134 Z' fill='%23f4f4f5'/%3E%3C/svg%3E">
  <link rel="stylesheet" href="gitbook.css">
</head>
<body>
  <aside class="sidebar" id="docs-sidebar" aria-label="Documentation navigation">
    <div class="sidebar-header">
      <span class="sidebar-title">Documentation</span>
    </div>

    <div class="search-box">
      <input type="search" id="search-input" class="search-input" placeholder="Search pages" aria-label="Search documentation pages" autocomplete="off">
    </div>

    {sidebar_nav}
  </aside>
  <button type="button" class="sidebar-backdrop" aria-label="Close navigation"></button>

  <div class="main-wrapper">
    <header class="top-nav">
      <div class="top-nav-left">
        <button type="button" id="menu-button" class="menu-button" aria-label="Open navigation" aria-controls="docs-sidebar" aria-expanded="false">
          <svg viewBox="0 0 20 20" fill="none" aria-hidden="true"><path d="M3 5.5h14M3 10h14M3 14.5h14" stroke="currentColor" stroke-width="1.5" stroke-linecap="round"/></svg>
        </button>
        <a href="index.html" class="brand-link">
          <svg class="brand-icon" viewBox="0 0 256 256" aria-hidden="true"><rect width="256" height="256" rx="48" fill="#0a0a0b"/><path d="M150 28 L64 150 H122 L106 228 L192 106 H134 Z" fill="#f4f4f5"/></svg>
          <span>Lightning</span>
        </a>
        <span class="product-area">Docs</span>
      </div>
      <nav class="top-nav-right" aria-label="Product links">
        <a href="../index.html" class="header-link">Playground</a>
        <a href="https://github.com/li-script/lightning" class="header-link">GitHub</a>
      </nav>
    </header>

    <main class="content-container">
      <article class="markdown-body">
        {content_html}
      </article>

      <nav class="chapter-nav">
        {prev_nav}
        {next_nav}
      </nav>
    </main>
  </div>

  <script src="prism.min.js"></script>
  <script src="prism-lightning.js"></script>
  <script src="gitbook.js"></script>
</body>
</html>
"""


def generate_examples_page():
    """Generates examples.html listing all 12 runnable examples from docs/examples/."""
    examples = []
    for p in sorted(EXAMPLES_DIR.glob("*.li")):
        code = p.read_text(encoding="utf-8")
        title = p.stem.replace("_", " ").title()
        examples.append((title, p.name, code))

    body = '<h1 id="lightning-script-examples">Lightning Script Examples</h1>\n'
    body += "<p>Complete, runnable programs covering the core features of Lightning Script. Open any example in the playground or copy it for local use.</p>\n\n"

    for title, fname, code in examples:
        encoded = urllib.parse.quote(code)
        example_id = "example-" + re.sub(r"[^a-z0-9]+", "-", fname.lower()).strip("-")
        body += f'<h2 id="{example_id}">{title} <code>{fname}</code></h2>\n'
        body += f"""<div class="code-block-wrapper">
  <div class="code-block-header">
    <span class="code-lang">LIGHTNING</span>
    <div class="code-actions">
      <a href="../index.html?code={encoded}" target="_blank" rel="noopener" class="btn-code-action btn-code-run">Run in Playground</a>
      <button type="button" class="btn-code-action btn-code-copy">Copy</button>
    </div>
  </div>
  <pre><code class="language-lightning">{code.replace('&', '&amp;').replace('<', '&lt;').replace('>', '&gt;')}</code></pre>
</div>\n\n"""

    return body


def main():
    parser = argparse.ArgumentParser(description="Build GitBook-style documentation for Lightning Script")
    parser.add_argument("--out", "-o", default=str(ROOT_DIR / "web" / "docs"), help="Output directory for generated docs")
    args = parser.parse_args()

    output_dir = Path(args.out).resolve()
    output_dir.mkdir(parents=True, exist_ok=True)

    # Write CSS and JS assets
    (output_dir / "gitbook.css").write_text(GITBOOK_CSS.strip(), encoding="utf-8")
    (output_dir / "gitbook.js").write_text(GITBOOK_JS.strip(), encoding="utf-8")

    # Copy Prism assets from web/
    shutil.copy2(ROOT_DIR / "web" / "prism.min.js", output_dir / "prism.min.js")
    shutil.copy2(ROOT_DIR / "web" / "prism-lightning.js", output_dir / "prism-lightning.js")

    md_processor = markdown.Markdown(extensions=["extra", "tables", "fenced_code", "toc", "sane_lists"])

    for idx, page in enumerate(ALL_PAGES):
        prev_p = ALL_PAGES[idx - 1] if idx > 0 else None
        next_p = ALL_PAGES[idx + 1] if idx < len(ALL_PAGES) - 1 else None

        if page["file"] is not None:
            src_file = DOCS_DIR / page["file"]
            if not src_file.exists():
                print(f"Warning: {src_file} does not exist, skipping.")
                continue
            text = src_file.read_text(encoding="utf-8")
            raw_html = md_processor.convert(text)
            md_processor.reset()
            content_html = rewrite_doc_links(wrap_code_blocks(raw_html))
        else:
            content_html = generate_examples_page()

        full_html = build_page_html(page["title"], content_html, page["out"], prev_p, next_p)
        out_path = output_dir / page["out"]
        out_path.write_text(full_html, encoding="utf-8")

    print(f"Documentation build complete: {output_dir}")


if __name__ == "__main__":
    main()
