import * as path from 'path';
import * as vscode from 'vscode';

/**
 * Textes des fichiers de pack ouverts dans l'éditeur, modifications non sauvegardées incluses,
 * indexés par chemin absolu. Permet d'analyser un <include> tel qu'il est édité.
 */
export function openPackTexts(): Map<string, string> {
    const texts = new Map<string, string>();
    for (const d of vscode.workspace.textDocuments) {
        if (d.languageId === 'mesen-snes-hd' && d.uri.scheme === 'file') {
            texts.set(path.resolve(d.uri.fsPath), d.getText());
        }
    }
    return texts;
}
