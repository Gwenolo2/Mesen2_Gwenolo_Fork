import * as fs from 'fs';
import * as path from 'path';
import * as vscode from 'vscode';
import { readPngSize } from './png';
import { openPackTexts } from './openDocs';
import { parseLine, resolveRef } from './packParser';
import { ResolvedLine, declaredConditions, resolvePack } from './resolver';
import { Schema } from './schema';

/** Conditions intégrées (SnesHdPackLoader.cpp, ligne ~343). */
const BUILTIN = new Set(['hmirror', 'vmirror', 'gpu3d', 'widescreen', 'widescreenMode7']);
const TILE_TAGS = new Set(['tile', 'tile_hflip', 'tile_vflip', 'tile_hvflip']);
const SOURCE = 'Mesen HD';

interface Found {
    severity: vscode.DiagnosticSeverity;
    code: string;
    message: string;
}

/** Noms des conditions du préfixe [a&!b], sans le ! ni les espaces. */
function conditionNames(text: string): string[] {
    const m = /^\s*\[([^\]]*)\]/.exec(text);
    if (!m) return [];
    return m[1].split('&').map(s => s.trim().replace(/^!\s*/, '')).filter(Boolean);
}

/** Longueur du préfixe [..] éventuel. */
function prefixLength(text: string): number {
    const m = /^\s*\[[^\]]*\]/.exec(text);
    return m ? m[0].length : 0;
}

/** Clé des conditions d'un préfixe [..] : triée, sans espaces, ! inclus (deux tiles identiques si mêmes conditions). */
function conditionKey(text: string): string {
    const m = /^\s*\[([^\]]*)\]/.exec(text);
    if (!m) return '';
    return m[1].split('&').map(s => s.replace(/\s+/g, '')).filter(Boolean).sort().join('&');
}

/** Ligne entière (sans les espaces de tête), pour que le soulignement soit visible. */
function lineRange(doc: vscode.TextDocument, index: number): vscode.Range {
    const text = doc.lineAt(index).text;
    return new vscode.Range(index, text.length - text.trimStart().length, index, text.length);
}

/**
 * Diagnostics du fichier ouvert. L'analyse suit l'ordre du loader : includes et alias résolus,
 * conditions valables seulement après leur déclaration, <scale> avant les tiles.
 * Le texte de l'éditeur est utilisé pour le fichier ouvert (modifications non sauvegardées incluses).
 */
export function computeDiagnostics(doc: vscode.TextDocument, schema: Schema): vscode.Diagnostic[] {
    const docPath = path.resolve(doc.uri.fsPath);
    const pack = resolvePack(docPath, openPackTexts());
    const declared = new Set<string>();
    const pngCache = new Map<string, { width: number; height: number } | undefined>();
    const diags: vscode.Diagnostic[] = [];
    let catalog = 0;
    const seenTiles = new Map<string, ResolvedLine>();
    let scale: number | undefined;
    let scaleSeen = false;

    const report = (r: ResolvedLine, f: Found): void => {
        if (r.file !== docPath) return;
        const d = new vscode.Diagnostic(lineRange(doc, r.line - 1), f.message, f.severity);
        d.source = SOURCE;
        d.code = f.code;
        diags.push(d);
    };

    for (const r of pack.lines) {
        const text = r.text;

        // Conditions : doivent être déclarées avant la ligne (ordre de lecture).
        for (const c of conditionNames(text)) {
            if (!declared.has(c) && !BUILTIN.has(c)) {
                report(r, {
                    severity: vscode.DiagnosticSeverity.Error,
                    code: 'cond-undeclared',
                    message: `Condition « ${c} » non déclarée avant cette ligne : le loader l'ignore sans message.`,
                });
            }
        }

        if (text.startsWith('<condition>')) {
            const name = text.slice('<condition>'.length).split(',')[0].trim();
            if (name) declared.add(name);
            continue;
        }

        const body = text.slice(prefixLength(text)).trimStart();
        const m = /<([A-Za-z][A-Za-z0-9_]*)>/.exec(body);
        if (!m) continue;
        const written = m[1];
        const key = written.toLowerCase();
        const tag = schema.byKey.get(key);

        // Balise inconnue, absente ou planifiée.
        if (!tag) {
            if (body.startsWith('<')) {
                if (schema.notRecognized.has(key)) {
                    report(r, { severity: vscode.DiagnosticSeverity.Error, code: 'tag-not-recognized',
                        message: `<${written}> est absente du loader SNES : elle sera ignorée.` });
                } else if (schema.planned.has(key)) {
                    report(r, { severity: vscode.DiagnosticSeverity.Error, code: 'tag-planned',
                        message: `<${written}> n'est pas encore implémentée.` });
                } else {
                    report(r, { severity: vscode.DiagnosticSeverity.Error, code: 'tag-unknown',
                        message: `Balise inconnue <${written}> : elle sera ignorée sans message.` });
                }
            }
            continue;
        }

        // Casse : certaines balises sont sensibles à la casse dans le loader.
        if (written !== tag.key && !schema.caseInsensitive.has(key)) {
            report(r, { severity: vscode.DiagnosticSeverity.Error, code: 'tag-case',
                message: `Casse incorrecte : <${written}>, le loader attend ${tag.name} (sinon ignorée).` });
        }
        if (tag.status === 'docOnly') {
            report(r, { severity: vscode.DiagnosticSeverity.Information, code: 'tag-doc-only',
                message: `<${tag.key}> est décrite dans les docs mais non vérifiée dans le code.` });
        }

        const argsText = body.slice(body.indexOf('>') + 1);
        const args = argsText.trim() === '' ? [] : argsText.split(',');
        const positional: string[] = [];
        for (const a of args) {
            if (a.includes('=')) break;
            positional.push(a.trim());
        }

        // Tile définie plusieurs fois avec les mêmes conditions : doublon probable.
        if (TILE_TAGS.has(tag.key) && positional[0] && /^[0-9A-Fa-f]+$/.test(positional[0])) {
            const tileKey = `${positional[0].toUpperCase()}|${conditionKey(text)}`;
            const first = seenTiles.get(tileKey);
            if (!first) {
                seenTiles.set(tileKey, r);
            } else {
                report(r, { severity: vscode.DiagnosticSeverity.Warning, code: 'tile-duplicate',
                    message: `Tile ${positional[0].toUpperCase()} déjà définie avec les mêmes conditions (${path.basename(first.file)}:${first.line}) : doublon probable.` });
            }
        }

        // Nombre de paramètres.
        let need = tag.params.filter(p => !p.kv && p.required).length;
        if (TILE_TAGS.has(tag.key) && positional[2] !== undefined && /^(disable|blank)$/i.test(positional[2])) {
            need = 2;
        }
        if (positional.length < need && !/\{/.test(text)) {
            report(r, { severity: vscode.DiagnosticSeverity.Error, code: 'params-missing',
                message: `<${tag.key}> : ${need} paramètre(s) requis, ${positional.length} fourni(s).` });
        }

        // Palettes : décimal, de 0 à 15 (ou * / all).
        const paletteFields = TILE_TAGS.has(tag.key) || tag.key === 'disableTile' || tag.key.toLowerCase() === 'distabletile'
            ? [1]
            : tag.key.toLowerCase() === 'addition' ? [1, 3] : [];
        for (const i of paletteFields) {
            const v = positional[i];
            if (v === undefined || v === '' || v === '*' || v.toLowerCase() === 'all') continue;
            if (!/^\d+$/.test(v)) {
                report(r, { severity: vscode.DiagnosticSeverity.Error, code: 'palette-format',
                    message: `Palette « ${v} » : les palettes s'écrivent en décimal.` });
            } else if (Number(v) > 15) {
                report(r, { severity: vscode.DiagnosticSeverity.Error, code: 'palette-range',
                    message: `Palette ${v} hors de 0-15.` });
            }
        }

        // Fichiers référencés et index d'image.
        if (TILE_TAGS.has(tag.key) && positional[2] !== undefined && /^\d+$/.test(positional[2])) {
            const index = Number(positional[2]);
            if (index >= catalog) {
                report(r, { severity: vscode.DiagnosticSeverity.Error, code: 'img-index',
                    message: `Index d'image ${index} hors du catalogue (${catalog} <img> déclaré(s) avant cette ligne).` });
            }
        }
        for (const ref of parseLine(text)?.refs ?? []) {
            if (ref.kind !== 'file') continue;
            const target = resolveRef(pack.rootFolder, ref);
            if (!fs.existsSync(target)) {
                report(r, { severity: vscode.DiagnosticSeverity.Error, code: 'file-missing',
                    message: `Fichier introuvable : ${ref.path}` });
                continue;
            }
            // Zone de tile hors de l'image (en-tête PNG).
            if (TILE_TAGS.has(tag.key) && scale !== undefined && ref.path.toLowerCase().endsWith('.png')) {
                const x = Number(positional[3]);
                const y = Number(positional[4]);
                if (!Number.isNaN(x) && !Number.isNaN(y)) {
                    if (!pngCache.has(target)) pngCache.set(target, readPngSize(target));
                    const size = pngCache.get(target);
                    const span = 8 * scale;
                    if (size && (x + span > size.width || y + span > size.height)) {
                        report(r, { severity: vscode.DiagnosticSeverity.Warning, code: 'tile-outside',
                            message: `Zone ${x},${y} de ${span} px hors de l'image ${size.width}×${size.height}.` });
                    }
                }
            }
        }

        // Ordre : <scale> avant les tiles.
        if (TILE_TAGS.has(tag.key) && !scaleSeen) {
            report(r, { severity: vscode.DiagnosticSeverity.Error, code: 'scale-missing',
                message: 'Aucun <scale> avant cette balise : la taille des tiles est inconnue.' });
        }

        // Mise à jour de l'état pour les lignes suivantes.
        if (tag.key === 'scale') {
            const n = Number(positional[0] ?? args[0]);
            if (!Number.isNaN(n)) {
                scale = n;
                scaleSeen = true;
            }
        } else if (tag.key === 'img') {
            catalog++;
        }
    }

    // Conditions déclarées mais jamais utilisées dans le pack, fichier principal et includes (information seulement).
    const used = new Set<string>();
    for (const l of pack.lines) for (const c of conditionNames(l.text)) used.add(c);
    for (const c of declaredConditions(pack)) {
        if (c.file !== docPath || used.has(c.name)) continue;
        const d = new vscode.Diagnostic(lineRange(doc, c.line - 1),
            `Condition « ${c.name} » déclarée mais jamais utilisée dans le pack.`, vscode.DiagnosticSeverity.Information);
        d.source = SOURCE;
        d.code = 'cond-unused';
        diags.push(d);
    }
    return diags;
}
