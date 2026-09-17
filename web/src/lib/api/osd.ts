import { base64ToBytes } from "../base64";
import { onOsdEvent, osdRequest } from "../osdSocket";

// OSD data comes over the /ws/osd WebSocket (see osdSocket.ts).

export interface OsdPlaneInfo {
    ok: boolean;
    available: boolean;
    on?: number;
    osk?: number;
    rows?: number;
    width?: number;
    stride?: number;
    cells?: number;
    text?: string;  // base64, one character code per cell
    color?: string; // base64, same length, one attribute byte per cell
    error?: string;
}

export type OsdBannerResult =
    | { available: false }
    | { available: true; unchanged: true }
    | { available: true; unchanged: false; path: string; bytes: Uint8Array };

export async function getOsdFont(): Promise<Uint8Array> {
    const res = await osdRequest<{ ok: boolean; data?: string; error?: string }>("font");

    if (!res.ok || !res.data)
        throw new Error(res.error || "Font download failed");

    return base64ToBytes(res.data);
}

export function getOsdPlane(plane: 1 | 2): Promise<OsdPlaneInfo> {
    return osdRequest<OsdPlaneInfo>("plane", { plane });
}

// Fires when the bridge has sent every queued remote press.
export function onRemoteDone(handler: () => void): () => void {
    return onOsdEvent("remote", handler);
}

// Pass the path of the banner already held to skip re-sending it.
export async function getOsdBanner(knownPath?: string): Promise<OsdBannerResult> {
    const res = await osdRequest<{
        ok: boolean;
        available: boolean;
        unchanged?: boolean;
        path?: string;
        bytes?: ArrayBuffer;
        error?: string;
    }>("banner", knownPath ? { knownPath } : {});

    if (!res.ok)
        throw new Error(res.error || "Failed to fetch the OSD banner.");

    if (!res.available)
        return { available: false };

    if (res.unchanged)
        return { available: true, unchanged: true };

    return {
        available: true,
        unchanged: false,
        path: res.path || "",
        bytes: new Uint8Array(res.bytes ?? new ArrayBuffer(0)),
    };
}
