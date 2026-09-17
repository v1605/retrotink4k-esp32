<script lang="ts">
    import { onMount, onDestroy } from "svelte";
    import { osdStore } from "../lib/osd.svelte";
    import Message from "../components/Message.svelte";
    import HelpIcon from "../components/HelpIcon.svelte";

    const KEY_TO_BUTTON: Record<string, string> = {
        ArrowUp: "up",
        ArrowDown: "down",
        ArrowLeft: "left",
        ArrowRight: "right",
        Enter: "ok",
        " ": "menu",
        Backspace: "back",
    };

    let canvasEl: HTMLCanvasElement;

    function onPlaneChange(plane: 1 | 2): void {
        osdStore.setPlane(plane);
        osdStore.refresh();
    }

    // Also stops space scrolling the page and Backspace navigating back.
    function onKeyDown(event: KeyboardEvent): void {
        const button = KEY_TO_BUTTON[event.key];
        if (!button)
            return;

        event.preventDefault();
        osdStore.sendRemoteButton(button);
    }

    onMount(() => {
        osdStore.setCanvas(canvasEl);
        osdStore.refresh();
        window.addEventListener("keydown", onKeyDown);
    });

    onDestroy(() => {
        window.removeEventListener("keydown", onKeyDown);
        osdStore.setCanvas(null);
    });
</script>

<div class="flex-grow-1 overflow-auto d-flex flex-column min-h-0">
    <div class="container py-4 d-flex flex-column flex-grow-1 min-h-0">
        <div class="row justify-content-center flex-grow-1 min-h-0">
            <div class="col-12 col-lg-8 d-flex flex-column min-h-0">

                <div class="card bg-body-tertiary mb-3 d-flex flex-column flex-grow-1 min-h-0">
                    <div class="card-body d-flex flex-column min-h-0">
                        <div class="d-flex justify-content-between align-items-center mb-2">
                            <h5 class="card-title mb-0 d-flex align-items-center gap-2">
                                OSD snapshot
                                <HelpIcon
                                    text="Reconstructs the RT4K's on-screen display from its raw text-mode buffer -- no HDMI capture needed. This is a still snapshot, not a live feed; press Refresh to update it. Arrow keys, Enter, Space, and Backspace act as the remote's D-pad, OK, Menu, and Back buttons, and refresh the snapshot automatically."
                                />
                            </h5>
                            <div class="d-flex gap-1">
                                <button
                                    type="button"
                                    class="btn btn-sm"
                                    class:btn-primary={osdStore.plane === 1}
                                    class:btn-secondary={osdStore.plane !== 1}
                                    onclick={() => onPlaneChange(1)}
                                >
                                    Plane 1
                                </button>
                                <button
                                    type="button"
                                    class="btn btn-sm"
                                    class:btn-primary={osdStore.plane === 2}
                                    class:btn-secondary={osdStore.plane !== 2}
                                    onclick={() => onPlaneChange(2)}
                                >
                                    Plane 2
                                </button>
                                <button
                                    type="button"
                                    class="btn btn-sm btn-secondary"
                                    disabled={osdStore.loading}
                                    onclick={() => osdStore.refresh()}
                                >
                                    {osdStore.loading ? "Loading..." : "Refresh"}
                                </button>
                            </div>
                        </div>

                        {#if osdStore.plane === 2 && osdStore.available}
                            <p class="card-text text-body-secondary mb-2">
                                Menu {osdStore.on ? "on" : "off"}{osdStore.osk ? ", on-screen keyboard visible" : ""}
                            </p>
                        {/if}

                        <div
                            class="bg-black p-2 rounded d-flex justify-content-center align-items-center flex-grow-1 min-h-0 lh-0 overflow-hidden"
                        >
                            <canvas bind:this={canvasEl} class="h-100 w-auto img-pixelated"></canvas>
                        </div>

                        {#if !osdStore.available && !osdStore.loading}
                            <p class="card-text text-body-secondary mt-2 mb-0">
                                Nothing currently shown on the RT4K's OSD.
                            </p>
                        {/if}
                    </div>
                </div>

                <Message text={osdStore.message} />

            </div>
        </div>
    </div>
</div>
