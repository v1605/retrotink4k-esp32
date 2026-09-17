<script lang="ts">
    let { text }: { text: string } = $props();
</script>

{#if text}
    <div class="alert alert-warning" role="alert">{text}</div>
{/if}
