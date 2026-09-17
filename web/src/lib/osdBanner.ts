// Decodes the banner BMP: 24/32bpp using 4-4-3 bits per channel, with magenta
// (255,0,255) as the colorkey.

const BANNER_DISPLAY_WIDTH = 320;
const BANNER_DISPLAY_HEIGHT = 128;

export interface BannerBitmap {
    width: number;
    height: number;
    pixels: Uint8ClampedArray;
}

export function decodeBannerBmp(bytes: Uint8Array): BannerBitmap {
    if (bytes.length < 54)
        throw new Error("Banner is not a valid BMP");

    const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    if (view.getUint16(0, true) !== 0x4d42)
        throw new Error("Banner is not a BMP");

    const pixelOffset = view.getUint32(10, true);
    const width = view.getInt32(18, true);
    const height = view.getInt32(22, true);
    const bitsPerPixel = view.getUint16(28, true);
    const compression = view.getUint32(30, true);

    if (width < 1 || height === 0)
        throw new Error("Banner dimensions are invalid");

    if (bitsPerPixel !== 24 && bitsPerPixel !== 32)
        throw new Error(`Unsupported ${bitsPerPixel}-bit banner BMP`);

    if (compression !== 0 && !(compression === 3 && bitsPerPixel === 32))
        throw new Error("Compressed banner BMPs are unsupported");

    const topDown = height < 0;
    const sourceHeight = Math.abs(height);
    const bytesPerPixel = bitsPerPixel / 8;
    const rowStride = Math.ceil((width * bytesPerPixel) / 4) * 4;
    const requiredLength = pixelOffset + rowStride * sourceHeight;

    if (
        pixelOffset < 54
        || !Number.isSafeInteger(rowStride)
        || !Number.isSafeInteger(requiredLength)
        || requiredLength > bytes.length
    ) {
        throw new Error("Banner BMP pixel data is truncated");
    }

    const displayWidth = Math.min(width, BANNER_DISPLAY_WIDTH);
    const displayHeight = Math.min(sourceHeight, BANNER_DISPLAY_HEIGHT);
    const pixels = new Uint8ClampedArray(displayWidth * displayHeight * 4);

    for (let y = 0; y < displayHeight; y++) {
        let sourceIndex = pixelOffset + (topDown ? y : sourceHeight - 1 - y) * rowStride;

        for (let x = 0; x < displayWidth; x++) {
            const blue = bytes[sourceIndex];
            const green = bytes[sourceIndex + 1];
            const red = bytes[sourceIndex + 2];
            sourceIndex += bytesPerPixel;

            if (red === 255 && green === 0 && blue === 255)
                continue; // magenta colorkey -> transparent

            const pixelIndex = (y * displayWidth + x) * 4;
            pixels[pixelIndex] = Math.floor(((red >> 4) * 255) / 15);
            pixels[pixelIndex + 1] = Math.floor(((green >> 4) * 255) / 15);
            pixels[pixelIndex + 2] = Math.floor(((blue >> 5) * 255) / 7);
            pixels[pixelIndex + 3] = 255;
        }
    }

    return { width: displayWidth, height: displayHeight, pixels };
}
