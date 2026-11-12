import * as fs from 'fs';

/** Dimensions d'un PNG lues dans l'en-tête IHDR, sans dépendance. Renvoie undefined si ce n'est pas un PNG lisible. */
export function readPngSize(file: string): { width: number; height: number } | undefined {
    let fd: number | undefined;
    try {
        fd = fs.openSync(file, 'r');
        const buf = Buffer.alloc(24);
        if (fs.readSync(fd, buf, 0, 24, 0) < 24) return undefined;
        if (buf.subarray(0, 8).toString('hex') !== '89504e470d0a1a0a') return undefined;
        if (buf.subarray(12, 16).toString('ascii') !== 'IHDR') return undefined;
        return { width: buf.readUInt32BE(16), height: buf.readUInt32BE(20) };
    } catch {
        return undefined;
    } finally {
        if (fd !== undefined) fs.closeSync(fd);
    }
}
