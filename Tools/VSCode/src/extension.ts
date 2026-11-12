import * as vscode from 'vscode';
import { PackCodeLensProvider } from './codelens';
import { PackCompletionProvider } from './completion';
import { computeDiagnostics } from './diagnostics';
import { PackHoverProvider } from './hover';
import { PackLinkProvider } from './links';
import { declaredConditions, resolvePack } from './resolver';
import { PackSignatureProvider } from './signature';
import { loadSchema } from './schema';

export type HdConsole = 'snes' | 'nes';

/** Console déduite du nom du fichier (doc 29, §2) : snes_hires.txt = SNES, hires.txt = NES. */
export function consoleForFileName(fileName: string): HdConsole | undefined {
    const base = (fileName.split(/[\\/]/).pop() ?? '').toLowerCase();
    if (base === 'snes_hires.txt') return 'snes';
    if (base === 'hires.txt') return 'nes';
    return undefined;
}

export function activate(context: vscode.ExtensionContext): void {
    const output = vscode.window.createOutputChannel('Mesen HD Pack');
    context.subscriptions.push(output);

    const schema = loadSchema(context.extensionPath);
    const selector: vscode.DocumentSelector = { language: 'mesen-snes-hd', scheme: 'file' };
    context.subscriptions.push(
        vscode.languages.registerDocumentLinkProvider(selector, new PackLinkProvider()),
        vscode.languages.registerCodeLensProvider(selector, new PackCodeLensProvider()),
        vscode.languages.registerCompletionItemProvider(selector, new PackCompletionProvider(schema), '<', '[', '&', ',', '='),
        vscode.languages.registerHoverProvider(selector, new PackHoverProvider(schema)),
        vscode.languages.registerSignatureHelpProvider(selector, new PackSignatureProvider(schema), ','),
        vscode.commands.registerCommand('mesenHd.listConditions', () => {
            const doc = vscode.window.activeTextEditor?.document;
            if (!doc || doc.uri.scheme !== 'file') {
                vscode.window.showWarningMessage('Ouvrez un snes_hires.txt.');
                return;
            }
            const pack = resolvePack(doc.uri.fsPath, doc.getText());
            const conditions = declaredConditions(pack);
            output.show(true);
            output.appendLine(`--- Pack : ${pack.rootFolder}`);
            output.appendLine(`Lignes utiles : ${pack.lines.length}, alias : ${pack.aliases.size}, conditions : ${conditions.length}`);
            const files = [...new Set(pack.lines.map(l => l.file))];
            output.appendLine(`Fichiers lus : ${files.length}`);
            for (const f of files) output.appendLine(`  ${f}`);
            for (const c of conditions) output.appendLine(`  condition ${c.name}  (${c.file}:${c.line})`);
            for (const e of pack.errors) output.appendLine(`ERREUR : ${e}`);
        }),
    );

    // Diagnostics : mis à jour à l'ouverture, à la modification, et à chaque sauvegarde (les includes changent aussi).
    const diagnostics = vscode.languages.createDiagnosticCollection('mesen-hd-snes');
    context.subscriptions.push(diagnostics);
    const refresh = (doc: vscode.TextDocument): void => {
        if (doc.languageId !== 'mesen-snes-hd' || doc.uri.scheme !== 'file') return;
        diagnostics.set(doc.uri, computeDiagnostics(doc, schema));
    };
    vscode.workspace.textDocuments.forEach(refresh);
    context.subscriptions.push(
        vscode.workspace.onDidOpenTextDocument(refresh),
        vscode.workspace.onDidChangeTextDocument(e => refresh(e.document)),
        vscode.workspace.onDidSaveTextDocument(() => vscode.workspace.textDocuments.forEach(refresh)),
        vscode.workspace.onDidCloseTextDocument(doc => diagnostics.delete(doc.uri)),
    );

    output.appendLine('Mesen HD Pack (SNES) activé. Étape 5 : diagnostics.');
}

export function deactivate(): void {
    // Rien à libérer pour l'instant.
}
