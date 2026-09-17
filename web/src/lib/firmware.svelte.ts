import {
    checkFirmware,
    getFirmwareDevice,
    installFirmware,
    openFirmwareStatusSocket,
    uploadFirmwareFile,
} from "./api";
import type { FirmwareStatusEvent } from "./api";
import { extractZipEntry, listZip } from "./zip";
import type { ZipEntry } from "./zip";

type StepStatus = "pending" | "active" | "done" | "error";

export interface Step {
    key: string;
    label: string;
    status: StepStatus;
    detail: string;
    progress: number | null; // 0..1 while a measurable phase runs
    file?: string;           // upload steps: the name the bridge reports progress for
}

interface Plan {
    zipName: string;
    rbf: ZipEntry;
    update: ZipEntry;
    version: string | null; // from the .rbf's name, e.g. rt4k_1801.rbf -> 1.80.1
}

const REBOOT_POLL_MS = 5000;
// Flashing takes about 40s; an earlier reply is from before the reboot.
const REBOOT_MIN_MS = 40000;
const REBOOT_TIMEOUT_MS = 180000;

// .rbf filename prefix for the model the RT4K reports (e.g. "RT4K_Pro").
function rbfPrefixFor(model: string): string | null {
    const upper = model.toUpperCase();
    if (upper.includes("6X"))
        return "rt6x_";
    if (upper.includes("CE"))
        return "rt4kce_";
    if (upper.includes("4K"))
        return "rt4k_";
    return null;
}

// "1801" -> "1.80.1", following RetroTINK's file naming.
function versionFromDigits(digits: string): string | null {
    return /^\d{4}$/.test(digits) ? digits[0] + "." + digits.slice(1, 3) + "." + digits[3] : null;
}

function delay(ms: number): Promise<void> {
    return new Promise((resolve) => setTimeout(resolve, ms));
}

class FirmwareStore {
    device = $state<{ model: string; version: string } | null>(null);
    deviceError = $state("");
    loadingDevice = $state(false);

    zipFile = $state<File | null>(null);
    plan = $state<Plan | null>(null);
    planError = $state("");

    steps = $state<Step[]>([]);
    running = $state(false);
    message = $state("");

    // Installing whatever's already on the SD card, without a zip.
    checking = $state(false);
    installing = $state(false);
    pending = $state<{ version: string; token: string } | null>(null);
    installMessage = $state("");

    private entries: ZipEntry[] = [];
    private closeSocket: (() => void) | null = null;

    attach(): void {
        if (!this.closeSocket)
            this.closeSocket = openFirmwareStatusSocket((event) => this.onStatus(event));
        if (!this.running)
            this.refreshDevice();
    }

    detach(): void {
        this.closeSocket?.();
        this.closeSocket = null;
    }

    refreshDevice(): Promise<void> {
        this.loadingDevice = true;
        this.deviceError = "";

        return getFirmwareDevice()
            .then((info) => {
                if (info.ok && info.model && info.version) {
                    this.device = { model: info.model, version: info.version };
                    this.buildPlan();
                } else {
                    this.device = null;
                    this.deviceError = info.error ?? "The RT4K didn't report its model.";
                }
            })
            .catch(() => {
                this.device = null;
                this.deviceError = "Could not reach the device.";
            })
            .finally(() => (this.loadingDevice = false));
    }

    async setZip(file: File | null): Promise<void> {
        this.zipFile = file;
        this.entries = [];
        this.plan = null;
        this.planError = "";
        this.steps = [];
        this.message = "";

        if (!file)
            return;

        try {
            this.entries = await listZip(file);
        } catch (error) {
            this.planError = (error as Error).message;
            return;
        }

        this.buildPlan();
    }

    private buildPlan(): void {
        const zip = this.zipFile;
        if (!zip || this.entries.length === 0)
            return;

        this.plan = null;
        this.planError = "";

        const update = this.entries.find((e) => e.baseName.toLowerCase() === "rt4kup.bin");
        if (!update) {
            this.planError = "No rt4kup.bin in " + zip.name + " -- is it a RetroTINK firmware zip?";
            return;
        }

        // Finished once the RT4K reports its model.
        if (!this.device)
            return;

        const prefix = rbfPrefixFor(this.device.model);
        if (!prefix) {
            this.planError = "Not sure which firmware file is for a " + this.device.model + ".";
            return;
        }

        const pattern = new RegExp("^" + prefix + "(\\d+)\\.rbf$", "i");
        const rbf = this.entries.find((e) => pattern.test(e.baseName));
        if (!rbf) {
            this.planError = zip.name + " has no " + prefix + "*.rbf for your " + this.device.model + ".";
            return;
        }

        const digits = pattern.exec(rbf.baseName)?.[1] ?? "";
        this.plan = { zipName: zip.name, rbf, update, version: versionFromDigits(digits) };
    }

    async run(): Promise<void> {
        const plan = this.plan;
        const zip = this.zipFile;
        if (!plan || !zip || this.running)
            return;

        const rbfName = plan.rbf.baseName;

        this.running = true;
        this.message = "";
        this.pending = null;
        this.installMessage = "";
        this.steps = [
            { key: "extract", label: "Extract and verify " + rbfName + " and rt4kup.bin", status: "pending", detail: "", progress: null },
            { key: "rbf", label: "Upload " + rbfName, status: "pending", detail: "", progress: null, file: rbfName },
            { key: "update", label: "Upload rt4kup.bin", status: "pending", detail: "", progress: null, file: "rt4kup.bin" },
            { key: "check", label: "Check the SD card", status: "pending", detail: "", progress: null },
            { key: "install", label: "Start the install", status: "pending", detail: "", progress: null },
            { key: "reboot", label: "Wait for the RT4K to restart", status: "pending", detail: "", progress: null },
        ];

        try {
            this.begin("extract");
            const rbfData = await extractZipEntry(zip, plan.rbf);
            const updateData = await extractZipEntry(zip, plan.update);
            this.finish("extract");

            // .rbf first, so a failed upload leaves rt4kup.bin on the old version.
            await this.upload("rbf", rbfName, rbfData);
            await this.upload("update", "rt4kup.bin", updateData);

            this.begin("check");
            const check = await checkFirmware();
            if (!check.ok || !check.version || !check.token)
                throw new Error(check.error ?? "The RT4K couldn't validate rt4kup.bin.");
            this.finish("check", "rt4kup.bin is for " + check.version);

            if (plan.version && check.version !== plan.version &&
                !confirm("The SD card's rt4kup.bin is for " + check.version + ", but " + plan.zipName +
                    " looked like " + plan.version + ". Install " + check.version + " anyway?"))
                throw new Error("Stopped before installing: the versions didn't match.");

            this.begin("install");
            const install = await installFirmware(check.token);
            if (!install.ok)
                throw new Error(install.error ?? "The RT4K didn't start the install.");
            this.finish("install");

            this.begin("reboot", "Flashing -- don't power off the RT4K (LED pink, then blue)");
            const version = await this.waitForRestart(check.version);
            this.finish("reboot", "Running " + version);

            this.message = "Firmware " + version + " installed.";
        } catch (error) {
            const active = this.steps.find((s) => s.status === "active");
            if (active) {
                active.status = "error";
                active.progress = null;
            }
            this.message = (error as Error).message;
        } finally {
            this.running = false;
            this.refreshDevice();
        }
    }

    private async upload(key: string, name: string, data: Uint8Array<ArrayBuffer>): Promise<void> {
        this.begin(key, "Sending to the bridge");
        const step = this.step(key);
        step.progress = 0;

        const result = await uploadFirmwareFile(name, new Blob([data]), (fraction) => {
            if (step.detail === "Sending to the bridge")
                step.progress = fraction;
        });

        if (!result.ok)
            throw new Error("Uploading " + name + " failed: " + result.error);

        this.finish(key);
    }

    // Poll until the RT4K reboots and reports the expected version.
    private async waitForRestart(expected: string): Promise<string> {
        const started = Date.now();
        let last = "";

        while (Date.now() - started < REBOOT_TIMEOUT_MS) {
            await delay(REBOOT_POLL_MS);

            try {
                const info = await getFirmwareDevice();
                if (info.ok && info.version) {
                    last = info.version;
                    if (info.version === expected && Date.now() - started >= REBOOT_MIN_MS)
                        return info.version;
                }
            } catch {}
        }

        throw new Error(last
            ? "The RT4K is still reporting " + last + " -- check its LED (blinking red means the install failed)."
            : "The RT4K didn't come back within 3 minutes -- check its LED.");
    }

    // Status events for the RT4K write (the HTTP upload only covers the bridge).
    private onStatus(event: FirmwareStatusEvent): void {
        const step = this.steps.find((s) => s.status === "active" && s.file !== undefined && s.file === event.file);
        if (!step)
            return;

        if (event.type === "progress" && event.total) {
            step.detail = "Writing to the RT4K's SD card";
            step.progress = (event.sent ?? 0) / event.total;
        } else if (event.type === "verifying") {
            step.detail = "RT4K verifying the file";
            step.progress = null;
        }
    }

    private step(key: string): Step {
        return this.steps.find((s) => s.key === key)!;
    }

    private begin(key: string, detail = ""): void {
        const step = this.step(key);
        step.status = "active";
        step.detail = detail;
        step.progress = null;
    }

    private finish(key: string, detail = ""): void {
        const step = this.step(key);
        step.status = "done";
        step.detail = detail;
        step.progress = null;
    }

    check(): void {
        this.checking = true;
        this.pending = null;
        this.installMessage = "";

        checkFirmware()
            .then((info) => {
                if (!info.ok || !info.version || !info.token) {
                    this.installMessage = "Check failed: " + (info.error ?? "no update found");
                    return;
                }
                this.pending = { version: info.version, token: info.token };
            })
            .catch(() => (this.installMessage = "Could not reach the device."))
            .finally(() => (this.checking = false));
    }

    install(): void {
        if (!this.pending || this.installing)
            return;

        const { version, token } = this.pending;
        this.installing = true;
        this.installMessage = "";

        installFirmware(token)
            .then((info) => {
                if (!info.ok) {
                    this.installMessage = "Install failed: " + info.error;
                    return;
                }
                this.pending = null;
                this.installMessage = "Installing " + version + ". The RT4K reboots for about 40 seconds (LED flashing pink, then blue) and comes back with a green LED when it's done.";
            })
            .catch(() => (this.installMessage = "Lost contact with the device -- watch the RT4K's LED to see whether the install started."))
            .finally(() => (this.installing = false));
    }
}

export const firmwareStore = new FirmwareStore();
