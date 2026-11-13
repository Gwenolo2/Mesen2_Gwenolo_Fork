import * as fs from 'fs';
import * as path from 'path';
import * as vscode from 'vscode';
import { parseLine, resolveRef } from './packParser';
import { packRoot } from './resolver';

/**
 * Rend cliquables : les fichiers référencés qui existent, et les <include>.
 * Les lignes contenant un @alias sont ignorées pour l'instant (le chemin final n'est pas à la position écrite).
 */
export class PackLinkProvider implements vscode.DocumentLinkProvider {
    provideDocumentLinks(doc: vscode.TextDocument): vscode.DocumentLink[] {
        const links: vscode.DocumentLink[] = [];
        const root = packRoot(doc.uri.fsPath);
        for (let i = 0; i < doc.lineCount; i++) {
            const text = doc.lineAt(i).text;
            if (text.includes('@')) continue;

            const inc = /^[ \t]*<include>[ \t]*([^#\r]*?)[ \t]*(#.*)?$/.exec(text);
            if (inc && inc[1]) {
                const start = text.indexOf(inc[1], text.indexOf('<include>') + '<include>'.length);
                const target = path.resolve(root, inc[1].replace(/\\/g, '/'));
                if (fs.existsSync(target)) {
                    const link = new vscode.DocumentLink(
                        new vscode.Range(i, start, i, start + inc[1].length),
                        vscode.Uri.file(target),
                    );
                    link.tooltip = `Ouvrir le fichier inclus ${inc[1]}`;
                    links.push(link);
                }
                continue;
            }

            const parsed = parseLine(text);
            for (const ref of parsed?.refs ?? []) {
                if (ref.kind !== 'file') continue;
                const target = resolveRef(root, ref);
                if (!fs.existsSync(target)) continue;
                const link = new vscode.DocumentLink(
                    new vscode.Range(i, ref.start, i, ref.end),
                    vscode.Uri.file(target),
                );
                link.tooltip = `Ouvrir ${ref.path}`;
                links.push(link);
            }
        }
        return links;
    }
}
