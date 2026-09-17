import { getJson, postForm } from "./client";

export interface ScannedNetwork {
    ssid: string;
    rssi: number;
    secure: boolean;
}

export interface ScanResult {
    ok: boolean;
    networks: ScannedNetwork[];
    error?: string;
}

export interface WifiInfo {
    mode: string;
    ssid: string;
    ip: string;
    hostname: string;
    scan: ScanResult;
}

export function getWifi(): Promise<WifiInfo> {
    return getJson("/api/wifi");
}

// Triggers a live scan, which takes a few seconds.
export function scanWifiNetworks(): Promise<ScanResult> {
    return getJson("/api/wifi/scan");
}

// The three below all restart the device to apply.
export function postWifi(ssid: string, password: string): Promise<Response> {
    return postForm("/api/wifi", { ssid, password });
}

export function postWifiForget(): Promise<Response> {
    return postForm("/api/wifi/forget");
}

export function postHostname(hostname: string): Promise<Response> {
    return postForm("/api/wifi/hostname", { hostname });
}
