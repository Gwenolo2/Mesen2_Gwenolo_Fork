#!/usr/bin/env python3
"""Génère syntaxes/mesen-snes-hd.tmLanguage.json depuis schemas/29_schema_balises_snes.json.

Relancer après toute modification du schéma : python3 scripts/generate-grammar.py
"""
import json
import pathlib
import re

ROOT = pathlib.Path(__file__).resolve().parent.parent
SCHEMA = ROOT / 'schemas' / '29_schema_balises_snes.json'
OUT = ROOT / 'syntaxes' / 'mesen-snes-hd.tmLanguage.json'

# Balises comparées en minuscules par SnesHdPackLoader.cpp ; les autres sont sensibles à la casse.
CASE_INSENSITIVE = {
    'frontof3d', 'ignore2doverlays', 'spritelight', '2dspritelight', 'noliquidreflect',
    'nolightfxonsprite', 'm7effect', 'm7liquidbackground', 'm7material', '2dreflect',
}


def alt(words):
    return '|'.join(re.escape(w) for w in sorted(set(words), key=len, reverse=True))


def main():
    s = json.loads(SCHEMA.read_text(encoding='utf-8'))

    tags = [t['name'][1:-1] for t in s['tags'] if t['status'] != 'docOnly']
    exact = [t for t in tags if t.lower() not in CASE_INSENSITIVE]
    insensitive = [t for t in tags if t.lower() in CASE_INSENSITIVE]
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
        {"name": "comment.line.number-sign.mesen-snes-hd", "match": "#.*$"},
        {
            "name": "meta.condition.mesen-snes-hd",
            "begin": "^\\s*\\[",
            "end": "\\]",
            "beginCaptures": {"0": {"name": "punctuation.definition.condition.begin.mesen-snes-hd"}},
            "endCaptures": {"0": {"name": "punctuation.definition.condition.end.mesen-snes-hd"}},
            "patterns": [
                {"name": "keyword.operator.and.mesen-snes-hd", "match": "&"},
                {"name": "keyword.operator.not.mesen-snes-hd", "match": "!"},
                {"name": "support.function.condition.mesen-snes-hd", "match": "\\b(?:" + alt(conds) + ")\\b"},
                {"name": "constant.numeric.hash.mesen-snes-hd", "match": "\\b[0-9A-Fa-f]{8,16}\\b"},
                {"name": "constant.numeric.mesen-snes-hd", "match": "-?\\d+(?:\\.\\d+)?"},
                {"name": "keyword.operator.or.mesen-snes-hd", "match": "\\|"},
            ],
        },
        {"name": "punctuation.section.block.mesen-snes-hd", "match": "\\}>?|\\{"},
        {"name": "keyword.control.tag.mesen-snes-hd", "match": "<(?:" + alt(exact) + ")>"},
        {"name": "keyword.control.tag.mesen-snes-hd", "match": "(?i:<(?:" + alt(insensitive) + ")>)"},
        {"name": "invalid.deprecated.tag.mesen-snes-hd", "match": "(?i:<(?:" + alt(deprecated) + ")>)"},
        {"name": "invalid.illegal.tag.mesen-snes-hd", "match": "<[^>\\s]*>"},
        {"name": "variable.parameter.mesen-snes-hd", "match": "(?<=[,>\\s])[A-Za-z_]\\w*(?==)"},
        {
            "name": "string.unquoted.path.mesen-snes-hd",
            "match": "[^\\s,\\[\\]{}<>=|#]+\\.(?i:png|ogg|json|obj|mtl|gltf|glb|mpg|mp4|txt|mesen)\\b",
        },
        {"name": "constant.numeric.hash.mesen-snes-hd", "match": "\\b[0-9A-Fa-f]{8,16}\\b"},
        {"name": "constant.numeric.mesen-snes-hd", "match": "(?<![\\w.])-?\\d+(?:\\.\\d+)?\\b"},
        {"name": "constant.language.boolean.mesen-snes-hd", "match": "\\b(?i:true|false|yes|no)\\b"},
        {"name": "keyword.operator.wildcard.mesen-snes-hd", "match": "\\*"},
        {"name": "keyword.operator.or.mesen-snes-hd", "match": "\\|"},
    ]

    grammar = {
        "$schema": "https://raw.githubusercontent.com/martinring/tmlanguage/master/tmlanguage.json",
        "name": "Mesen HD Pack (SNES)",
        "scopeName": "source.mesen-snes-hd",
        "comment": "Généré par scripts/generate-grammar.py depuis schemas/29_schema_balises_snes.json. Ne pas éditer à la main.",
        "patterns": patterns,
        "repository": {},
    }
    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text(json.dumps(grammar, ensure_ascii=False, indent=2) + "\n", encoding='utf-8')
    print(f"{OUT.name} : {len(exact) + len(insensitive)} balises, {len(conds)} noms de conditions")


if __name__ == '__main__':
    main()
