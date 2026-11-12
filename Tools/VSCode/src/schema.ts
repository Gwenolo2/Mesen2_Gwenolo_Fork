import * as fs from 'fs';
import * as path from 'path';

export interface SchemaParam {
    name: string;
    pos?: number;
    kv?: boolean;
    type?: string;
    required?: boolean;
    default?: unknown;
    values?: string[] | string;
    range?: string;
    alias?: string[];
    desc?: string;
}

export interface SchemaTag {
    /** Nom tel qu'écrit dans le schéma, avec les chevrons : "<tile>". */
    name: string;
    /** Nom sans chevrons : "tile". */
    key: string;
    status: string;
    syntax?: string;
    notes?: string;
    params: SchemaParam[];
}

export interface Schema {
    tags: SchemaTag[];
    /** Recherche insensible à la casse. Les entrées ne gardent que les balises utilisables en priorité. */
    byKey: Map<string, SchemaTag>;
    /** Noms en minuscules, sans chevrons. */
    notRecognized: Set<string>;
    planned: Set<string>;
    /** Balises que le loader compare en minuscules. */
    caseInsensitive: Set<string>;
}

/** Statuts proposés en complétion (une balise planned ou notRecognized n'est jamais proposée). */
export const USABLE = new Set(['implemented', 'partial']);

function bareName(s: string): string {
    const first = s.trim().split(/\s+/)[0];
    return first.replace(/^</, '').replace(/>$/, '').toLowerCase();
}

export function loadSchema(extensionRoot: string): Schema {
    const file = path.join(extensionRoot, 'schemas', '29_schema_balises_snes.json');
    const raw = JSON.parse(fs.readFileSync(file, 'utf8')) as Record<string, unknown>;
    const tags: SchemaTag[] = (raw.tags as Array<Record<string, unknown>>).map(t => {
        const name = String(t.name);
        return {
            name,
            key: name.slice(1, -1),
            status: String(t.status),
            syntax: typeof t.syntax === 'string' ? t.syntax : undefined,
            notes: typeof t.notes === 'string' ? t.notes : undefined,
            params: Array.isArray(t.params) ? (t.params as SchemaParam[]) : [],
        };
    });

    // Une clé peut désigner deux entrées (ex. <spritelight> et <SpriteLight> docOnly) : on garde l'utilisable.
    const byKey = new Map<string, SchemaTag>();
    for (const t of tags) {
        const k = t.key.toLowerCase();
        if (!byKey.has(k) || USABLE.has(t.status)) byKey.set(k, t);
    }

    const notRecognized = new Set(
        ((raw.notRecognizedByLoader ?? []) as Array<{ name: string }>).map(x => bareName(x.name)),
    );
    const planned = new Set(
        ((raw.planned ?? []) as Array<{ name: string }>).map(x => bareName(x.name)),
    );
    const matching = (raw.matching ?? {}) as { caseInsensitive?: string[] };
    const caseInsensitive = new Set((matching.caseInsensitive ?? []).map(bareName));

    return { tags, byKey, notRecognized, planned, caseInsensitive };
}

export interface TagContext {
    tag: SchemaTag;
    /** Colonne de début du <balise>. */
    start: number;
    /** Colonne juste après le >. */
    end: number;
    /** Texte entre la fin de la balise et le curseur. */
    argsText: string;
    /** Nombre de virgules avant le curseur : index du champ courant. */
    argIndex: number;
}

const TAG_RE = /<([A-Za-z][A-Za-z0-9_]*)>/g;

/** Colonne où commence le commentaire #, ou longueur de la ligne. */
function commentStart(text: string): number {
    const hash = text.indexOf('#');
    return hash >= 0 ? hash : text.length;
}

function allTags(text: string): RegExpExecArray[] {
    const out: RegExpExecArray[] = [];
    const re = new RegExp(TAG_RE.source, 'g');
    let m: RegExpExecArray | null;
    while ((m = re.exec(text)) !== null) out.push(m);
    return out;
}

/** Dernière balise terminée avant le curseur : contexte des arguments. */
export function tagBefore(text: string, col: number, schema: Schema): TagContext | undefined {
    if (col > commentStart(text)) return undefined;
    let last: RegExpExecArray | undefined;
    for (const m of allTags(text)) {
        if (m.index + m[0].length <= col) last = m;
    }
    if (!last) return undefined;
    const tag = schema.byKey.get(last[1].toLowerCase());
    if (!tag) return undefined;
    const end = last.index + last[0].length;
    const argsText = text.slice(end, col);
    return { tag, start: last.index, end, argsText, argIndex: (argsText.match(/,/g) ?? []).length };
}

/** Balise sous le curseur (survol). */
export function tagCovering(text: string, col: number, schema: Schema): SchemaTag | undefined {
    for (const m of allTags(text)) {
        if (col >= m.index && col <= m.index + m[0].length) {
            return schema.byKey.get(m[1].toLowerCase());
        }
    }
    return undefined;
}

/** Si le curseur est dans un préfixe [..] non fermé, renvoie ce qui est déjà écrit dedans. */
export function conditionPrefix(text: string, col: number): string | undefined {
    const m = /^\s*\[([^\]]*)$/.exec(text.slice(0, col));
    return m ? m[1] : undefined;
}
