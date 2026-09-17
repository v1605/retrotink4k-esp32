import { getStatus, postSerialBaud } from "./api";

class SerialStore {
    ftdiConnected = $state(false);
    baud = $state(0);
    selectedBaud = $state("");
    message = $state("");

    private timer: ReturnType<typeof setInterval> | undefined;

    refresh(): void {
        getStatus()
            .then((info) => {
                this.ftdiConnected = info.ftdi_connected;
                this.baud = info.baud;
                this.selectedBaud = String(info.baud);
            })
            .catch(() => {});
    }

    apply(): void {
        if (!this.selectedBaud) {
            this.message = "Choose a baud rate first.";
            return;
        }

        this.message = "Applying...";

        postSerialBaud(this.selectedBaud)
            .then((info) => {
                if (!info.ok) {
                    this.message = "Failed to apply: " + info.error;
                    return;
                }
                this.message = "Applied.";
                this.refresh();
            })
            .catch(() => (this.message = "Failed to apply."));
    }

    startPolling(): void {
        if (this.timer)
            return;
        this.refresh();
        this.timer = setInterval(() => this.refresh(), 3000);
    }

    stopPolling(): void {
        if (this.timer)
            clearInterval(this.timer);
        this.timer = undefined;
    }
}

export const serialStore = new SerialStore();
