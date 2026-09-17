// Minimal ZIP reader: stored and deflated entries via DecompressionStream.
// No ZIP64 or encryption.

export interface ZipEntry {
    name: string;     // full path inside the archive
    baseName: string; // last path segment
    flags: number;
    method: number;   // 0 = stored, 8 = deflate
    crc32: number;
    compressedSize: number;
    size: number;
    localHeaderOffset: number;
}

const EOCD_SIGNATURE = 0x06054b50;
const CENTRAL_SIGNATURE = 0x02014b50;
const LOCAL_SIGNATURE = 0x04034b50;

async function readView(file: Blob, start: number, end: number): Promise<DataView> {
    return new DataView(await file.slice(start, end).arrayBuffer());
}

export async function listZip(file: Blob): Promise<ZipEntry[]> {
    // End-of-central-directory record: last 22 bytes plus up to 64KB comment.
    const tailStart = Math.max(0, file.size - (22 + 0xffff));
    const tail = await readView(file, tailStart, file.size);

    let eocd = -1;
    for (let i = tail.byteLength - 22; i >= 0; i--) {
        if (tail.getUint32(i, true) === EOCD_SIGNATURE) {
            eocd = i;
            break;
        }
    }
    if (eocd < 0)
        throw new Error("That file isn't a ZIP archive.");

    const count = tail.getUint16(eocd + 10, true);
    const dirSize = tail.getUint32(eocd + 12, true);
    const dirOffset = tail.getUint32(eocd + 16, true);
    if (count === 0xffff || dirOffset === 0xffffffff)
        throw new Error("ZIP64 archives aren't supported.");

    const dir = await readView(file, dirOffset, dirOffset + dirSize);
    const decoder = new TextDecoder();
    const entries: ZipEntry[] = [];

    let p = 0;
    for (let i = 0; i < count; i++) {
        if (dir.getUint32(p, true) !== CENTRAL_SIGNATURE)
            throw new Error("The ZIP's directory is damaged.");

        const nameLength = dir.getUint16(p + 28, true);
        const extraLength = dir.getUint16(p + 30, true);
        const commentLength = dir.getUint16(p + 32, true);
        const name = decoder.decode(new Uint8Array(dir.buffer, dir.byteOffset + p + 46, nameLength));

        entries.push({
            name,
            baseName: name.slice(name.lastIndexOf("/") + 1),
            flags: dir.getUint16(p + 8, true),
            method: dir.getUint16(p + 10, true),
            crc32: dir.getUint32(p + 16, true),
            compressedSize: dir.getUint32(p + 20, true),
            size: dir.getUint32(p + 24, true),
            localHeaderOffset: dir.getUint32(p + 42, true),
        });

        p += 46 + nameLength + extraLength + commentLength;
    }

    return entries;
}

const CRC_TABLE = (() => {
    const table = new Uint32Array(256);
    for (let n = 0; n < 256; n++) {
        let c = n;
        for (let k = 0; k < 8; k++)
            c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
        table[n] = c >>> 0;
    }
    return table;
})();

function crc32(data: Uint8Array): number {
    let crc = 0xffffffff;
    for (let i = 0; i < data.length; i++)
        crc = CRC_TABLE[(crc ^ data[i]) & 0xff] ^ (crc >>> 8);
    return (crc ^ 0xffffffff) >>> 0;
}

// Throws unless the size and CRC-32 match the archive.
export async function extractZipEntry(file: Blob, entry: ZipEntry): Promise<Uint8Array<ArrayBuffer>> {
    if (entry.flags & 1)
        throw new Error(entry.name + " is encrypted.");

    const header = await readView(file, entry.localHeaderOffset, entry.localHeaderOffset + 30);
    if (header.getUint32(0, true) !== LOCAL_SIGNATURE)
        throw new Error("The ZIP entry for " + entry.name + " is damaged.");

    const dataStart = entry.localHeaderOffset + 30 + header.getUint16(26, true) + header.getUint16(28, true);
    const compressed = file.slice(dataStart, dataStart + entry.compressedSize);

    let data: Uint8Array<ArrayBuffer>;
    if (entry.method === 0) {
        data = new Uint8Array(await compressed.arrayBuffer());
    } else if (entry.method === 8) {
        const stream = compressed.stream().pipeThrough(new DecompressionStream("deflate-raw"));
        data = new Uint8Array(await new Response(stream).arrayBuffer());
    } else {
        throw new Error(entry.name + " uses an unsupported compression method (" + entry.method + ").");
    }

    if (data.length !== entry.size || crc32(data) !== entry.crc32)
        throw new Error(entry.name + " failed its integrity check -- the ZIP may be damaged.");

    return data;
}
