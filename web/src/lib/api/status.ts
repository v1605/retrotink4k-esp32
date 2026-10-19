import { getJson, postFormJson } from "./client";
import type { OkResult } from "./client";

export interface StatusInfo {
    ftdi_connected: boolean;
    baud: number;
    wifi_ssid: string;
    wifi_ip: string;
    ota_supported: boolean;
    retro_bridge: {
        clients: number;
        owner: string; // e.g. "lease:1/7", "internal:osd", or ""
    };
}

export function getStatus(): Promise<StatusInfo> {
    return getJson("/api/status");
}

export function postSerialBaud(baud: number | string): Promise<OkResult> {
    return postFormJson("/api/serial", { baud: String(baud) });
}
