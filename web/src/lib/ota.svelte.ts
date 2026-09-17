import { getStatus, uploadOta } from "./api";

class OtaStore {
    // True until checkSupport() resolves, so "not supported" doesn't flash.
    supported = $state(true);
    checked = $state(false);

    file = $state<File | null>(null);
    uploading = $state(false);
    progress = $state(0);
    message = $state("");

    checkSupport(): void {
        getStatus()
            .then((info) => (this.supported = info.ota_supported))
            .catch(() => {})
            .finally(() => (this.checked = true));
    }

    setFile(file: File | null): void {
        this.file = file;
        this.message = "";
    }

    upload(onCleared: () => void): void {
        if (!this.file || this.uploading)
            return;

        const file = this.file;
        this.uploading = true;
        this.progress = 0;
        this.message = "";

        uploadOta(file, (fraction) => (this.progress = fraction))
            .then(() => {
                this.message = "Update applied. The device is restarting -- reload this page in a few seconds.";
                this.file = null;
                onCleared();
            })
            .catch((error: Error) => (this.message = "Update failed: " + error.message))
            .finally(() => (this.uploading = false));
    }
}

export const otaStore = new OtaStore();
