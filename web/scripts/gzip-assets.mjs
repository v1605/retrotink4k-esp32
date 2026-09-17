// Gzips text assets in ../data; ESPAsyncWebServer serves the .gz files directly.
import { gzipSync } from "node:zlib";
import { readFileSync, writeFileSync, readdirSync, statSync, unlinkSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join, extname } from "node:path";

const scriptDir = dirname(fileURLToPath(import.meta.url));
const dataDir = join(scriptDir, "..", "..", "data");
const COMPRESSIBLE_EXTENSIONS = new Set([".html", ".css", ".js", ".svg"]);

function collectCompressibleFiles(dir) {
    const files = [];

    for (const entry of readdirSync(dir)) {
        const fullPath = join(dir, entry);

        if (statSync(fullPath).isDirectory()) {
            files.push(...collectCompressibleFiles(fullPath));
        } else if (COMPRESSIBLE_EXTENSIONS.has(extname(entry))) {
            files.push(fullPath);
        }
    }

    return files;
}

for (const filePath of collectCompressibleFiles(dataDir)) {
    const original = readFileSync(filePath);
    const compressed = gzipSync(original, { level: 9 });
    writeFileSync(`${filePath}.gz`, compressed);
    unlinkSync(filePath);

    console.log(`Gzipped ${filePath}: ${original.length} -> ${compressed.length} bytes`);
}
