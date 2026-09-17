<script lang="ts">
    import { onMount, onDestroy } from "svelte";
    import { terminalStore } from "../lib/terminal.svelte";

    let terminalEl: HTMLPreElement;
    let commandValue = $state("");
    let commandEl: HTMLInputElement;

    onMount(() => {
        terminalStore.startPolling();
        commandEl.focus();
    });

    onDestroy(() => terminalStore.stopPolling());

    $effect(() => {
        terminalStore.logText;
        if (terminalEl)
            terminalEl.scrollTop = terminalEl.scrollHeight;
    });

    function onSubmit(event: SubmitEvent): void {
        event.preventDefault();

        const text = commandValue.trim();
        if (text.length === 0)
            return;

        terminalStore.send(text);
        commandValue = "";
        commandEl.focus();
    }
</script>

<div class="d-flex align-items-center gap-2 p-2 bg-body-tertiary border-bottom border-secondary">
    <span
        class="badge"
        class:text-bg-success={terminalStore.wsConnected}
        class:text-bg-danger={!terminalStore.wsConnected}
    >
        WebSocket: {terminalStore.wsConnected ? "connected" : "disconnected"}
    </span>
    <span
        class="badge"
        class:text-bg-success={terminalStore.ftdiConnected}
        class:text-bg-danger={!terminalStore.ftdiConnected}
    >
        RT4K: {terminalStore.ftdiConnected ? "connected" : "not connected"} ({terminalStore.baud} baud)
    </span>
</div>

<pre
    bind:this={terminalEl}
    class="flex-grow-1 overflow-auto min-h-0 p-2 mb-0 bg-black text-light font-monospace text-pre-wrap text-break"
    role="log"
    aria-live="polite"
    aria-label="Serial output"
>{terminalStore.logText}</pre>

<form class="p-2 bg-dark border-top border-secondary" onsubmit={onSubmit}>
    <div class="input-group">
        <input
            bind:this={commandEl}
            type="text"
            class="form-control font-monospace"
            placeholder="Enter command..."
            autocomplete="off"
            spellcheck="false"
            bind:value={commandValue}
        >
        <button type="submit" class="btn btn-primary">SEND</button>
        <button type="button" class="btn btn-secondary" onclick={() => terminalStore.clear()}>CLEAR</button>
    </div>
</form>
