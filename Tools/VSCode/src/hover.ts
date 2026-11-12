import * as path from 'path';
import * as vscode from 'vscode';
import { declaredConditions, resolvePack } from './resolver';
import { Schema, SchemaParam, SchemaTag, tagBefore, tagCovering } from './schema';

/** Conditions intégrées (SnesHdPackLoader.cpp, ligne ~343). */
const BUILTIN_CONDITIONS: Record<string, string> = {
    hmirror: 'Vraie si la tile évaluée est retournée horizontalement.',
    vmirror: 'Vraie si la tile évaluée est retournée verticalement.',
};

/** Nom de condition sous le curseur dans un préfixe [ ], sans le ! ni les espaces. */
function conditionAt(text: string, col: number): string | undefined {
    const m = /^\s*\[([^\]]*)\]/.exec(text);
    if (!m) return undefined;
    let offset = m[0].indexOf('[') + 1;
    for (const part of m[1].split('&')) {
        const name = part.trim().replace(/^!\s*/, '');
        if (name) {
            const start = offset + part.indexOf(name);
            if (col >= start && col <= start + name.length) return name;
        }
        offset += part.length + 1;
    }
    return undefined;
}

export class PackHoverProvider implements vscode.HoverProvider {
    constructor(private readonly schema: Schema) {}

    provideHover(doc: vscode.TextDocument, pos: vscode.Position): vscode.Hover | undefined {
        const text = doc.lineAt(pos.line).text;

        const condName = conditionAt(text, pos.character);
        if (condName !== undefined) return new vscode.Hover(this.conditionMarkdown(doc, condName));

        const tag = tagCovering(text, pos.character, this.schema);
        if (tag) return new vscode.Hover(this.tagMarkdown(tag));

        // Nom de paramètre clé=valeur sous le curseur.
        const re = /[A-Za-z_][A-Za-z0-9_]*(?==)/g;
        let m: RegExpExecArray | null;
        while ((m = re.exec(text)) !== null) {
            if (pos.character < m.index || pos.character > m.index + m[0].length) continue;
            const ctx = tagBefore(text, m.index, this.schema);
            if (!ctx) return undefined;
            const param = this.findParam(ctx.tag, m[0]);
            if (!param) return undefined;
            return new vscode.Hover(this.paramMarkdown(ctx.tag, param));
        }
        return undefined;
    }

    private conditionMarkdown(doc: vscode.TextDocument, name: string): vscode.MarkdownString {
        const md = new vscode.MarkdownString();
        md.appendMarkdown(`**${name}**\n\n`);
        if (BUILTIN_CONDITIONS[name]) {
            md.appendMarkdown(`Condition intégrée. ${BUILTIN_CONDITIONS[name]}`);
            return md;
        }
        const pack = resolvePack(doc.uri.fsPath);
        const def = declaredConditions(pack).find(c => c.name === name);
        if (!def) {
            md.appendMarkdown('Condition non déclarée : le loader l\'ignore sans message.');
            return md;
        }
        md.appendCodeblock(def.text, 'text');
        md.appendMarkdown(`Déclarée dans \`${path.relative(pack.rootFolder, def.file).replace(/\\/g, '/')}\` ligne ${def.line}`);
        return md;
    }

    private findParam(tag: SchemaTag, name: string): SchemaParam | undefined {
        const key = name.toLowerCase();
        return tag.params.find(p => p.name.toLowerCase() === key || (p.alias ?? []).some(a => a.toLowerCase() === key));
    }

    private tagMarkdown(tag: SchemaTag): vscode.MarkdownString {
        const md = new vscode.MarkdownString();
        md.appendMarkdown(`**${tag.name}** — statut : \`${tag.status}\`\n\n`);
        if (tag.syntax) md.appendMarkdown(`Syntaxe : \`${tag.syntax}\`\n\n`);
        if (tag.notes) md.appendMarkdown(`${tag.notes}\n\n`);
        const positional = tag.params.filter(p => !p.kv).sort((a, b) => (a.pos ?? 0) - (b.pos ?? 0));
        if (positional.length) {
            md.appendMarkdown('**Paramètres**\n\n');
            for (const p of positional) md.appendMarkdown(`- ${p.pos}. \`${p.name}\`${p.type ? ` (${p.type})` : ''}${p.required ? ' — requis' : ''}${p.desc ? ` : ${p.desc}` : ''}\n`);
        }
        return md;
    }

    private paramMarkdown(tag: SchemaTag, p: SchemaParam): vscode.MarkdownString {
        const md = new vscode.MarkdownString();
        md.appendMarkdown(`**${p.name}** (${tag.name})\n\n`);
        if (p.type) md.appendMarkdown(`Type : \`${p.type}\`\n\n`);
        if (p.default !== undefined) md.appendMarkdown(`Défaut : \`${String(p.default)}\`\n\n`);
        if (p.range) md.appendMarkdown(`Plage : ${p.range}\n\n`);
        if (Array.isArray(p.values)) md.appendMarkdown(`Valeurs : ${p.values.map(v => `\`${v}\``).join(', ')}\n\n`);
        if (p.alias?.length) md.appendMarkdown(`Alias : ${p.alias.map(a => `\`${a}\``).join(', ')}\n\n`);
        if (p.desc) md.appendMarkdown(p.desc);
        return md;
    }
}
