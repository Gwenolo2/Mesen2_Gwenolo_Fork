import * as vscode from 'vscode';
import { Schema, tagBefore } from './schema';

/** Aide à la signature : syntaxe de la balise, paramètre positionnel courant en gras. */
export class PackSignatureProvider implements vscode.SignatureHelpProvider {
    constructor(private readonly schema: Schema) {}

    provideSignatureHelp(doc: vscode.TextDocument, pos: vscode.Position): vscode.SignatureHelp | undefined {
        const text = doc.lineAt(pos.line).text;
        const ctx = tagBefore(text, pos.character, this.schema);
        if (!ctx) return undefined;

        const positional = ctx.tag.params.filter(p => !p.kv).sort((a, b) => (a.pos ?? 0) - (b.pos ?? 0));
        if (positional.length === 0) return undefined;

        const label = ctx.tag.syntax ?? ctx.tag.name;
        const info = new vscode.SignatureInformation(label, ctx.tag.notes);
        info.parameters = positional.map(p => new vscode.ParameterInformation(p.name, p.desc ?? p.type));

        const help = new vscode.SignatureHelp();
        help.signatures = [info];
        help.activeSignature = 0;
        help.activeParameter = Math.min(ctx.argIndex, positional.length - 1);
        return help;
    }
}
