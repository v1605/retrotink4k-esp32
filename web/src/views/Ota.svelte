<script lang="ts">
    import { onMount } from "svelte";
    import { otaStore } from "../lib/ota.svelte";
    import Message from "../components/Message.svelte";
    import HelpIcon from "../components/HelpIcon.svelte";

    let fileInputEl: HTMLInputElement = $state()!;

    const progressPercent = $derived(Math.round(otaStore.progress * 100));

    function onFileChange(): void {
        otaStore.setFile(fileInputEl.files?.[0] ?? null);
    }

    function onUpload(): void {
        if (!confirm("Flash this update and restart the device? Don't power it off while this runs."))
            return;

        otaStore.upload(() => (fileInputEl.value = ""));
    }

    onMount(() => otaStore.checkSupport());
</script>

<div class="flex-grow-1 overflow-auto min-h-0">
    <div class="container py-4">
        <div class="row justify-content-center">
            <div class="col-12 col-sm-8 col-md-6 col-lg-4">

                {#if otaStore.checked && !otaStore.supported}
                    <div class="alert alert-warning" role="alert">
                        This board's firmware wasn't built with OTA support.
                    </div>
                {:else}
                    <div class="card bg-body-tertiary mb-3">
                        <div class="card-body">
                            <h5 class="card-title d-flex align-items-center gap-2">
                                Firmware update
                                <HelpIcon
                                    text="Flashes firmware and web UI together from one combined update file (built by `pio run -t release` as <board>-ota-update.bin) and restarts the device. Don't power it off or close this page while the upload is running -- an interrupted update can leave the web UI needing a re-upload to recover, though the device stays reachable either way."
                                />
                            </h5>

                            <input
                                bind:this={fileInputEl}
                                type="file"
                                class="form-control"
                                accept=".bin"
                                disabled={otaStore.uploading}
                                onchange={onFileChange}
                            >

                            {#if otaStore.uploading}
                                <div
                                    class="progress mt-2"
                                    role="progressbar"
                                    aria-label="Upload progress"
                                    aria-valuenow={progressPercent}
                                    aria-valuemin="0"
                                    aria-valuemax="100"
                                >
                                    <div class="progress-bar" style="width: {progressPercent}%"></div>
                                </div>
                            {/if}

                            <button
                                type="button"
                                class="btn btn-primary mt-2"
                                disabled={!otaStore.file || otaStore.uploading}
                                onclick={onUpload}
                            >
                                {otaStore.uploading ? "Uploading (" + progressPercent + "%)..." : "Flash and restart"}
                            </button>
                        </div>
                    </div>
                {/if}

                <Message text={otaStore.message} />

            </div>
        </div>
    </div>
</div>
