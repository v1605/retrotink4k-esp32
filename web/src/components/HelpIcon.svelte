<script lang="ts">
    import { faCircleQuestion } from "@fortawesome/free-solid-svg-icons";
    import Icon from "./Icon.svelte";

    let { text }: { text: string } = $props();

    let open = $state(false);
    let rootEl: HTMLElement;

    const popupId = $props.id();

    function onDocumentClick(event: MouseEvent): void {
        if (rootEl && !rootEl.contains(event.target as Node))
            open = false;
    }

    $effect(() => {
        if (!open)
            return;

        document.addEventListener("click", onDocumentClick);
        return () => document.removeEventListener("click", onDocumentClick);
    });
</script>

<span class="position-relative d-inline-block" bind:this={rootEl}>
    <button
        type="button"
        class="btn btn-link p-0 text-body-secondary align-middle lh-1"
        aria-label="More info"
        aria-expanded={open}
        aria-controls={popupId}
        onclick={() => (open = !open)}
    >
        <Icon icon={faCircleQuestion} size={15} />
    </button>

    {#if open}
        <div
            id={popupId}
            role="tooltip"
            class="position-absolute top-100 start-0 mt-1 p-2 rounded shadow-sm bg-body-tertiary border small text-body w-max-content mw-sm z-3"
        >
            {text}
        </div>
    {/if}
</span>
