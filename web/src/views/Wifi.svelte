<script lang="ts">
    import { onMount } from "svelte";
    import { wifiStore } from "../lib/wifi.svelte";
    import type { ScannedNetwork } from "../lib/api";
    import { faLock, faLockOpen } from "@fortawesome/free-solid-svg-icons";
    import Message from "../components/Message.svelte";
    import HelpIcon from "../components/HelpIcon.svelte";
    import Icon from "../components/Icon.svelte";

    let passwordEl: HTMLInputElement;

    function onSubmit(event: SubmitEvent): void {
        event.preventDefault();
        wifiStore.connect();
    }

    function onSelect(network: ScannedNetwork): void {
        wifiStore.selectNetwork(network);
        passwordEl.focus();
    }

    function onSaveHostname(event: SubmitEvent): void {
        event.preventDefault();
        wifiStore.saveHostname();
    }

    onMount(() => wifiStore.refresh());
</script>

<div class="flex-grow-1 overflow-auto min-h-0">
    <div class="container py-4">
        <div class="row justify-content-center">
            <div class="col-12 col-sm-8 col-md-6 col-lg-4">

                <div class="card bg-body-tertiary mb-3">
                    <div class="card-body">
                        <h5 class="card-title">Current status</h5>
                        <p class="card-text mb-0">SSID: {wifiStore.currentSsid}</p>
                        <p class="card-text mb-0">IP: {wifiStore.ip}</p>
                        <p class="card-text mb-0">Hostname: {wifiStore.hostname}.local</p>
                    </div>
                </div>

                <form class="card bg-body-tertiary mb-3" onsubmit={onSaveHostname}>
                    <div class="card-body">
                        <h5 class="card-title d-flex align-items-center gap-2">
                            Device name
                            <HelpIcon
                                text="Reachable as <name>.local once on your network. Letters, digits, and hyphens only. Saving restarts the device."
                            />
                        </h5>

                        <div class="mb-3">
                            <label for="hostname" class="form-label">Hostname</label>
                            <input
                                type="text"
                                class="form-control"
                                id="hostname"
                                autocomplete="off"
                                spellcheck="false"
                                maxlength="32"
                                pattern="[A-Za-z0-9]([A-Za-z0-9\-]*[A-Za-z0-9])?"
                                required
                                bind:value={wifiStore.hostnameInput}
                            >
                        </div>

                        <button type="submit" class="btn btn-primary">Save</button>
                    </div>
                </form>

                <form class="card bg-body-tertiary mb-3" onsubmit={onSubmit}>
                    <div class="card-body">
                        <div class="d-flex justify-content-between align-items-center mb-2">
                            <h5 class="card-title mb-0">Connect to a network</h5>
                            <button
                                type="button"
                                class="btn btn-sm btn-secondary"
                                disabled={wifiStore.scanning}
                                onclick={() => wifiStore.scan()}
                            >
                                {wifiStore.scanning ? "Scanning..." : "Scan"}
                            </button>
                        </div>

                        {#if wifiStore.scanned}
                            <div class="list-group mb-3">
                                {#if wifiStore.networks.length === 0}
                                    <div class="list-group-item text-body-secondary">No networks found.</div>
                                {:else}
                                    {#each wifiStore.networks as network (network.ssid)}
                                        <button
                                            type="button"
                                            class="list-group-item list-group-item-action d-flex justify-content-between align-items-center"
                                            onclick={() => onSelect(network)}
                                        >
                                            <span class="d-flex align-items-center gap-1">
                                                <Icon icon={network.secure ? faLock : faLockOpen} size={14} />
                                                {network.ssid}
                                            </span>
                                            <span class="badge text-bg-secondary">{network.rssi} dBm</span>
                                        </button>
                                    {/each}
                                {/if}
                            </div>
                        {:else}
                            <div class="list-group mb-3">
                                <div class="list-group-item text-body-secondary">
                                    {wifiStore.scanning ? "Scanning..." : "No scan yet. Press Scan to search for nearby networks."}
                                </div>
                            </div>
                        {/if}

                        <div class="mb-3">
                            <label for="ssid" class="form-label">Network name (SSID)</label>
                            <input
                                type="text"
                                class="form-control"
                                id="ssid"
                                autocomplete="off"
                                spellcheck="false"
                                required
                                bind:value={wifiStore.ssid}
                            >
                        </div>

                        <div class="mb-3">
                            <label for="password" class="form-label">Password</label>
                            <input
                                bind:this={passwordEl}
                                type="password"
                                class="form-control"
                                id="password"
                                autocomplete="off"
                                bind:value={wifiStore.password}
                            >
                        </div>

                        <button type="submit" class="btn btn-primary">Connect</button>
                    </div>
                </form>

                <div class="card bg-body-tertiary mb-3">
                    <div class="card-body">
                        <h5 class="card-title d-flex align-items-center gap-2">
                            Forget network
                            <HelpIcon text="Clears the saved network and restarts into the TinkEsp32 access point." />
                        </h5>
                        <button type="button" class="btn btn-outline-danger" onclick={() => wifiStore.forget()}>
                            Forget network
                        </button>
                    </div>
                </div>

                <Message text={wifiStore.message} />

            </div>
        </div>
    </div>
</div>
