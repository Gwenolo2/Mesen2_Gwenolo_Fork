import * as vscode from 'vscode';
import { PackCodeLensProvider } from './codelens';
import { PackCompletionProvider } from './completion';
import { ConsoleProfile, HdConsole, PROFILES, profileForDocument } from './consoles';
import { computeDiagnostics } from './diagnostics';
import { PackHoverProvider } from './hover';
import { PackLinkProvider } from './links';
import { openPackTexts } from './openDocs';
import { declaredConditions, resolvePack } from './resolver';
import { PackSignatureProvider } from './signature';
import { Schema, loadSchema } from './schema';

/** Console déduite du nom du fichier : snes_hires.txt = SNES, hires.txt = NES, gb_hires.txt = Game Boy. */
export function consoleForFileName(fileName: string): HdConsole | undefined {
    const base = (fileName.split(/[\\/]/).pop() ?? '').toLowerCase();
    if (base === 'snes_hires.txt') return 'snes';
    if (base === 'hires.txt') return 'nes';
    if (base === 'gb_hires.txt') return 'gb';
    return undefined;
}

export function activate(context: vscode.ExtensionContext): void {
    const output = vscode.window.createOutputChannel('Mesen HD Pack');
    context.subscriptions.push(output);

    // Un jeu de fournisseurs par console : chacun a son schéma, ses champs fichiers et ses conditions intégrées.
    const schemas = new Map<string, Schema>();
    for (const profile of PROFILES) {
        const schema = loadSchema(context.extensionPath, profile.schemaFile);
        schemas.set(profile.id, schema);
        // Sélecteur par nom de fichier : le fournisseur suit le fichier, pas la langue VS Code (qui peut être mal détectée).
        const selector: vscode.DocumentSelector = { scheme: 'file', pattern: `**/${profile.entryName}` };
        context.subscriptions.push(
            vscode.languages.registerDocumentLinkProvider(selector, new PackLinkProvider(profile)),
            vscode.languages.registerCodeLensProvider(selector, new PackCodeLensProvider(profile)),
            vscode.languages.registerCompletionItemProvider(selector, new PackCompletionProvider(schema, profile), '<', '[', '&', ',', '=', '/', '\\'),
            vscode.languages.registerHoverProvider(selector, new PackHoverProvider(schema, profile)),
            vscode.languages.registerSignatureHelpProvider(selector, new PackSignatureProvider(schema), ','),
        );
    }

    context.subscriptions.push(
        vscode.commands.registerCommand('mesenHd.toggleComment', () => vscode.commands.executeCommand('editor.action.commentLine')),
        vscode.commands.registerCommand('mesenHd.listConditions', () => {
            const doc = vscode.window.activeTextEditor?.document;
            if (!doc || doc.uri.scheme !== 'file' || !profileForDocument(doc)) {
                vscode.window.showWarningMessage('Ouvrez un snes_hires.txt, un hires.txt ou un gb_hires.txt.');
                return;
            }
            const pack = resolvePack(doc.uri.fsPath, openPackTexts());
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
    const diagnostics = vscode.languages.createDiagnosticCollection('mesen-hd');
    context.subscriptions.push(diagnostics);
    const refresh = (doc: vscode.TextDocument): void => {
        const profile: ConsoleProfile | undefined = profileForDocument(doc);
        if (!profile || doc.uri.scheme !== 'file') return;
        const schema = schemas.get(profile.id);
        if (!schema) return;
        diagnostics.set(doc.uri, computeDiagnostics(doc, schema, profile));
    };
    vscode.workspace.textDocuments.forEach(refresh);
    context.subscriptions.push(
        vscode.workspace.onDidOpenTextDocument(refresh),
        vscode.workspace.onDidChangeTextDocument(e => refresh(e.document)),
        vscode.workspace.onDidSaveTextDocument(() => vscode.workspace.textDocuments.forEach(refresh)),
        vscode.workspace.onDidCloseTextDocument(doc => diagnostics.delete(doc.uri)),
    );

    output.appendLine('Mesen HD Pack (SNES, NES et Game Boy) activé.');
}

export function deactivate(): void {
    // Rien à libérer pour l'instant.
}
