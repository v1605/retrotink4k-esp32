import type { BannerBitmap } from "./osdBanner";

// Renders OSD text buffers (character + attribute byte per cell) with the
// 8x16 1bpp font.

// Attribute byte: bits 6-7 select one of four fixed background colours,
// bits 0-5 are the foreground as RGB222 (2 bits per channel, scaled by 85).
const BACKGROUND_COLORS: [number, number, number][] = [
    [5, 7, 12],
    [233, 237, 243],
    [32, 192, 32],
    [208, 32, 32],
];

const GLYPH_WIDTH = 8;
const GLYPH_HEIGHT = 16;
const GLYPH_COUNT = 256;

// The banner is drawn on plane 1 only, above the menu: its area must be empty
// and something must be shown below it.
function usesBanner(
    text: Uint8Array,
    color: Uint8Array,
    rows: number,
    width: number,
    stride: number,
    plane: 1 | 2,
    banner: BannerBitmap | null,
): boolean {
    if (plane !== 1 || !banner)
        return false;

    const bannerRows = Math.min(rows, Math.ceil(banner.height / GLYPH_HEIGHT));

    for (let row = 0; row < Math.max(1, bannerRows - 1); row++) {
        for (let column = 0; column < width; column++) {
            if (text[row * stride + column] > 0x20)
                return false;
        }
    }

    for (let row = bannerRows; row < rows; row++) {
        for (let column = 0; column < width; column++) {
            const index = row * stride + column;
            if (text[index] > 0x20 || color[index] & 0xc0)
                return true;
        }
    }

    return false;
}

export interface OsdBitmap {
    width: number;
    height: number;
    pixels: Uint8ClampedArray;
    usesBanner: boolean;
}

export function createOsdBitmap(
    text: Uint8Array,
    color: Uint8Array,
    font: Uint8Array,
    rows: number,
    width: number,
    stride: number,
    plane: 1 | 2,
    banner: BannerBitmap | null,
): OsdBitmap {
    // Drawn uncropped, so a small box keeps the full plane's scale.
    const withBanner = usesBanner(text, color, rows, width, stride, plane, banner);
    const pixelWidth = width * GLYPH_WIDTH;
    const pixelHeight = rows * GLYPH_HEIGHT;
    const pixels = new Uint8ClampedArray(pixelWidth * pixelHeight * 4);

    for (let row = 0; row < rows; row++) {
        for (let column = 0; column < width; column++) {
            const cellIndex = row * stride + column;
            const glyph = text[cellIndex] ?? 0;
            const attr = color[cellIndex] ?? 0;

            const foreground: [number, number, number] = [
                ((attr >> 4) & 3) * 85,
                ((attr >> 2) & 3) * 85,
                (attr & 3) * 85,
            ];
            const backgroundMode = (attr >> 6) & 3;
            const background = BACKGROUND_COLORS[backgroundMode];

            for (let glyphRow = 0; glyphRow < GLYPH_HEIGHT; glyphRow++) {
                // Row-major across all glyphs, not per-glyph-contiguous.
                const glyphBits = font[glyphRow * GLYPH_COUNT + glyph] ?? 0;

                for (let glyphCol = 0; glyphCol < GLYPH_WIDTH; glyphCol++) {
                    const x = column * GLYPH_WIDTH + glyphCol;
                    const y = row * GLYPH_HEIGHT + glyphRow;
                    const pixelIndex = (y * pixelWidth + x) * 4;

                    if ((glyphBits >> glyphCol) & 1) {
                        pixels[pixelIndex] = foreground[0];
                        pixels[pixelIndex + 1] = foreground[1];
                        pixels[pixelIndex + 2] = foreground[2];
                    } else if (
                        backgroundMode === 0
                        && withBanner
                        && banner
                        && x < banner.width
                        && y < banner.height
                        && banner.pixels[(y * banner.width + x) * 4 + 3]
                    ) {
                        const bannerIndex = (y * banner.width + x) * 4;
                        pixels[pixelIndex] = banner.pixels[bannerIndex];
                        pixels[pixelIndex + 1] = banner.pixels[bannerIndex + 1];
                        pixels[pixelIndex + 2] = banner.pixels[bannerIndex + 2];
                    } else {
                        pixels[pixelIndex] = background[0];
                        pixels[pixelIndex + 1] = background[1];
                        pixels[pixelIndex + 2] = background[2];
                    }

                    pixels[pixelIndex + 3] = 255;
                }
            }
        }
    }

    return { width: pixelWidth, height: pixelHeight, pixels, usesBanner: withBanner };
}

export function drawOsdBitmap(canvas: HTMLCanvasElement, bitmap: OsdBitmap): void {
    canvas.width = bitmap.width;
    canvas.height = bitmap.height;

    const ctx = canvas.getContext("2d");
    if (!ctx)
        return;

    const image = ctx.createImageData(bitmap.width, bitmap.height);
    image.data.set(bitmap.pixels);
    ctx.putImageData(image, 0, 0);
}
