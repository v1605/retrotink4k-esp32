import { getWifi, postHostname, postWifi, postWifiForget, scanWifiNetworks } from "./api";
import type { ScannedNetwork } from "./api";

class WifiStore {
    mode = $state("--");
    currentSsid = $state("--");
    ip = $state("--");
    hostname = $state("--");
    hostnameInput = $state("");
    ssid = $state("");
    password = $state("");
    message = $state("");
    scanning = $state(false);
    scanned = $state(false);
    networks = $state<ScannedNetwork[]>([]);

    refresh(): void {
        getWifi()
            .then((info) => {
                this.mode = info.mode;
                this.currentSsid = info.ssid;
                this.ip = info.ip;
                this.hostname = info.hostname;

                if (!this.ssid)
                    this.ssid = info.ssid;

                if (!this.hostnameInput)
                    this.hostnameInput = info.hostname;

                if (info.scan.ok) {
                    this.networks = info.scan.networks;
                    this.scanned = true;
                }
            })
            .catch(() => {});
    }

    saveHostname(): void {
        if (!this.hostnameInput)
            return;

        this.message = "Saving hostname and restarting...";

        postHostname(this.hostnameInput).catch(() => {});
    }

    connect(): void {
        if (!this.ssid)
            return;

        this.message =
            "Saving and restarting. Reconnect to the new network " +
            "(or TinkEsp32 if it fails) and reload this page.";

        postWifi(this.ssid, this.password).catch(() => {});
    }

    forget(): void {
        this.message = "Forgetting network and restarting into TinkEsp32 access point mode.";
        postWifiForget().catch(() => {});
    }

    scan(): void {
        this.scanning = true;
        this.scanned = false;
        this.networks = [];

        scanWifiNetworks()
            .then((info) => {
                if (!info.ok) {
                    this.message = info.error || "Scan failed.";
                    return;
                }
                this.networks = info.networks;
                this.scanned = true;
            })
            .catch(() => (this.message = "Scan failed."))
            .finally(() => (this.scanning = false));
    }

    selectNetwork(network: ScannedNetwork): void {
        this.ssid = network.ssid;
    }
}

export const wifiStore = new WifiStore();
