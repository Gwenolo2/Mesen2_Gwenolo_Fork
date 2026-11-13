import * as path from 'path';

/** Référence à un fichier ou à un dossier aléatoire, telle qu'écrite dans le pack. */
export interface PackRef {
    /** `file` : chemin vers un fichier existant à vérifier. `folder` : dossier parent d'un segment randomize. */
    kind: 'file' | 'folder';
    /** Valeur brute, sans espaces de bord. */
    value: string;
    /** Chemin relatif à la racine du pack (pour `folder`, le dossier avant le segment randomize). */
    path: string;
    /** Colonnes de début et de fin dans la ligne (0-based, fin exclue). */
    start: number;
    end: number;
}

export interface PackLine {
    tag: string;
    refs: PackRef[];
}

/**
 * Index (0-based) du champ fichier dans la liste séparée par des virgules, pour chaque balise.
 * Clés en minuscules : le loader compare certaines balises en minuscules et d'autres exactement.
 * Source : schemas/29_schema_balises_snes.json et SnesHdPackLoader.cpp.
 */
const FILE_FIELD: Record<string, number> = {
    'img': 0,
    'tile': 2,
    'tile_hflip': 2,
    'tile_vflip': 2,
    'tile_hvflip': 2,
    'background': 0,
    'm7img': 0,
    'm7tiles': 0,
    '2doverlay': 0,
    '2dperspective': 0,
    'msutoogg': 1,
    'video': 0,
    'loadingscreen': 0,
    'm7model': 0,
    'patch': 0,
};

const TAG = /<([A-Za-z][A-Za-z0-9_]*)>/;
const FILE_EXT = /\.(png|ogg|json|obj|mtl|gltf|glb|mpg|mp4)$/i;
const RANDOM_SEGMENT = /^random(ize)?$/i;

/**
 * Analyse une ligne du pack : retire le commentaire `#`, saute le préfixe [conditions],
 * trouve la balise et renvoie les références à des fichiers situées dans le champ attendu.
 * Ne gère pas encore <include> ni les alias @Nom (étape 3).
 */
export function parseLine(line: string): PackLine | undefined {
    const hash = line.indexOf('#');
    const content = hash >= 0 ? line.slice(0, hash) : line;
    const prefix = /^\s*\[[^\]]*\]/.exec(content);
    const from = prefix ? prefix[0].length : 0;

    const tagMatch = TAG.exec(content.slice(from));
    if (!tagMatch) return undefined;
    const tag = tagMatch[1];
    const tagEnd = from + tagMatch.index + tagMatch[0].length;

    const fieldIndex = FILE_FIELD[tag.toLowerCase()];
    if (fieldIndex === undefined) return { tag, refs: [] };

    // Repère le champ voulu sans modifier les autres : offset de début de chaque champ.
    const fields = content.slice(tagEnd).split(',');
    if (fieldIndex >= fields.length) return { tag, refs: [] };
    let offset = tagEnd;
    for (let i = 0; i < fieldIndex; i++) offset += fields[i].length + 1;

    const raw = fields[fieldIndex];
    const value = raw.trim();
    if (!value || /^(disable|blank)$/i.test(value)) return { tag, refs: [] };

    const start = offset + (raw.length - raw.trimStart().length);
    const end = start + value.length;

    const segments = value.split(/[\\/]/);
    const randomAt = segments.findIndex(s => RANDOM_SEGMENT.test(s));
    if (randomAt >= 0) {
        return {
            tag,
            refs: [{ kind: 'folder', value, path: segments.slice(0, randomAt).join('/'), start, end }],
        };
    }
    if (FILE_EXT.test(value)) {
        return { tag, refs: [{ kind: 'file', value, path: value, start, end }] };
    }
    return { tag, refs: [] };
}

/** Chemin absolu d'une référence : relatif à la racine du pack (dossier de snes_hires.txt), comme le loader. */
export function resolveRef(rootFolder: string, ref: PackRef): string {
    return path.resolve(rootFolder, ref.path.replace(/\\/g, '/'));
}
