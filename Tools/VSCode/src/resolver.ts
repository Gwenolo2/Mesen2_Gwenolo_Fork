import * as fs from 'fs';
import * as path from 'path';

/** Profondeur maximale des <include>, comme SnesHdPackLoader.cpp (depth > 20 = arrêt). */
const MAX_INCLUDE_DEPTH = 20;
/** Passes maximales de remplacement d'alias par ligne (SnesHdPackLoader.cpp). */
const MAX_ALIAS_PASSES = 10;

export interface ResolvedLine {
    /** Texte après retrait du commentaire, des espaces de tête et remplacement des alias. */
    text: string;
    /** Fichier d'origine, chemin absolu. */
    file: string;
    /** Numéro de ligne dans ce fichier, à partir de 1. */
    line: number;
    /** Vrai si au moins un @alias a été remplacé. */
    aliased: boolean;
}

export interface ResolvedPack {
    /** Dossier qui contient le fichier d'entrée : racine de tous les chemins du pack. */
    rootFolder: string;
    lines: ResolvedLine[];
    aliases: Map<string, string>;
    /** Messages équivalents à ceux du journal du loader. */
    errors: string[];
}

interface RawLine {
    text: string;
    file: string;
    line: number;
}

/** Même nettoyage que le loader : \r retirés, commentaire # coupé, espaces de tête retirés. */
export function stripLine(raw: string): string {
    let text = raw.replace(/\r/g, '');
    const hash = text.indexOf('#');
    if (hash >= 0) text = text.slice(0, hash);
    return text.replace(/^[ \t]+/, '');
}

function collect(file: string, rootFolder: string, depth: number, out: RawLine[], errors: string[], openTexts?: Map<string, string>): void {
    if (depth > MAX_INCLUDE_DEPTH) {
        errors.push(`Profondeur d'include dépassée à : ${file}`);
        return;
    }
    let content: string;
    try {
        content = openTexts?.get(file) ?? fs.readFileSync(file, 'utf8');
    } catch {
        errors.push(`Fichier inclus introuvable : ${file}`);
        return;
    }
    content.split('\n').forEach((raw, index) => {
        const text = raw.replace(/\r/g, '');
        const stripped = stripLine(raw);
        if (stripped.startsWith('<include>')) {
            const target = stripped.slice('<include>'.length).trim();
            collect(path.resolve(rootFolder, target.replace(/\\/g, '/')), rootFolder, depth + 1, out, errors, openTexts);
        } else {
            out.push({ text, file, line: index + 1 });
        }
    });
}

/** Remplace les @alias d'une ligne, comme la boucle du loader : une substitution par passe. */
export function expandAliases(text: string, aliases: Map<string, string>): { text: string; changed: boolean } {
    let current = text;
    let changed = false;
    let pass = 0;
    let replaced: boolean;
    do {
        replaced = false;
        let at = 0;
        while ((at = current.indexOf('@', at)) >= 0) {
            let end = at + 1;
            while (end < current.length && /[A-Za-z0-9_]/.test(current[end])) end++;
            const name = current.slice(at, end);
            const value = aliases.get(name);
            if (value !== undefined) {
                current = current.slice(0, at) + value + current.slice(end);
                replaced = true;
                changed = true;
                break;
            }
            at = end;
        }
        pass++;
    } while (replaced && pass < MAX_ALIAS_PASSES);
    return { text: current, changed };
}

/**
 * Fichier d'entrée du pack : le snes_hires.txt du dossier du fichier ouvert ou d'un de ses parents.
 * Un .mesen inclus n'est donc pas sa propre racine : ses chemins se lisent depuis le dossier de snes_hires.txt.
 * Si aucun snes_hires.txt n'est trouvé, le fichier ouvert lui-même sert d'entrée.
 */
export function findPackEntry(file: string): string {
    const start = path.resolve(file);
    let dir = path.dirname(start);
    for (;;) {
        const candidate = path.join(dir, 'snes_hires.txt');
        if (fs.existsSync(candidate)) return candidate;
        const parent = path.dirname(dir);
        if (parent === dir) return start;
        dir = parent;
    }
}

/** Racine du pack (dossier de l'entrée) : base de tous les chemins et des <include>. */
export function packRoot(file: string): string {
    return path.dirname(findPackEntry(file));
}

/**
 * Lit le fichier d'entrée du pack et ses <include>, puis applique les alias dans l'ordre de lecture.
 * Reproduit LoadFileLinesRecursive et la boucle principale de SnesHdPackLoader.cpp.
 * `openTexts` remplace le contenu disque des fichiers ouverts dans l'éditeur (chemins absolus).
 */
export function resolvePack(file: string, openTexts?: Map<string, string>): ResolvedPack {
    const entryFile = findPackEntry(file);
    const rootFolder = path.dirname(entryFile);
    const raw: RawLine[] = [];
    const errors: string[] = [];
    collect(entryFile, rootFolder, 0, raw, errors, openTexts);

    const aliases = new Map<string, string>();
    const lines: ResolvedLine[] = [];
    for (const src of raw) {
        const text = stripLine(src.text);
        if (!text) continue;
        if (text.startsWith('<alias>')) {
            const data = text.slice('<alias>'.length);
            const comma = data.indexOf(',');
            if (comma >= 0) {
                aliases.set('@' + data.slice(0, comma).trim(), data.slice(comma + 1).trim());
            } else {
                errors.push(`${src.file}:${src.line} : syntaxe <alias> attendue : <alias>Nom,Valeur`);
            }
            continue;
        }
        const expanded = expandAliases(text, aliases);
        lines.push({ text: expanded.text, file: src.file, line: src.line, aliased: expanded.changed });
    }
    return { rootFolder, lines, aliases, errors };
}

export interface DeclaredCondition {
    name: string;
    /** Ligne <condition> complète, après alias. */
    text: string;
    file: string;
    line: number;
}

/**
 * Conditions déclarées par <condition>nom,type,... : crée nom et !nom (comme le loader).
 * Renvoie les noms de base, dans l'ordre de déclaration.
 */
export function declaredConditions(pack: ResolvedPack): DeclaredCondition[] {
    const result: DeclaredCondition[] = [];
    for (const l of pack.lines) {
        if (!l.text.startsWith('<condition>')) continue;
        const name = l.text.slice('<condition>'.length).split(',')[0].trim();
        if (name) result.push({ name, text: l.text, file: l.file, line: l.line });
    }
    return result;
}
