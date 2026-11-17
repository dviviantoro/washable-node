"use client";

import { useCallback, useEffect, useState } from "react";
import { ApiError, api, downloadFile, type DeviceEvent, type NodeInfo, type NodeLog, type Release, type Status } from "@/lib/api";
import { OverviewPanel } from "./OverviewPanel";
import { NodesPanel } from "./NodesPanel";
import { UploadPanel } from "./UploadPanel";
import { ReleasesPanel } from "./ReleasesPanel";
import { ActivityPanel } from "./ActivityPanel";
import { StudioPanel } from "./StudioPanel";
import { PreviewPanel } from "./PreviewPanel";

type Tab = "overview" | "nodes" | "releases" | "code" | "preview" | "activity";
const TABS: { id: Tab; label: string }[] = [
  { id: "overview", label: "Overview" },
  { id: "nodes", label: "Nodes" },
  { id: "releases", label: "Releases" },
  { id: "code", label: "Code & build" },
  { id: "preview", label: "Kiosk preview" },
  { id: "activity", label: "Activity" },
];
const TAB_KEY = "washable-ota-tab";

export function Dashboard({ token, onLogout }: { token: string; onLogout: () => void }) {
  const [tab, setTabState] = useState<Tab>("overview");
  const [status, setStatus] = useState<Status | null>(null);
  const [releases, setReleases] = useState<Release[]>([]);
  const [nodes, setNodes] = useState<NodeInfo[]>([]);
  const [events, setEvents] = useState<DeviceEvent[]>([]);
  const [activity, setActivity] = useState<NodeLog[]>([]);
  const [deploy, setDeploy] = useState<Release | null>(null);
  const [error, setError] = useState<string | null>(null);

  const setTab = useCallback((t: Tab) => {
    setTabState(t);
    try { window.localStorage.setItem(TAB_KEY, t); } catch { /* ignore */ }
  }, []);
  useEffect(() => {
    try {
      const t = window.localStorage.getItem(TAB_KEY) as Tab | null;
      if (t && TABS.some((x) => x.id === t)) setTabState(t);
    } catch { /* ignore */ }
  }, []);

  useEffect(() => {
    // on a phone the tab strip scrolls sideways: bring the active tab into view (also after a reload)
    document.querySelector("nav.tabs button.on")?.scrollIntoView({ inline: "center", block: "nearest" });
  }, [tab, status !== null]);

  const refresh = useCallback(async () => {
    try {
      const [s, r, n, e, a] = await Promise.all([
        api<Status>("/api/status", token),
        api<Release[]>("/api/releases", token),
        api<NodeInfo[]>("/api/nodes", token),
        api<DeviceEvent[]>("/api/events?limit=200", token),
        api<NodeLog[]>("/api/activity?limit=100", token),
      ]);
      setStatus(s);
      setReleases(r);
      setNodes(n);
      setEvents(e);
      setActivity(a);
      setError(null);
    } catch (err) {
      if (err instanceof ApiError && err.status === 401) onLogout();
      else setError(err instanceof Error ? err.message : "Failed to load");
    }
  }, [token, onLogout]);

  useEffect(() => {
    refresh();
    const t = setInterval(refresh, 15_000);
    return () => clearInterval(t);
  }, [refresh]);

  async function act(path: string, init: RequestInit = { method: "POST" }) {
    try {
      await api(path, token, init);
      await refresh();
    } catch (err) {
      if (err instanceof ApiError && err.status === 401) onLogout();
      else window.alert(err instanceof Error ? err.message : "Request failed");
    }
  }

  const attention = status?.nodes.attention ?? 0;

  return (
    <>
      <header className="topbar">
        <div className="brand">
          <span className="logo" aria-hidden>↻</span>
          <strong>Washable OTA</strong>
          {status && <span className="muted small">server v{status.serverVersion}</span>}
        </div>
        <button className="btn ghost" onClick={onLogout}>Sign out</button>
      </header>
      <nav className="tabs" aria-label="Sections">
        {TABS.map((t) => (
          <button key={t.id} className={tab === t.id ? "on" : ""} aria-current={tab === t.id ? "page" : undefined} onClick={() => setTab(t.id)}>
            {t.label}
            {t.id === "nodes" && status && status.nodes.total > 0 && <span className={`pill ${attention ? "warn" : ""}`}>{status.nodes.online}/{status.nodes.total}</span>}
          </button>
        ))}
      </nav>
      <main className={`page ${tab === "code" ? "wide" : ""}`}>
        {error && <p className="alert error" role="alert">{error}</p>}
        {!status ? (
          <p className="muted">Loading…</p>
        ) : (
          <>
            {tab === "overview" && <OverviewPanel status={status} nodes={nodes} onOpenNodes={() => setTab("nodes")} />}
            {tab === "nodes" && <NodesPanel token={token} nodes={nodes} releases={releases} preselect={deploy} onChanged={refresh} onUnauthorized={onLogout} />}
            {tab === "releases" && (
              <>
                <ReleasesPanel
                  releases={releases}
                  onDeploy={(r) => { setDeploy(r); setTab("nodes"); }}
                  onDownload={(r) => downloadFile(token, `/api/releases/${r.id}/firmware.bin`, `firmware-${r.version}.bin`).catch((e) => window.alert(e instanceof Error ? e.message : "Download failed"))}
                  onDelete={(r) => window.confirm(`Delete v${r.version}? The file is removed from the server.`) && act(`/api/releases/${r.id}`, { method: "DELETE" })}
                />
                <UploadPanel token={token} status={status} releases={releases} onDone={refresh} onUnauthorized={onLogout} />
              </>
            )}
            {tab === "code" && <StudioPanel token={token} releases={releases} onReleased={refresh} onUnauthorized={onLogout} onStudioChanged={() => {}} />}
            {tab === "preview" && <PreviewPanel token={token} onUnauthorized={onLogout} onStudioChanged={() => {}} />}
            {tab === "activity" && <ActivityPanel events={events} activity={activity} nodes={nodes} />}
          </>
        )}
      </main>
    </>
  );
}
