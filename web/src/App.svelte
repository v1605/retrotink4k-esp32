<script lang="ts">
    import { onMount } from "svelte";
    import type { Component } from "svelte";
    import Terminal from "./views/Terminal.svelte";
    import Wifi from "./views/Wifi.svelte";
    import Serial from "./views/Serial.svelte";
    import Profiles from "./views/Profiles.svelte";
    import Osd from "./views/Osd.svelte";
    import Firmware from "./views/Firmware.svelte";
    import Ota from "./views/Ota.svelte";
    import { getStatus } from "./lib/api";

    interface Route {
        name: string;
        label: string;
        component: Component;
    }

    const BASE_ROUTES: Route[] = [
        { name: "terminal", label: "Terminal", component: Terminal },
        { name: "wifi", label: "WiFi", component: Wifi },
        { name: "serial", label: "Serial", component: Serial },
        { name: "profiles", label: "Profiles", component: Profiles },
        { name: "osd", label: "OSD", component: Osd },
        { name: "firmware", label: "Firmware", component: Firmware },
    ];

    const OTA_ROUTE: Route = { name: "ota", label: "Update", component: Ota };

    // Routes always resolve; only the nav link waits on the support check.
    const ALL_ROUTES = [...BASE_ROUTES, OTA_ROUTE];

    let otaSupported = $state(false);

    onMount(() => {
        getStatus()
            .then((info) => (otaSupported = info.ota_supported))
            .catch(() => {});
    });

    const navRoutes = $derived(otaSupported ? ALL_ROUTES : BASE_ROUTES);

    const DEFAULT_ROUTE = "terminal";

    // First hash segment is the route; the rest is view state ("#profiles/foo").
    function currentRouteName(): string {
        const hash = location.hash.replace(/^#\/?/, "").split(/[/?]/)[0];
        return ALL_ROUTES.some((r) => r.name === hash) ? hash : DEFAULT_ROUTE;
    }

    let routeName = $state(currentRouteName());

    function onHashChange(): void {
        routeName = currentRouteName();
    }

    $effect(() => {
        window.addEventListener("hashchange", onHashChange);
        return () => window.removeEventListener("hashchange", onHashChange);
    });

    const activeRoute = $derived(ALL_ROUTES.find((r) => r.name === routeName) ?? ALL_ROUTES[0]);
    const ActiveView = $derived(activeRoute.component);
</script>

<nav class="navbar navbar-expand navbar-dark bg-dark border-bottom border-secondary">
    <div class="container-fluid">
        <span class="navbar-brand mb-0 h1">RT4K Serial Bridge</span>
        <ul class="navbar-nav">
            {#each navRoutes as route (route.name)}
                <li class="nav-item">
                    <a
                        class="nav-link"
                        class:active={route.name === routeName}
                        aria-current={route.name === routeName ? "page" : undefined}
                        href="#{route.name}"
                    >
                        {route.label}
                    </a>
                </li>
            {/each}
        </ul>
    </div>
</nav>

<main class="d-flex flex-column flex-grow-1 min-h-0">
    <ActiveView />
</main>
