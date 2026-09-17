// Firmware and web UI update from <env>-ota-update.bin, when /api/status
// reports ota_supported. XHR for upload progress.
export function uploadOta(file: File, onProgress: (fraction: number) => void): Promise<void> {
    return new Promise((resolve, reject) => {
        const xhr = new XMLHttpRequest();
        xhr.open("POST", "/api/ota/upload");
        xhr.setRequestHeader("Content-Type", "application/octet-stream");

        xhr.upload.onprogress = (event) => {
            if (event.lengthComputable)
                onProgress(event.loaded / event.total);
        };

        xhr.onload = () => {
            if (xhr.status >= 200 && xhr.status < 300) {
                resolve();
                return;
            }

            let message = "Update failed (" + xhr.status + ")";
            try {
                const info = JSON.parse(xhr.responseText);
                if (info.error)
                    message = info.error;
            } catch {}

            reject(new Error(message));
        };

        xhr.onerror = () => reject(new Error("Lost connection to the device during upload."));

        xhr.send(file);
    });
}
