import * as fs from 'fs';
import * as vscode from 'vscode';
import { parseLine, resolveRef } from './packParser';

/**
 * Bouton « Ouvrir le dossier » au-dessus des lignes qui utilisent randomize.
 * Les fichiers ne sont plus signalés par un bouton : le lien cliquable (Ctrl+clic) suffit.
 */
export class PackCodeLensProvider implements vscode.CodeLensProvider {
    provideCodeLenses(doc: vscode.TextDocument): vscode.CodeLens[] {
        const lenses: vscode.CodeLens[] = [];
        for (let i = 0; i < doc.lineCount; i++) {
            const text = doc.lineAt(i).text;
            if (text.includes('@')) continue;
            const parsed = parseLine(text);
            for (const ref of parsed?.refs ?? []) {
                if (ref.kind !== 'folder') continue;
                const target = resolveRef(doc.uri.fsPath, ref);
                if (!fs.existsSync(target)) continue;
                lenses.push(new vscode.CodeLens(new vscode.Range(i, ref.start, i, ref.end), {
                    title: 'Ouvrir le dossier (randomize)',
                    command: 'revealFileInOS',
                    arguments: [vscode.Uri.file(target)],
                }));
            }
        }
        return lenses;
    }
}
