import { getOsdBanner, getOsdFont, getOsdPlane, onRemoteDone, postRemoteButton } from "./api";
import type { OsdPlaneInfo } from "./api";
import { base64ToBytes } from "./base64";
import { decodeBannerBmp } from "./osdBanner";
import type { BannerBitmap } from "./osdBanner";
import { createOsdBitmap, drawOsdBitmap } from "./osdBitmap";

// After a press, the update starts on the bridge's "remote" event (or this
// fallback) and reads the plane until it changes, then until two reads match.
const REMOTE_DONE_FALLBACK_MS = 1500;
const CHANGE_POLL_MS = 100;
const SETTLE_CHECK_MS = 150;
const CHANGE_TIMEOUT_MS = 1500;
const SETTLE_TIMEOUT_MS = 3000;

function snapshotKey(info: OsdPlaneInfo): string {
    return info.ok && info.available ? `${info.text}${info.color}` : `${info.ok}:${info.available}`;
}

function sleep(ms: number): Promise<void> {
    return new Promise((resolve) => setTimeout(resolve, ms));
}

class OsdStore {
    plane = $state<1 | 2>(1);
    loading = $state(false);
    available = $state(false);
    usesBanner = $state(false);
    message = $state("");
    on = $state(0);
    osk = $state(0);

    private font: Uint8Array | null = null;
    private canvas: HTMLCanvasElement | null = null;
    private cachedBannerPath: string | null = null;
    private cachedBanner: BannerBitmap | null = null;
    private remoteFallbackTimer: ReturnType<typeof setTimeout> | null = null;
    // Key of the last rendered plane.
    private shownSnapshot = "";
    // Bumped per update, so a superseded one can't overwrite a newer one.
    private renderGeneration = 0;
    // One banner fetch at a time; /ws/osd tracks one per client.
    private bannerFetchPromise: Promise<BannerBitmap | null> | null = null;

    constructor() {
        onRemoteDone(() => this.followRemote());
    }

    setCanvas(canvas: HTMLCanvasElement | null): void {
        this.canvas = canvas;
    }

    setPlane(plane: 1 | 2): void {
        this.plane = plane;
    }

    sendRemoteButton(button: string): void {
        postRemoteButton(button).catch((error) => console.error("Failed to send remote button", error));

        this.clearRemoteFallback();
        this.remoteFallbackTimer = setTimeout(() => this.followRemote(), REMOTE_DONE_FALLBACK_MS);
    }

    refresh(): Promise<void> {
        return this.update(false);
    }

    private followRemote(): void {
        this.clearRemoteFallback();
        if (this.canvas)
            void this.update(true);
    }

    private clearRemoteFallback(): void {
        if (this.remoteFallbackTimer) {
            clearTimeout(this.remoteFallbackTimer);
            this.remoteFallbackTimer = null;
        }
    }

    // Fetched once per session; the font never changes at runtime.
    private async ensureFont(): Promise<Uint8Array> {
        if (this.font)
            return this.font;

        const font = await getOsdFont();
        this.font = font;
        return font;
    }

    // Banner overlay only applies to plane 1.
    private fetchBannerDeduped(): Promise<BannerBitmap | null> {
        if (this.plane !== 1)
            return Promise.resolve(null);

        if (!this.bannerFetchPromise) {
            this.bannerFetchPromise = this.fetchBanner().finally(() => {
                this.bannerFetchPromise = null;
            });
        }

        return this.bannerFetchPromise;
    }

    private async fetchBanner(): Promise<BannerBitmap | null> {
        if (this.plane !== 1)
            return null;

        try {
            const banner = await getOsdBanner(this.cachedBannerPath ?? undefined);

            if (!banner.available) {
                this.cachedBannerPath = null;
                this.cachedBanner = null;
                return null;
            }

            if (banner.unchanged)
                return this.cachedBanner;

            this.cachedBannerPath = banner.path;
            this.cachedBanner = decodeBannerBmp(banner.bytes);
            return this.cachedBanner;
        } catch (error) {
            console.error("Failed to decode OSD banner", error);
            this.cachedBannerPath = null;
            this.cachedBanner = null;
            return null;
        }
    }

    private render(font: Uint8Array, info: OsdPlaneInfo, banner: BannerBitmap | null): void {
        this.shownSnapshot = snapshotKey(info);

        if (!info.ok) {
            this.available = false;
            this.message = info.error || "Failed to fetch the OSD.";
            return;
        }

        this.available = info.available;

        if (!info.available || !info.text || !info.color || !info.rows || !info.width) {
            this.usesBanner = false;
            return;
        }

        this.on = info.on ?? 0;
        this.osk = info.osk ?? 0;

        const bitmap = createOsdBitmap(
            base64ToBytes(info.text),
            base64ToBytes(info.color),
            font,
            info.rows,
            info.width,
            info.stride || info.width,
            this.plane,
            banner,
            true,
        );

        this.usesBanner = bitmap.usesBanner;

        if (this.canvas)
            drawOsdBitmap(this.canvas, bitmap);
    }

    private async update(waitForChange: boolean): Promise<void> {
        // No re-entrancy guard -- see renderGeneration.
        const generation = ++this.renderGeneration;
        this.loading = true;
        this.message = "";

        try {
            const font = await this.ensureFont();
            const info = await this.readPlane(font, generation, waitForChange);
            if (!info)
                return;

            // Rendered with the cached banner so far; recheck it now.
            if (this.plane === 1) {
                const banner = await this.fetchBannerDeduped();
                if (generation !== this.renderGeneration)
                    return;
                this.render(font, info, banner);
            }
        } catch (error) {
            if (generation !== this.renderGeneration)
                return;
            this.available = false;
            this.message = error instanceof Error ? error.message : "Failed to fetch the OSD.";
        }

        this.loading = false;
    }

    // Renders each new read. With waitForChange, keeps reading until the plane
    // differs from what was shown and then two reads match. Null if superseded.
    private async readPlane(font: Uint8Array, generation: number, waitForChange: boolean): Promise<OsdPlaneInfo | null> {
        const start = Date.now();
        let changed = false;

        for (;;) {
            const info = await getOsdPlane(this.plane);
            if (generation !== this.renderGeneration)
                return null;

            const elapsed = Date.now() - start;
            const isNew = snapshotKey(info) !== this.shownSnapshot;

            if (isNew || !waitForChange)
                this.render(font, info, this.plane === 1 ? this.cachedBanner : null);

            if (!waitForChange || elapsed >= SETTLE_TIMEOUT_MS)
                return info;

            if (isNew)
                changed = true;
            else if (changed || elapsed >= CHANGE_TIMEOUT_MS)
                return info;

            await sleep(changed ? SETTLE_CHECK_MS : CHANGE_POLL_MS);
            if (generation !== this.renderGeneration)
                return null;
        }
    }
}

export const osdStore = new OsdStore();
