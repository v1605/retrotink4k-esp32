<script lang="ts">
    // A @fortawesome/free-solid-svg-icons icon (duotone paths are arrays).
    type FaIcon = { icon: [number, number, unknown, unknown, string | string[]] };

    let { icon, size = 16 }: { icon: FaIcon; size?: number } = $props();

    const parsed = $derived.by(() => {
        const [width, height, , , path] = icon.icon;
        return { width, height, paths: Array.isArray(path) ? path : [path] };
    });
</script>

<svg viewBox="0 0 {parsed.width} {parsed.height}" width={size} height={size} fill="currentColor" aria-hidden="true">
    {#each parsed.paths as d (d)}
        <path {d} />
    {/each}
</svg>
