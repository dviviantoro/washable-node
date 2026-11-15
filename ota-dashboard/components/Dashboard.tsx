"use client";

import { useCallback, useEffect, useState } from "react";
import { ApiError, api, type DeviceEvent, type Release, type Status } from "@/lib/api";
import { cmpVer } from "@/lib/format";
import { OverviewPanel } from "./OverviewPanel";
import { FleetPanel } from "./FleetPanel";
import { UploadPanel } from "./UploadPanel";
import { ReleasesPanel } from "./ReleasesPanel";
import { ActivityPanel } from "./ActivityPanel";

export function Dashboard({ token, onLogout }: { token: string; onLogout: () => void }) {
  const [status, setStatus] = useState<Status | null>(null);
  const [releases, setReleases] = useState<Release[]>([]);
  const [events, setEvents] = useState<DeviceEvent[]>([]);
  const [error, setError] = useState<string | null>(null);

  const refresh = useCallback(async () => {
    try {
      const [s, r, e] = await Promise.all([
        api<Status>("/api/status", token),
        api<Release[]>("/api/releases", token),
        api<DeviceEvent[]>("/api/events?limit=200", token),
      ]);
      setStatus(s);
      setReleases(r);
      setEvents(e);
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
      <main className="page">
        {error && <p className="alert error" role="alert">{error}</p>}
        {!status ? (
          <p className="muted">Loading…</p>
        ) : (
          <>
            <OverviewPanel status={status} onPause={() => act("/api/releases/deactivate")} />
            <FleetPanel status={status} />
            <UploadPanel token={token} status={status} releases={releases} onDone={refresh} onUnauthorized={onLogout} />
            <ReleasesPanel
              releases={releases}
              onActivate={(r) => {
                const cur = status.active;
                if (cur && r.version !== cur.version && cmpVer(r.version, cur.version) < 0) {
                  if (!window.confirm(`v${r.version} is OLDER than the active v${cur.version}.\n\nKiosks never downgrade: devices already on a newer version will ignore it. Only kiosks below v${r.version} will update. Continue?`)) return;
                } else if (!window.confirm(`Publish v${r.version} to all kiosks?\n\nEach kiosk installs it at night (01:00–05:00 WIB) when idle.`)) return;
                act(`/api/releases/${r.id}/activate`);
              }}
              onDelete={(r) => window.confirm(`Delete v${r.version}? The file is removed from the server.`) && act(`/api/releases/${r.id}`, { method: "DELETE" })}
            />
            <ActivityPanel events={events} />
          </>
        )}
      </main>
    </>
  );
}

