<script lang="ts">
    import { onMount } from "svelte";
    import { faFile, faFolder } from "@fortawesome/free-solid-svg-icons";
    import { profilesStore } from "../lib/profiles.svelte";
    import Message from "../components/Message.svelte";
    import Icon from "../components/Icon.svelte";

    let fileInputEl: HTMLInputElement;

    function onFileChange(): void {
        profilesStore.setUploadFile(fileInputEl.files?.[0] ?? null);
    }

    function onUpload(): void {
        profilesStore.upload(() => (fileInputEl.value = ""));
    }

    function onDelete(name: string): void {
        if (confirm("Delete " + name + "? This can't be undone."))
            profilesStore.delete(name);
    }

    onMount(() => {
        profilesStore.refreshCurrentProfile();
        profilesStore.refresh();
    });
</script>

<div class="flex-grow-1 overflow-auto min-h-0">
    <div class="container py-4">
        <div class="row justify-content-center">
            <div class="col-12 col-sm-10 col-md-8 col-lg-6">

                <div class="card bg-body-tertiary mb-3">
                    <div class="card-body">
                        <h5 class="card-title">Currently loaded profile</h5>
                        <p class="card-text mb-0">{profilesStore.currentProfile}</p>
                    </div>
                </div>

                <div class="card bg-body-tertiary mb-3">
                    <div class="card-body">
                        <div class="d-flex justify-content-between align-items-center mb-2">
                            <h5 class="card-title mb-0">SD card: /profile</h5>
                            <button type="button" class="btn btn-sm btn-secondary" onclick={() => profilesStore.refresh()}>
                                Refresh
                            </button>
                        </div>

                        <nav aria-label="breadcrumb">
                            <ol class="breadcrumb mb-2">
                                {#each profilesStore.breadcrumb as crumb (crumb.path)}
                                    <li
                                        class="breadcrumb-item"
                                        class:active={crumb.isLast}
                                        aria-current={crumb.isLast ? "page" : undefined}
                                    >
                                        {#if crumb.isLast}
                                            {crumb.label}
                                        {:else}
                                            <button
                                                type="button"
                                                class="btn btn-link p-0 align-baseline text-decoration-none"
                                                onclick={() => profilesStore.goToBreadcrumb(crumb.path)}
                                            >{crumb.label}</button>
                                        {/if}
                                    </li>
                                {/each}
                            </ol>
                        </nav>

                        <ul class="list-group">
                            {#if profilesStore.navigating}
                                <li class="list-group-item text-body-secondary">Loading...</li>
                            {:else if profilesStore.visibleEntries.length === 0}
                                <li class="list-group-item text-body-secondary">This folder is empty.</li>
                            {:else}
                                {#each profilesStore.visibleEntries as entry (entry.name)}
                                    <li class="list-group-item d-flex justify-content-between align-items-center gap-2">
                                        {#if entry.isDirectory}
                                            <button
                                                type="button"
                                                class="btn btn-link p-0 align-baseline text-decoration-none"
                                                onclick={() => profilesStore.openDirectory(entry.name)}
                                            ><Icon icon={faFolder} size={14} /> {entry.name}</button>
                                        {:else}
                                            <span><Icon icon={faFile} size={14} /> {entry.name}</span>

                                            <div class="d-flex gap-2">
                                                <button
                                                    type="button"
                                                    class="btn btn-sm btn-primary"
                                                    disabled={profilesStore.loadingName === profilesStore.relativeName(entry.name)}
                                                    onclick={() => profilesStore.load(profilesStore.relativeName(entry.name))}
                                                >
                                                    {profilesStore.loadingName === profilesStore.relativeName(entry.name) ? "Loading..." : "Load"}
                                                </button>
                                                <button
                                                    type="button"
                                                    class="btn btn-sm btn-secondary"
                                                    disabled={profilesStore.downloadingName === profilesStore.relativeName(entry.name)}
                                                    onclick={() => profilesStore.download(profilesStore.relativeName(entry.name))}
                                                >
                                                    {profilesStore.downloadingName === profilesStore.relativeName(entry.name) ? "Downloading..." : "Download"}
                                                </button>
                                                <button
                                                    type="button"
                                                    class="btn btn-sm btn-outline-danger"
                                                    disabled={profilesStore.deletingName === profilesStore.relativeName(entry.name)}
                                                    onclick={() => onDelete(profilesStore.relativeName(entry.name))}
                                                >
                                                    {profilesStore.deletingName === profilesStore.relativeName(entry.name) ? "Deleting..." : "Delete"}
                                                </button>
                                            </div>
                                        {/if}
                                    </li>
                                {/each}
                            {/if}
                        </ul>
                    </div>
                </div>

                <div class="card bg-body-tertiary mb-3">
                    <div class="card-body">
                        <h5 class="card-title">Upload a profile</h5>
                        <input
                            bind:this={fileInputEl}
                            type="file"
                            class="form-control"
                            accept=".rt4,.rt6"
                            onchange={onFileChange}
                        >
                        <button
                            type="button"
                            class="btn btn-primary mt-2"
                            disabled={!profilesStore.uploadFile || profilesStore.uploading}
                            onclick={onUpload}
                        >
                            {profilesStore.uploading ? "Uploading..." : "Upload"}
                        </button>
                    </div>
                </div>

                <Message text={profilesStore.message} />

            </div>
        </div>
    </div>
</div>
