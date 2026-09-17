import { errorMessage, getJson, postFormJson, readJson, withQuery } from "./client";
import type { OkResult } from "./client";

// Names are relative to /profile on the device's SD card.

export interface ProfileEntry {
    name: string;
    isDirectory: boolean;
    size: number;
}

export interface ProfileListInfo {
    ok: boolean;
    entries: ProfileEntry[];
    error?: string;
}

export interface CurrentProfileInfo {
    ok: boolean;
    loaded: boolean;
    path?: string;
    error?: string;
}

export function getCurrentProfile(): Promise<CurrentProfileInfo> {
    return getJson("/api/profiles/current");
}

export function listProfiles(path: string): Promise<ProfileListInfo> {
    return getJson("/api/profiles", { path });
}

// Applies the profile to the device's live settings immediately.
export function loadProfile(name: string): Promise<OkResult> {
    return postFormJson("/api/profiles/load", { name });
}

export function deleteProfile(name: string): Promise<OkResult> {
    return postFormJson("/api/profiles/delete", { name });
}

export async function downloadProfile(name: string): Promise<Blob> {
    const res = await fetch(withQuery("/api/profiles/download", { name }));

    if (!res.ok)
        throw new Error(await errorMessage(res, "Download failed"));

    return res.blob();
}

export async function uploadProfile(name: string, file: File): Promise<OkResult> {
    const res = await fetch(withQuery("/api/profiles/upload", { name }), {
        method: "POST",
        headers: { "Content-Type": "application/octet-stream" },
        body: file,
    });

    return readJson(res);
}
