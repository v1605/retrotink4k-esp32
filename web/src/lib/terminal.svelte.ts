import { getStatus } from "./api";

const textDecoder = new TextDecoder();

const HISTORY_KEY = "terminalHistory";
const HISTORY_LIMIT = 20;

function loadHistory(): string[] {
    try {
        const saved = JSON.parse(localStorage.getItem(HISTORY_KEY) ?? "[]");
        return Array.isArray(saved) ? saved.filter((item) => typeof item === "string").slice(-HISTORY_LIMIT) : [];
    } catch {
        return [];
    }
}

class TerminalStore {
    logText = $state("");
    wsConnected = $state(false);
    ftdiConnected = $state(false);
    baud = $state(0);

    readonly history: string[] = loadHistory();

    private socket: WebSocket | null = null;
    private statusTimer: ReturnType<typeof setInterval> | null = null;

    connect(): void {
        if (this.socket)
            return;

        const protocol = location.protocol === "https:" ? "wss://" : "ws://";
        const socket = new WebSocket(protocol + location.host + "/ws");
        this.socket = socket;

        socket.binaryType = "arraybuffer";

        socket.onopen = () => {
            this.wsConnected = true;
            this.refreshStatus();
        };

        socket.onclose = () => {
            this.wsConnected = false;
            this.socket = null;
            setTimeout(() => this.connect(), 1000);
        };

        socket.onerror = () => socket.close();

        socket.onmessage = (event: MessageEvent) => {
            // The bridge sends printable ASCII only, so UTF-8 decoding is exact.
            this.print(event.data instanceof ArrayBuffer ? textDecoder.decode(event.data) : event.data);
        };
    }

    print(text: string): void {
        this.logText += text;
    }

    clear(): void {
        this.logText = "";
    }

    send(text: string): void {
        this.remember(text);

        if (!this.socket || this.socket.readyState !== WebSocket.OPEN) {
            this.print("\r\n[WebSocket not connected]\r\n");
            return;
        }

        this.socket.send(text + "\n");
        this.print("> " + text + "\r\n");
    }

    private remember(text: string): void {
        if (this.history[this.history.length - 1] !== text)
            this.history.push(text);
        if (this.history.length > HISTORY_LIMIT)
            this.history.splice(0, this.history.length - HISTORY_LIMIT);

        try {
            localStorage.setItem(HISTORY_KEY, JSON.stringify(this.history));
        } catch {}
    }

    refreshStatus(): void {
        getStatus()
            .then((info) => {
                this.ftdiConnected = info.ftdi_connected;
                this.baud = info.baud;
            })
            .catch(() => {});
    }

    startPolling(): void {
        if (this.statusTimer)
            return;
        this.refreshStatus();
        this.statusTimer = setInterval(() => this.refreshStatus(), 3000);
    }

    stopPolling(): void {
        if (this.statusTimer)
            clearInterval(this.statusTimer);
        this.statusTimer = null;
    }
}

export const terminalStore = new TerminalStore();
terminalStore.connect();
