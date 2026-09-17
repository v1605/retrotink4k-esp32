<script lang="ts">
    import { onMount, onDestroy } from "svelte";
    import { serialStore } from "../lib/serial.svelte";
    import Message from "../components/Message.svelte";
    import HelpIcon from "../components/HelpIcon.svelte";

    function onSubmit(event: SubmitEvent): void {
        event.preventDefault();
        serialStore.apply();
    }

    onMount(() => serialStore.startPolling());
    onDestroy(() => serialStore.stopPolling());
</script>

<div class="flex-grow-1 overflow-auto min-h-0">
    <div class="container py-4">
        <div class="row justify-content-center">
            <div class="col-12 col-sm-8 col-md-6 col-lg-4">

                <div class="card bg-body-tertiary mb-3">
                    <div class="card-body">
                        <h5 class="card-title">Current status</h5>
                        <p class="card-text mb-0">
                            RT4K:
                            <span
                                class="badge"
                                class:text-bg-success={serialStore.ftdiConnected}
                                class:text-bg-danger={!serialStore.ftdiConnected}
                            >
                                {serialStore.ftdiConnected ? "connected" : "not connected"}
                            </span>
                        </p>
                        <p class="card-text mb-0">Baud: {serialStore.baud}</p>
                    </div>
                </div>

                <form class="card bg-body-tertiary mb-3" onsubmit={onSubmit}>
                    <div class="card-body">
                        <h5 class="card-title d-flex align-items-center gap-2">
                            Baud rate
                            <HelpIcon text="Must match the RetroTINK-4K firmware." />
                        </h5>

                        <div class="form-check">
                            <input
                                class="form-check-input"
                                type="radio"
                                name="baud"
                                id="baud2000000"
                                value="2000000"
                                bind:group={serialStore.selectedBaud}
                            >
                            <label class="form-check-label" for="baud2000000">
                                2,000,000 bps &mdash; firmware &ge; 1.75.0
                            </label>
                        </div>

                        <div class="form-check mb-3">
                            <input
                                class="form-check-input"
                                type="radio"
                                name="baud"
                                id="baud115200"
                                value="115200"
                                bind:group={serialStore.selectedBaud}
                            >
                            <label class="form-check-label" for="baud115200">
                                115,200 bps &mdash; firmware &lt; 1.75.0
                            </label>
                        </div>

                        <button type="submit" class="btn btn-primary">Apply</button>
                    </div>
                </form>

                <Message text={serialStore.message} />

            </div>
        </div>
    </div>
</div>
