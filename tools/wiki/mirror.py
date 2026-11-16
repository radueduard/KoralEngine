#!/usr/bin/env python3
"""Mirrors the MkDocs guides into the repository's GitHub wiki.

The wiki is a flat set of pages, named by their files, so each guide becomes a page named by its title in mkdocs.yml's
nav ("The pass graph" -> The-pass-graph.md), index.md becomes Home, and the nav becomes the wiki's sidebar. Links
between guides are rewritten to page names; links to files the site leaves out, or outside docs/, go to GitHub. Code is
left as it is. What only MkDocs understands is undone: a snippet include is brought in whole, and the home page's card
grid is left as the list it is made of.

    python3 tools/wiki/mirror.py <wiki checkout>      # run from the repository's root

Every page in the wiki checkout is replaced: the wiki is a copy, and docs/ is what is edited.
"""
import os
import posixpath
import re
import sys

import yaml

ROOT = os.getcwd()


def load_config():
    with open(os.path.join(ROOT, "mkdocs.yml"), encoding="utf-8") as f:
        config = yaml.safe_load(f)
    docs = config.get("docs_dir", "docs")
    repo = config["repo_url"].rstrip("/")
    edit = config.get("edit_uri", "edit/main/docs/")
    branch = edit.split("/")[1] if edit.startswith("edit/") else "main"
    excluded = [line.strip() for line in (config.get("exclude_docs") or "").splitlines() if line.strip()]
    return config, docs, repo, branch, excluded


def page_name(title):
    """A wiki page's file name: GitHub shows it with its hyphens as spaces. What a link cannot carry is spelt out."""
    name = title.replace("#", " Sharp").replace("+", " Plus")
    name = re.sub(r'[\\/:*?"<>|]', "", name)
    return re.sub(r"\s+", "-", name.strip())


def walk_nav(nav, pages, sidebar, depth=0, section=None):
    """Each nav entry: a page named by its title, a section of entries, or a link elsewhere."""
    for entry in nav:
        if isinstance(entry, str):                       # a section's own page: named as the section
            title = section or entry
            pages.setdefault(entry, page_name(title))
            sidebar.append((depth, title, pages[entry], None))
            continue
        for title, value in entry.items():
            if isinstance(value, list):
                sidebar.append((depth, title, None, None))
                walk_nav(value, pages, sidebar, depth + 1, title)
            elif re.match(r"https?://", value):
                sidebar.append((depth, title, None, value))
            else:
                pages.setdefault(value, "Home" if value == "index.md" else page_name(title))
                sidebar.append((depth, title, pages[value], None))


def first_heading(path):
    with open(path, encoding="utf-8") as f:
        for line in f:
            if line.startswith("# "):
                return line[2:].strip()
    return os.path.splitext(os.path.basename(path))[0]


LINK = re.compile(r"(!?\[[^\]]*\]\()([^)\s]+)(\))")
REFERENCE = re.compile(r"^(\s*\[[^\]]+\]:\s*)(\S+)(.*)$")
SNIPPET = re.compile(r'^\s*--8<--\s+"([^"]+)"\s*$')


def convert(source, text, pages, docs, repo, branch):
    here = posixpath.dirname(source)

    def target(link):
        if re.match(r"[a-z]+:", link) or link.startswith("#"):
            return link
        path, _, anchor = link.partition("#")
        resolved = posixpath.normpath(posixpath.join(here, path)) if path else source
        if resolved in pages:
            return pages[resolved] + ("#" + anchor if anchor else "")
        # Left out of the site, or not a guide: the file on GitHub.
        in_repo = posixpath.normpath(posixpath.join(docs, resolved))
        return f"{repo}/blob/{branch}/{in_repo}" + ("#" + anchor if anchor else "")

    def links(line):
        # Only outside `code`: a span's backticks split the line, and the odd pieces are code.
        pieces = re.split(r"(`+[^`]*`+)", line)
        for i in range(0, len(pieces), 2):
            pieces[i] = LINK.sub(lambda m: m.group(1) + target(m.group(2)) + m.group(3), pieces[i])
        return "".join(pieces)

    out, fence = [], None
    for line in text.splitlines():
        stripped = line.lstrip()
        if fence:
            out.append(line)
            if stripped.startswith(fence):
                fence = None
            continue
        if stripped.startswith("```") or stripped.startswith("~~~"):
            fence = stripped[:3]
            out.append(line)
            continue
        snippet = SNIPPET.match(line)
        if snippet:
            with open(os.path.join(ROOT, snippet.group(1)), encoding="utf-8") as f:
                out.extend(convert(source, f.read(), pages, docs, repo, branch).splitlines())
            continue
        if re.match(r"^\s*<div\b[^>]*\bmarkdown\b[^>]*>\s*$", line) or re.match(r"^\s*</div>\s*$", line):
            continue
        reference = REFERENCE.match(line)
        if reference:
            out.append(reference.group(1) + target(reference.group(2)) + reference.group(3))
            continue
        out.append(links(line))
    return "\n".join(out) + "\n"


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    wiki = sys.argv[1]
    config, docs, repo, branch, excluded = load_config()

    pages, sidebar = {}, []
    walk_nav(config.get("nav", []), pages, sidebar)
    # A guide the nav does not name is still a page, by its heading.
    docs_path = os.path.join(ROOT, docs)
    for directory, _, files in os.walk(docs_path):
        for file in sorted(files):
            relative = posixpath.relpath(posixpath.join(directory, file), docs_path).replace(os.sep, "/")
            if file.endswith(".md") and relative not in pages and relative not in excluded and file not in excluded:
                pages[relative] = page_name(first_heading(os.path.join(directory, file)))

    for file in os.listdir(wiki):
        if file.endswith(".md"):
            os.remove(os.path.join(wiki, file))

    for source, name in pages.items():
        with open(os.path.join(docs_path, source), encoding="utf-8") as f:
            text = f.read()
        with open(os.path.join(wiki, name + ".md"), "w", encoding="utf-8") as f:
            f.write(convert(source, text, pages, docs, repo, branch))

    lines = []
    for depth, title, page, url in sidebar:
        indent = "  " * depth
        if page:
            lines.append(f"{indent}- [[{title}|{page}]]")
        elif url:
            lines.append(f"{indent}- [{title}]({url})")
        else:
            lines.append(f"{indent}- **{title}**")
    with open(os.path.join(wiki, "_Sidebar.md"), "w", encoding="utf-8") as f:
        f.write(f"**[{config['site_name']}]({config.get('site_url', repo)})**\n\n" + "\n".join(lines) + "\n")
    with open(os.path.join(wiki, "_Footer.md"), "w", encoding="utf-8") as f:
        f.write(f"This wiki is a copy of [`docs/`]({repo}/tree/{branch}/{docs}), made on every push to `{branch}`: "
                f"edit the guides there. They are also a site: {config.get('site_url', repo)}\n")
    print(f"{len(pages)} pages written to {wiki}")


if __name__ == "__main__":
    main()
