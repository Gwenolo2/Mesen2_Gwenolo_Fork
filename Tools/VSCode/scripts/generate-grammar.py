#!/usr/bin/env python3
"""Génère les grammaires TextMate des packs SNES et NES depuis leurs schémas.

Sorties : syntaxes/mesen-snes-hd.tmLanguage.json (schemas/29_schema_balises_snes.json)
          syntaxes/mesen-nes-hd.tmLanguage.json (schemas/schema_balises_nes.json)
Relancer après toute modification d'un schéma : python3 scripts/generate-grammar.py
"""
import json
import pathlib
import re

ROOT = pathlib.Path(__file__).resolve().parent.parent

# (schéma, grammaire, suffixe du scope, nom affiché)
TARGETS = [
    ('schemas/29_schema_balises_snes.json', 'syntaxes/mesen-snes-hd.tmLanguage.json', 'snes-hd', 'Mesen HD Pack (SNES)'),
    ('schemas/schema_balises_nes.json', 'syntaxes/mesen-nes-hd.tmLanguage.json', 'nes-hd', 'Mesen HD Pack (NES)'),
]


def alt(words):
    return '|'.join(re.escape(w) for w in sorted(set(words), key=len, reverse=True))


def build(schema_path, out_path, suffix, display):
    s = json.loads((ROOT / schema_path).read_text(encoding='utf-8'))
    scope = f'source.mesen-{suffix}'
    tag = f'mesen-{suffix}'

    tags = [t['name'][1:-1] for t in s['tags'] if t['status'] != 'docOnly']
    ci = {c.lower() for c in s.get('matching', {}).get('caseInsensitive', [])}
    ci = {c[1:-1] if c.startswith('<') else c for c in ci}
    exact = [t for t in tags if t.lower() not in ci]
    insensitive = [t for t in tags if t.lower() in ci]
    deprecated = [t['name'][1:-1] for t in s.get('notRecognizedByLoader', [])]

    conds = set()
    for ct in s.get('conditionTypes', []):
        conds.add(ct['name'])
        for v in ct.get('variants', []):
            base = v.lstrip('!')
            conds.add(base)
            if ct.get('constantVariants'):
                conds.add(base + 'Constant')

    patterns = [
        {"name": f"comment.line.number-sign.{tag}", "match": "#.*$"},
        {
            "name": f"meta.condition.{tag}",
            "begin": "^\\s*\\[",
            "end": "\\]",
            "beginCaptures": {"0": {"name": f"punctuation.definition.condition.begin.{tag}"}},
            "endCaptures": {"0": {"name": f"punctuation.definition.condition.end.{tag}"}},
            "patterns": [
                {"name": f"keyword.operator.and.{tag}", "match": "&"},
                {"name": f"keyword.operator.not.{tag}", "match": "!"},
                {"name": f"support.function.condition.{tag}", "match": "\\b(?:" + alt(conds) + ")\\b"},
                {"name": f"constant.numeric.hash.{tag}", "match": "\\b[0-9A-Fa-f]{8,16}\\b"},
                {"name": f"constant.numeric.{tag}", "match": "-?\\d+(?:\\.\\d+)?"},
                {"name": f"keyword.operator.or.{tag}", "match": "\\|"},
            ],
        },
        {"name": f"punctuation.section.block.{tag}", "match": "\\}>?|\\{"},
    ]
    if exact:
        patterns.append({"name": f"keyword.control.tag.{tag}", "match": "<(?:" + alt(exact) + ")>"})
    if insensitive:
        patterns.append({"name": f"keyword.control.tag.{tag}", "match": "(?i:<(?:" + alt(insensitive) + ")>)"})
    if deprecated:
        patterns.append({"name": f"invalid.deprecated.tag.{tag}", "match": "(?i:<(?:" + alt(deprecated) + ")>)"})
    patterns += [
        {"name": f"invalid.illegal.tag.{tag}", "match": "<[^>\\s]*>"},
        {"name": f"variable.parameter.{tag}", "match": "(?<=[,>\\s])[A-Za-z_]\\w*(?==)"},
        {
            "name": f"string.unquoted.path.{tag}",
            "match": "[^\\s,\\[\\]{}<>=|#]+\\.(?i:png|ogg|json|obj|mtl|gltf|glb|mpg|mp4|txt|mesen)\\b",
        },
        {"name": f"constant.numeric.hash.{tag}", "match": "\\b[0-9A-Fa-f]{8,16}\\b"},
        {"name": f"constant.numeric.{tag}", "match": "(?<![\\w.])-?\\d+(?:\\.\\d+)?\\b"},
        {"name": f"constant.language.boolean.{tag}", "match": "\\b(?i:true|false|yes|no)\\b"},
        {"name": f"keyword.operator.wildcard.{tag}", "match": "\\*"},
        {"name": f"keyword.operator.or.{tag}", "match": "\\|"},
    ]

    grammar = {
        "$schema": "https://raw.githubusercontent.com/martinring/tmlanguage/master/tmlanguage.json",
        "name": display,
        "scopeName": scope,
        "comment": f"Généré par scripts/generate-grammar.py depuis {schema_path}. Ne pas éditer à la main.",
        "patterns": patterns,
        "repository": {},
    }
    out = ROOT / out_path
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(grammar, ensure_ascii=False, indent=2) + "\n", encoding='utf-8')
    print(f"{out.name} : {len(exact) + len(insensitive)} balises, {len(conds)} noms de conditions")


def main():
    for schema_path, out_path, suffix, display in TARGETS:
        build(schema_path, out_path, suffix, display)


if __name__ == '__main__':
    main()
