import * as vscode from 'vscode';
import { declaredConditions, resolvePack } from './resolver';
import { Schema, USABLE, conditionPrefix, tagBefore } from './schema';

/** Conditions intégrées (SnesHdPackLoader.cpp, ligne ~343). */
const BUILTIN_CONDITIONS = ['hmirror', 'vmirror', 'gpu3d', 'widescreen', 'widescreenMode7'];

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

        // 2. Nom de balise en cours de frappe, après '<'.
        const partial = /<([A-Za-z0-9_]*)$/.exec(before);
        if (partial) return this.tagItems(pos, col - partial[0].length);

        // 3. Début de ligne (éventuellement après un préfixe fermé) : proposer les balises.
        if (/^\s*(\[[^\]]*\]\s*)?$/.test(before)) return this.tagItems(pos, col);

        // 4. Arguments d'une balise.
        const ctx = tagBefore(text, col, this.schema);
        if (ctx) return this.argItems(pos, ctx.tag, ctx.argsText, ctx.argIndex);

        return [];
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
