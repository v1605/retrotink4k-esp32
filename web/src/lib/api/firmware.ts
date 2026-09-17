import { getJson, postFormJson, withQuery } from "./client";
import type { OkResult } from "./client";

export interface FirmwareDeviceInfo {
    ok: boolean;
    modelId?: number;
    model?: string;   // e.g. "RT4K_Pro"
    version?: string; // running firmware, e.g. "1.80.1"
    error?: string;
}

export interface FirmwareCheckInfo {
    ok: boolean;
    version?: string;
    token?: string;
    error?: string;
}

// Pushed over /ws/firmware while the bridge works on an upload or install.
export interface FirmwareStatusEvent {
    type: "progress" | "verifying" | "uploaded" | "checking" | "checked" | "installing" | "flashing" | "error";
    file?: string;
    sent?: number;  // progress: bytes written to the RT4K so far
    total?: number;
    version?: string;
    message?: string;
}

export function getFirmwareDevice(): Promise<FirmwareDeviceInfo> {
    return getJson("/api/firmware/device");
}

// Validates rt4kup.bin on the SD card, which decides the installed version.
export function checkFirmware(): Promise<FirmwareCheckInfo> {
    return getJson("/api/firmware/check");
}

// Flashes what checkFirmware() validated; the RT4K reboots to do it.
export function installFirmware(token: string): Promise<OkResult> {
    return postFormJson("/api/firmware/install", { token });
}

// Writes a file to the SD card root. onProgress covers the HTTP upload; the
// RT4K write is reported by openFirmwareStatusSocket(). XHR for upload progress.
export function uploadFirmwareFile(name: string, data: Blob, onProgress?: (fraction: number) => void): Promise<OkResult> {
    return new Promise((resolve, reject) => {
        const xhr = new XMLHttpRequest();
        xhr.open("POST", withQuery("/api/firmware/upload", { name }));
        xhr.setRequestHeader("Content-Type", "application/octet-stream");

        xhr.upload.onprogress = (event) => {
            if (event.lengthComputable && onProgress)
                onProgress(event.loaded / event.total);
        };

        xhr.onload = () => {
            try {
                resolve(JSON.parse(xhr.responseText));
            } catch {
                resolve({ ok: false, error: xhr.responseText || "Upload failed (" + xhr.status + ")" });
            }
        };

        xhr.onerror = () => reject(new Error("Lost connection to the device during the upload."));

        xhr.send(data);
    });
}

// Status events; reconnects until the returned function is called.
export function openFirmwareStatusSocket(onEvent: (event: FirmwareStatusEvent) => void): () => void {
    let socket: WebSocket | null = null;
    let closed = false;

    const open = () => {
        const protocol = location.protocol === "https:" ? "wss://" : "ws://";
        const ws = new WebSocket(protocol + location.host + "/ws/firmware");
        socket = ws;

        ws.onmessage = (message: MessageEvent) => {
            if (typeof message.data !== "string")
                return;
            try {
                onEvent(JSON.parse(message.data));
            } catch {}
        };

        ws.onclose = () => {
            socket = null;
            if (!closed)
                setTimeout(open, 1000);
        };

        ws.onerror = () => ws.close();
    };

    open();

    return () => {
        closed = true;
        socket?.close();
    };
}
