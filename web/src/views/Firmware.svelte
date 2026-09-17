<script lang="ts">
    import { onDestroy, onMount } from "svelte";
    import { firmwareStore } from "../lib/firmware.svelte";
    import Message from "../components/Message.svelte";
    import HelpIcon from "../components/HelpIcon.svelte";

    let zipInputEl: HTMLInputElement = $state()!;

    function onZipChange(): void {
        firmwareStore.setZip(zipInputEl.files?.[0] ?? null);
    }

    function onRun(): void {
        const plan = firmwareStore.plan;
        const device = firmwareStore.device;
        if (!plan || !device)
            return;

        const target = plan.version ?? plan.rbf.baseName;
        const same = plan.version === device.version ? " (the version it's already running)" : "";
        if (!confirm("Install RT4K firmware " + target + same + " on your " + device.model + "? This uploads " +
            plan.rbf.baseName + " and rt4kup.bin, then flashes the RT4K -- it reboots for about 40 seconds. " +
            "Don't power it off while it's flashing."))
            return;

        firmwareStore.run();
    }

    function onInstall(): void {
        const version = firmwareStore.pending?.version;
        if (!version || !confirm("Install RT4K firmware " + version + "? The RT4K reboots for about 40 seconds -- don't power it off while it's flashing."))
            return;

        firmwareStore.install();
    }

    const statusBadgeClass: Record<string, string> = {
        pending: "text-bg-secondary",
        active: "text-bg-primary",
        done: "text-bg-success",
        error: "text-bg-danger",
    };

    const statusLabel: Record<string, string> = {
        pending: "Pending",
        active: "Working",
        done: "Done",
        error: "Failed",
    };

    onMount(() => firmwareStore.attach());
    onDestroy(() => firmwareStore.detach());
</script>

<div class="flex-grow-1 overflow-auto min-h-0">
    <div class="container py-4">
        <div class="row justify-content-center">
            <div class="col-12 col-sm-10 col-md-8 col-lg-6">

                <div class="card bg-body-tertiary mb-3">
                    <div class="card-body">
                        <h5 class="card-title d-flex align-items-center gap-2">
                            Update RT4K firmware
                            <HelpIcon
                                text="Pick the firmware .zip exactly as downloaded from RetroTINK. The page unzips it, checks each file against the zip's checksums, and picks the .rbf for the model your RT4K reports. It uploads that and rt4kup.bin to the SD card's root (a 4-5MB .rbf takes about a minute), confirms rt4kup.bin is for the expected version, then flashes it. The RT4K reboots for about 40 seconds (LED flashing pink, then blue, then green when done); don't power it off meanwhile. Profiles, masks, banners and other files on the card aren't touched."
                            />
                        </h5>

                        <p class="card-text mb-2">
                            {#if firmwareStore.device}
                                Connected: <strong>{firmwareStore.device.model}</strong>,
                                firmware <strong>{firmwareStore.device.version}</strong>
                            {:else if firmwareStore.loadingDevice}
                                <span class="text-body-secondary">Asking the RT4K for its model...</span>
                            {:else}
                                <span class="text-body-secondary">{firmwareStore.deviceError || "No RT4K detected."}</span>
                                <button
                                    type="button"
                                    class="btn btn-link p-0 align-baseline"
                                    onclick={() => firmwareStore.refreshDevice()}
                                >Retry</button>
                            {/if}
                        </p>

                        <input
                            bind:this={zipInputEl}
                            type="file"
                            class="form-control"
                            accept=".zip,application/zip"
                            disabled={firmwareStore.running}
                            onchange={onZipChange}
                        >

                        {#if firmwareStore.planError}
                            <div class="alert alert-warning mt-2 mb-0" role="alert">{firmwareStore.planError}</div>
                        {:else if firmwareStore.plan}
                            <p class="card-text mt-2 mb-0">
                                Firmware <strong>{firmwareStore.plan.version ?? "of unknown version"}</strong>:
                                uploads <code>{firmwareStore.plan.rbf.baseName}</code> and <code>rt4kup.bin</code>.
                                {#if firmwareStore.device && firmwareStore.plan.version === firmwareStore.device.version}
                                    <span class="text-body-secondary">Your RT4K is already running this version.</span>
                                {/if}
                            </p>
                        {/if}

                        {#if firmwareStore.steps.length > 0}
                            <ul class="list-group mt-3">
                                {#each firmwareStore.steps as step (step.key)}
                                    <li class="list-group-item">
                                        <div class="d-flex justify-content-between align-items-center gap-2">
                                            <span class="text-truncate">{step.label}</span>
                                            <span class="badge rounded-pill {statusBadgeClass[step.status]}">
                                                {statusLabel[step.status]}
                                            </span>
                                        </div>

                                        {#if step.detail}
                                            <div class="small text-body-secondary">{step.detail}</div>
                                        {/if}

                                        {#if step.status === "active" && step.progress !== null}
                                            <div
                                                class="progress mt-1"
                                                role="progressbar"
                                                aria-label={step.label}
                                                aria-valuenow={Math.round(step.progress * 100)}
                                                aria-valuemin="0"
                                                aria-valuemax="100"
                                            >
                                                <div class="progress-bar" style="width: {Math.round(step.progress * 100)}%"></div>
                                            </div>
                                        {/if}
                                    </li>
                                {/each}
                            </ul>
                        {/if}

                        <button
                            type="button"
                            class="btn btn-danger mt-3"
                            disabled={!firmwareStore.plan || !firmwareStore.device || firmwareStore.running}
                            onclick={onRun}
                        >
                            {firmwareStore.running ? "Updating..." : "Upload and install"}
                        </button>
                    </div>
                </div>

                <Message text={firmwareStore.message} />

                <div class="card bg-body-tertiary">
                    <div class="card-body">
                        <h5 class="card-title d-flex align-items-center gap-2">
                            Install files already on the SD card
                            <HelpIcon
                                text="For firmware files you copied to the card yourself. The RT4K installs whatever version its rt4kup.bin is for; Check SD card asks it which version that is."
                            />
                        </h5>

                        <div class="d-flex flex-wrap gap-2">
                            <button
                                type="button"
                                class="btn btn-secondary"
                                disabled={firmwareStore.checking || firmwareStore.installing || firmwareStore.running}
                                onclick={() => firmwareStore.check()}
                            >
                                {firmwareStore.checking ? "Checking..." : "Check SD card"}
                            </button>

                            {#if firmwareStore.pending}
                                <button
                                    type="button"
                                    class="btn btn-danger"
                                    disabled={firmwareStore.installing || firmwareStore.running}
                                    onclick={onInstall}
                                >
                                    {firmwareStore.installing ? "Starting..." : "Install " + firmwareStore.pending.version}
                                </button>
                            {/if}
                        </div>

                        {#if firmwareStore.pending}
                            <p class="card-text mt-2 mb-0">
                                The SD card has version <strong>{firmwareStore.pending.version}</strong> ready to install.
                            </p>
                        {/if}

                        {#if firmwareStore.installMessage}
                            <div class="mt-2">
                                <Message text={firmwareStore.installMessage} />
                            </div>
                        {/if}

                        <hr>

                        <p class="card-text mb-1 text-body-secondary">Or on the RT4K itself:</p>
                        <ol class="mb-2 ps-3 text-body-secondary">
                            <li>Open <strong>Advanced Settings &gt; OSD/Firmware</strong>.</li>
                            <li>Under Firmware update, choose <strong>Check SD Card</strong>, then confirm the install.</li>
                        </ol>
                        <p class="card-text mb-0 text-body-secondary">
                            Or the reset-button method: unplug the RT4K, hold the reset button on the back,
                            and plug it back in while still holding it.
                        </p>
                    </div>
                </div>

            </div>
        </div>
    </div>
</div>
