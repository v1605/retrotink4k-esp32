import {
    deleteProfile,
    downloadProfile,
    getCurrentProfile,
    listProfiles,
    loadProfile,
    uploadProfile,
} from "./api";
import type { ProfileEntry } from "./api";

const ROOT = "profile";

// Current folder from the URL hash ("#profiles/foo/bar"), read at load.
function pathFromHash(): string {
    const parts = location.hash.replace(/^#\/?/, "").split("/");
    if (parts[0] !== "profiles" || parts.length < 2)
        return ROOT;

    const subpath = parts.slice(1).map(decodeURIComponent).join("/");
    return subpath ? ROOT + "/" + subpath : ROOT;
}

function hashForPath(path: string): string {
    if (path === ROOT)
        return "#profiles";

    const subpath = path.slice(ROOT.length + 1)
        .split("/")
        .map(encodeURIComponent)
        .join("/");
    return "#profiles/" + subpath;
}

class ProfilesStore {
    currentProfile = $state("--");
    currentPath = $state(pathFromHash());
    entries = $state<ProfileEntry[]>([]);
    message = $state("");
    uploadFile = $state<File | null>(null);
    uploading = $state(false);
    loadingName = $state<string | null>(null);
    downloadingName = $state<string | null>(null);
    deletingName = $state<string | null>(null);
    navigating = $state(false);

    private requestId = 0;

    relativeName(entryName: string): string {
        if (this.currentPath === ROOT)
            return entryName;
        return this.currentPath.slice(ROOT.length + 1) + "/" + entryName;
    }

    get breadcrumb() {
        const segments = this.currentPath.split("/");
        let pathSoFar = "";
        return segments.map((segment, index) => {
            pathSoFar = pathSoFar ? pathSoFar + "/" + segment : segment;
            return { label: segment, path: pathSoFar, isLast: index === segments.length - 1 };
        });
    }

    get visibleEntries() {
        return this.entries
            .filter((e) => e.isDirectory || e.name.toLowerCase().endsWith(".rt4"))
            .slice()
            .sort((a, b) => {
                if (a.isDirectory !== b.isDirectory)
                    return a.isDirectory ? -1 : 1;
                return a.name.localeCompare(b.name);
            });
    }

    refreshCurrentProfile(): void {
        getCurrentProfile()
            .then((info) => {
                if (!info.ok) {
                    this.currentProfile = "Unknown (" + info.error + ")";
                    return;
                }
                this.currentProfile = info.loaded ? (info.path ?? "") : "None loaded";
            })
            .catch(() => (this.currentProfile = "Unknown"));
    }

    refresh(): void {
        this.message = "";
        this.entries = [];

        const requestId = ++this.requestId;

        listProfiles(this.currentPath)
            .then((info) => {
                if (requestId !== this.requestId)
                    return;

                if (!info.ok) {
                    this.message = "Could not list " + this.currentPath + ": " + info.error;
                    return;
                }
                this.entries = info.entries;
            })
            .catch(() => {
                if (requestId === this.requestId)
                    this.message = "Could not reach the device.";
            })
            .finally(() => {
                if (requestId === this.requestId)
                    this.navigating = false;
            });
    }

    private navigateTo(path: string): void {
        this.navigating = true;
        this.currentPath = path;
        history.replaceState(null, "", hashForPath(path));
        this.refresh();
    }

    openDirectory(name: string): void {
        if (this.navigating)
            return;

        this.navigateTo(this.currentPath + "/" + name);
    }

    goToBreadcrumb(path: string): void {
        if (this.navigating)
            return;

        this.navigateTo(path);
    }

    load(name: string): void {
        this.message = "";
        this.loadingName = name;

        loadProfile(name)
            .then((info) => {
                if (!info.ok) {
                    this.message = "Load failed: " + info.error;
                    return;
                }
                this.message = "Loaded " + name + ". The RT4K's live settings now match this profile.";
                this.refreshCurrentProfile();
            })
            .catch(() => (this.message = "Load failed."))
            .finally(() => (this.loadingName = null));
    }

    download(name: string): void {
        this.message = "";
        this.downloadingName = name;

        downloadProfile(name)
            .then((blob) => {
                const url = URL.createObjectURL(blob);
                const link = document.createElement("a");
                link.href = url;
                link.download = name.slice(name.lastIndexOf("/") + 1);
                document.body.appendChild(link);
                link.click();
                link.remove();
                URL.revokeObjectURL(url);
            })
            .catch((error: Error) => (this.message = "Download failed: " + error.message))
            .finally(() => (this.downloadingName = null));
    }

    delete(name: string): void {
        this.message = "";
        this.deletingName = name;

        deleteProfile(name)
            .then((info) => {
                if (!info.ok) {
                    this.message = "Delete failed: " + info.error;
                    return;
                }

                this.message = "Deleted " + name + ".";

                const entryName = name.slice(name.lastIndexOf("/") + 1);
                this.entries = this.entries.filter((e) => e.name !== entryName);
            })
            .catch(() => (this.message = "Delete failed."))
            .finally(() => (this.deletingName = null));
    }

    setUploadFile(file: File | null): void {
        this.uploadFile = file;
    }

    upload(onCleared: () => void): void {
        if (!this.uploadFile)
            return;

        const file = this.uploadFile;
        this.message = "";
        this.uploading = true;

        uploadProfile(this.relativeName(file.name), file)
            .then((info) => {
                if (!info.ok) {
                    this.message = "Upload failed: " + info.error;
                    return;
                }
                this.message = "Uploaded " + file.name + " to " + this.currentPath + ".";
                this.uploadFile = null;
                onCleared();
                this.refresh();
            })
            .catch(() => (this.message = "Upload failed."))
            .finally(() => (this.uploading = false));
    }
}

export const profilesStore = new ProfilesStore();
