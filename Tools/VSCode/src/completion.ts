import * as fs from 'fs';
import * as path from 'path';
import * as vscode from 'vscode';
import { FILE_FIELD } from './packParser';
import { declaredConditions, packRoot, resolvePack } from './resolver';
import { Schema, USABLE, conditionPrefix, tagBefore } from './schema';

/** Conditions intégrées (SnesHdPackLoader.cpp, ligne ~343 et ~357). */
const BUILTIN_CONDITIONS = ['hmirror', 'vmirror', 'gpu3d', 'widescreen', 'widescreenMode7'];

/** Balises dont le fichier est une image PNG. */
const PNG_TAGS = new Set(['img', 'tile', 'tile_hflip', 'tile_vflip', 'tile_hvflip', 'background', 'm7img', 'm7tiles', 'loadingscreen']);

/** Extension attendue selon la balise ; sinon tous les fichiers sont proposés. */
function extensionFilter(tag: string): RegExp | undefined {
    if (PNG_TAGS.has(tag)) return /\.png$/i;
    if (tag === 'msutoogg') return /\.ogg$/i;
    if (tag === 'video') return /\.(mpg|mp4)$/i;
    if (tag === 'm7model') return /\.(obj|gltf|glb)$/i;
    return undefined;
}

interface FileArg {
    /** Texte déjà tapé dans le champ fichier, depuis le début du champ. */
    typed: string;
    tag: string;
}

/** Si le curseur est dans le champ fichier d'une balise (index donné par FILE_FIELD), renvoie le texte tapé. */
function fileArgAt(text: string, col: number): FileArg | undefined {
    const before = text.slice(0, col);
    const hash = text.indexOf('#');
    if (hash >= 0 && col > hash) return undefined;
    const prefix = /^\s*\[[^\]]*\]/.exec(before);
    const from = prefix ? prefix[0].length : 0;
    let last: RegExpExecArray | undefined;
    const re = /<([A-Za-z][A-Za-z0-9_]*)>/g;
    let m: RegExpExecArray | null;
    while ((m = re.exec(before)) !== null) {
        if (m.index >= from) last = m;
    }
    if (!last) return undefined;
    const tag = last[1].toLowerCase();
    const fieldIndex = FILE_FIELD[tag];
    if (fieldIndex === undefined) return undefined;
    const fields = before.slice(last.index + last[0].length).split(',');
    if (fields.length - 1 !== fieldIndex) return undefined;
    return { typed: fields[fieldIndex].trimStart(), tag };
}

export class PackCompletionProvider implements vscode.CompletionItemProvider {
    constructor(private readonly schema: Schema) {}

    provideCompletionItems(doc: vscode.TextDocument, pos: vscode.Position): vscode.CompletionItem[] {
        const text = doc.lineAt(pos.line).text;
        const col = pos.character;
        const before = text.slice(0, col);

        // 1. Dans un préfixe [..] : conditions déclarées (et leurs inverses).
        const cond = conditionPrefix(text, col);
        if (cond !== undefined) {
            const token = cond.split('&').pop() ?? '';
            return this.conditionItems(doc, pos, token);
        }

        // 2. Champ fichier d'une balise (png, ogg, json…) : fichiers et dossiers du pack.
        const file = fileArgAt(text, col);
        if (file) return this.fileItems(doc, pos, file);

        // 3. Nom de balise en cours de frappe, après '<'.
        const partial = /<([A-Za-z0-9_]*)$/.exec(before);
        if (partial) return this.tagItems(pos, col - partial[0].length);

        // 4. Début de ligne (éventuellement après un préfixe fermé) : proposer les balises.
        if (/^\s*(\[[^\]]*\]\s*)?$/.test(before)) return this.tagItems(pos, col);

        // 5. Arguments d'une balise.
        const ctx = tagBefore(text, col, this.schema);
        if (ctx) return this.argItems(pos, ctx.tag, ctx.argsText, ctx.argIndex);

        return [];
    }

    private fileItems(doc: vscode.TextDocument, pos: vscode.Position, arg: FileArg): vscode.CompletionItem[] {
        const root = packRoot(doc.uri.fsPath);
        const cut = Math.max(arg.typed.lastIndexOf('\\'), arg.typed.lastIndexOf('/'));
        const dirTyped = arg.typed.slice(0, cut + 1);
        const sep = dirTyped.includes('/') && !dirTyped.includes('\\') ? '/' : '\\';
        const dirAbs = path.resolve(root, dirTyped.replace(/[\\/]/g, '/'));
        let entries: fs.Dirent[];
        try {
            entries = fs.readdirSync(dirAbs, { withFileTypes: true });
        } catch {
            return [];
        }
        const filter = extensionFilter(arg.tag);
        const range = new vscode.Range(pos.line, pos.character - arg.typed.length, pos.line, pos.character);
        const items: vscode.CompletionItem[] = [];
        for (const e of entries) {
            if (e.name.startsWith('.')) continue;
            const isDir = e.isDirectory();
            if (!isDir && filter && !filter.test(e.name)) continue;
            const insert = dirTyped + e.name + (isDir ? sep : '');
            const item = new vscode.CompletionItem(e.name, isDir ? vscode.CompletionItemKind.Folder : vscode.CompletionItemKind.File);
            item.insertText = insert;
            item.filterText = insert;
            item.range = range;
            item.detail = isDir ? 'dossier du pack' : 'fichier du pack';
            if (isDir) item.command = { command: 'editor.action.triggerSuggest', title: 'Suggérer à nouveau' };
            items.push(item);
        }
        return items;
    }

    private tagItems(pos: vscode.Position, startCol: number): vscode.CompletionItem[] {
        const range = new vscode.Range(pos.line, startCol, pos.line, pos.character);
        return this.schema.tags
            .filter(t => USABLE.has(t.status))
            .map(t => {
                const item = new vscode.CompletionItem(t.name, vscode.CompletionItemKind.Keyword);
                item.insertText = t.name;
                item.range = range;
                item.detail = t.syntax ?? t.status;
                if (t.notes) item.documentation = new vscode.MarkdownString(t.notes);
                return item;
            });
    }

    private conditionItems(doc: vscode.TextDocument, pos: vscode.Position, token: string): vscode.CompletionItem[] {
        const range = new vscode.Range(pos.line, pos.character - token.length, pos.line, pos.character);
        const declared = declaredConditions(resolvePack(doc.uri.fsPath)).map(c => c.name);
        const names = [...new Set([...declared, ...BUILTIN_CONDITIONS])];
        const items: vscode.CompletionItem[] = [];
        for (const n of names) {
            for (const label of [n, '!' + n]) {
                const item = new vscode.CompletionItem(label, vscode.CompletionItemKind.Variable);
                item.range = range;
                item.detail = label.startsWith('!') ? 'inverse de la condition' : 'condition déclarée';
                items.push(item);
            }
        }
        return items;
    }

    private argItems(pos: vscode.Position, tag: { params: import('./schema').SchemaParam[] }, argsText: string, argIndex: number): vscode.CompletionItem[] {
        const current = argsText.split(',').pop() ?? '';
        const range = new vscode.Range(pos.line, pos.character - current.trim().length, pos.line, pos.character);
        const eq = current.indexOf('=');

        // Valeur d'un paramètre clé=valeur : valeurs énumérées.
        if (eq >= 0) {
            const key = current.slice(0, eq).trim().toLowerCase();
            const param = tag.params.find(p => p.kv && (p.name.toLowerCase() === key || (p.alias ?? []).some(a => a.toLowerCase() === key)));
            return this.valueItems(param?.values, pos, range);
        }

        const items: vscode.CompletionItem[] = [];

        // Valeur du paramètre positionnel courant, si enumérée.
        const positional = tag.params.filter(p => !p.kv).sort((a, b) => (a.pos ?? 0) - (b.pos ?? 0));
        const current_ = positional[argIndex];
        items.push(...this.valueItems(current_?.values, pos, range));

        // Noms de paramètres clé=valeur.
        for (const p of tag.params.filter(p => p.kv)) {
            for (const label of [p.name, ...(p.alias ?? [])]) {
                const item = new vscode.CompletionItem(label + '=', vscode.CompletionItemKind.Property);
                item.range = range;
                item.detail = p.desc ?? p.type;
                items.push(item);
            }
        }
        return items;
    }

    private valueItems(values: string[] | string | undefined, pos: vscode.Position, range: vscode.Range): vscode.CompletionItem[] {
        if (!Array.isArray(values)) return [];
        return values.map(v => {
            const item = new vscode.CompletionItem(v, vscode.CompletionItemKind.EnumMember);
            item.range = range;
            return item;
        });
    }
}
