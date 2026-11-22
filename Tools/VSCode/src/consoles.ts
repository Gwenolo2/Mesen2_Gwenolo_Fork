/** Console d'un pack HD : fichier d'entrée, schéma, champs fichiers et conditions intégrées propres à chaque console. */
export type HdConsole = 'snes' | 'nes';

export interface ConsoleProfile {
    id: HdConsole;
    /** Identifiant du langage VS Code. */
    languageId: string;
    /** Nom du fichier d'entrée du pack : marqueur de la console. */
    entryName: string;
    /** Schéma des balises, dans le dossier schemas/. */
    schemaFile: string;
    /** Index (0-based) du champ fichier de chaque balise, en minuscules. */
    fileField: Record<string, number>;
    /** Conditions intégrées de la console, avec leur description. */
    builtins: Record<string, string>;
    /** Règles propres au SNES : palettes en hexadécimal, <scale> avant les tiles, hash de tile. */
    snesRules: boolean;
}

/** Source : schemas/29_schema_balises_snes.json et SnesHdPackLoader.cpp. */
const FILE_FIELD_SNES: Record<string, number> = {
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

/** Source : claude/19_Tags_HdPack_NES.md et HdPackLoader.cpp. */
const FILE_FIELD_NES: Record<string, number> = {
    'img': 0,
    'tile': 0,
    'tile_hflip': 0,
    'tile_vflip': 0,
    'tile_hvflip': 0,
    'background': 0,
    'video': 0,
    '2doverlay': 0,
    'bgm': 2,
    'sfx': 2,
    'patch': 0,
    'dioramaobject': 0,
    'dioramanormal': 0,
    'tile2obj': 0,
    'sprite2obj': 0,
    'daynight': 0,
    'loadingscreen': 0,
    'setvariable': 1,
};

export const SNES_PROFILE: ConsoleProfile = {
    id: 'snes',
    languageId: 'mesen-snes-hd',
    entryName: 'snes_hires.txt',
    schemaFile: '29_schema_balises_snes.json',
    fileField: FILE_FIELD_SNES,
    builtins: {
        hmirror: 'Vraie si la tile évaluée est retournée horizontalement.',
        vmirror: 'Vraie si la tile évaluée est retournée verticalement.',
        gpu3d: 'Vraie sauf si <gpu3d>off est utilisé (moteur 3D actif par défaut).',
        widescreen: 'Vraie si <widescreen> vaut mode7 ou all.',
        widescreenMode7: 'Vraie si <widescreen>mode7 ou all.',
    },
    snesRules: true,
};

export const NES_PROFILE: ConsoleProfile = {
    id: 'nes',
    languageId: 'mesen-nes-hd',
    entryName: 'hires.txt',
    schemaFile: 'schema_balises_nes.json',
    fileField: FILE_FIELD_NES,
    builtins: {
        hmirror: 'Indicateur intégré : vrai selon la tuile réelle à l\'écran (sprites), claude/19 § 5.',
        vmirror: 'Indicateur intégré : vrai selon la tuile réelle à l\'écran (sprites), claude/19 § 5.',
        bgpriority: 'Indicateur intégré NES, claude/19 § 5.',
        sppalette0: 'Indicateur intégré NES : palette de sprite 0, claude/19 § 5.',
        sppalette1: 'Indicateur intégré NES : palette de sprite 1, claude/19 § 5.',
        sppalette2: 'Indicateur intégré NES : palette de sprite 2, claude/19 § 5.',
        sppalette3: 'Indicateur intégré NES : palette de sprite 3, claude/19 § 5.',
    },
    snesRules: false,
};

export const PROFILES: ConsoleProfile[] = [SNES_PROFILE, NES_PROFILE];

export function profileForLanguage(languageId: string): ConsoleProfile | undefined {
    return PROFILES.find(p => p.languageId === languageId);
}

/**
 * Profil d'après le nom du fichier : snes_hires.txt = SNES, hires.txt = NES.
 * Prioritaire sur la langue VS Code, qui peut être mal détectée ou choisie à la main.
 */
export function profileForFileName(fileName: string): ConsoleProfile | undefined {
    const base = (fileName.split(/[\\/]/).pop() ?? '').toLowerCase();
    return PROFILES.find(p => p.entryName.toLowerCase() === base);
}

/** Profil d'un document : nom du fichier d'abord, puis langue VS Code en repli. */
export function profileForDocument(doc: { fileName: string; languageId: string }): ConsoleProfile | undefined {
    return profileForFileName(doc.fileName) ?? profileForLanguage(doc.languageId);
}

/** Vrai si le nom est une condition intégrée de la console (sans le !). */
export function isBuiltin(profile: ConsoleProfile, name: string): boolean {
    return Object.prototype.hasOwnProperty.call(profile.builtins, name);
}
