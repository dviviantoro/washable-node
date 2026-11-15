"use client";

import { useEffect, useMemo, useState } from "react";
import { ApiError, api, type NodeDetail, type NodeInfo, type Release } from "@/lib/api";
import { cmpVer, fmtAbs, fmtAgo, fmtBytes, fmtUptime } from "@/lib/format";

const nodeName = (n: NodeInfo) => n.label || (n.machineId ? `Mesin ${n.machineId}` : n.deviceId);

function StateBadge({ n }: { n: NodeInfo }) {
  if (!n.online) return <span className="badge" title={n.issues.join("; ")}>Offline</span>;
  if (n.level === "bad") return <span className="badge bad" title={n.issues.join("; ")}>Problem</span>;
  if (n.level === "warn") return <span className="badge warn" title={n.issues.join("; ")}>Attention</span>;
  return <span className="badge ok">Healthy</span>;
}

function UpdateCell({ n }: { n: NodeInfo }) {
  if (!n.target) return <span className="muted">–</span>;
  if (n.update === "done") return <span className="badge ok">v{n.target.version} installed</span>;
  return <span className="badge warn" title={`Assigned ${fmtAbs(n.target.setAt)}`}>→ v{n.target.version} pending</span>;
}

export function NodesPanel(props: {
  token: string;
  nodes: NodeInfo[];
  releases: Release[];
  preselect: Release | null;
  onChanged: () => void;
  onUnauthorized: () => void;
}) {
  const { token, nodes, releases, preselect, onChanged, onUnauthorized } = props;
  const [selected, setSelected] = useState<Set<string>>(new Set());
  const [releaseId, setReleaseId] = useState<number | "">("");
  const [msg, setMsg] = useState<{ kind: "ok" | "error" | "warn"; text: string } | null>(null);
  const [detailId, setDetailId] = useState<string | null>(null);
  const [busy, setBusy] = useState(false);

  useEffect(() => {
    if (preselect) setReleaseId(preselect.id);
  }, [preselect]);
  useEffect(() => {
    if (releaseId === "" && releases.length) setReleaseId(releases[0]!.id);
  }, [releases, releaseId]);

  const release = releases.find((r) => r.id === releaseId);
  const allIds = nodes.map((n) => n.deviceId);
  const eligible = (n: NodeInfo) => !release || !n.fwVersion || cmpVer(release.version, n.fwVersion) > 0;

  const versions = useMemo(() => {
    const m = new Map<string, number>();
    for (const n of nodes) m.set(n.fwVersion || "unknown", (m.get(n.fwVersion || "unknown") ?? 0) + 1);
    return [...m.entries()].sort((a, b) => cmpVer(b[0], a[0]));
  }, [nodes]);

  function toggle(id: string) {
    setSelected((s) => {
      const n = new Set(s);
      if (n.has(id)) n.delete(id);
      else n.add(id);
      return n;
    });
  }

  async function assign(rel: Release | null) {
    const ids = [...selected].filter((id) => allIds.includes(id));
    if (!ids.length) return;
    const names = ids.map((id) => nodeName(nodes.find((n) => n.deviceId === id)!)).join(", ");
    const q = rel
      ? `Send v${rel.version} to ${ids.length} selected node${ids.length > 1 ? "s" : ""} (${names}) only?\n\nEach node installs it by itself at night (01:00–05:00 WIB) when idle. Other nodes are not affected.`
      : `Cancel the pending update on ${ids.length} node${ids.length > 1 ? "s" : ""} (${names})?`;
    if (!window.confirm(q)) return;
    setBusy(true);
    setMsg(null);
    try {
      const r = await api<{ results: { deviceId: string; ok: boolean; error?: string }[] }>("/api/nodes/target", token, {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ releaseId: rel ? rel.id : null, deviceIds: ids }),
      });
      const failed = r.results.filter((x) => !x.ok);
      if (failed.length) {
        const text = failed.map((f) => `${nodeName(nodes.find((n) => n.deviceId === f.deviceId) ?? ({ deviceId: f.deviceId, label: "", machineId: "" } as NodeInfo))}: ${f.error}`).join(" · ");
        setMsg({ kind: failed.length === r.results.length ? "error" : "warn", text: `${r.results.length - failed.length} assigned, ${failed.length} skipped — ${text}` });
      } else {
        setMsg({ kind: "ok", text: rel ? `v${rel.version} assigned to ${ids.length} node${ids.length > 1 ? "s" : ""}. They pick it up on their next check.` : "Update cancelled." });
        setSelected(new Set());
      }
      onChanged();
    } catch (err) {
      if (err instanceof ApiError && err.status === 401) onUnauthorized();
      setMsg({ kind: "error", text: err instanceof Error ? err.message : "Failed" });
    } finally {
      setBusy(false);
    }
  }

  return (
    <section className="stack">
      <div className="card stack">
        <div className="row between wrap">
          <h2>Nodes</h2>
          <span className="muted small">
            {versions.map(([v, c]) => (
              <span key={v} className="badge inline">{c}× {v === "unknown" ? v : `v${v}`}</span>
            ))}
          </span>
        </div>

        {nodes.length === 0 ? (
          <p className="muted">
            No kiosk has reported in yet. A kiosk appears here after it contacts this server with its device ID, which needs firmware ≥ 1.3.0 (telemetry) and this server’s URL in its OTA setting.
          </p>
        ) : (
          <div className="scroll">
            <table>
              <thead>
                <tr>
                  <th><input type="checkbox" aria-label="Select all" checked={selected.size === nodes.length} onChange={(e) => setSelected(e.target.checked ? new Set(allIds) : new Set())} /></th>
                  <th>Node</th><th>Status</th><th>Firmware</th><th>Update</th><th>Activity</th><th>Wi-Fi</th><th>Up</th><th>Last seen</th>
                </tr>
              </thead>
              <tbody>
                {nodes.map((n) => (
                  <tr key={n.deviceId} className={detailId === n.deviceId ? "sel" : ""}>
                    <td><input type="checkbox" aria-label={`Select ${nodeName(n)}`} checked={selected.has(n.deviceId)} onChange={() => toggle(n.deviceId)} /></td>
                    <td>
                      <button className="link" onClick={() => setDetailId(detailId === n.deviceId ? null : n.deviceId)}>{nodeName(n)}</button>
                      <div className="muted small mono">{n.deviceId}</div>
                    </td>
                    <td><StateBadge n={n} /></td>
                    <td className="mono">{n.fwVersion ? `v${n.fwVersion}` : "–"}</td>
                    <td><UpdateCell n={n} /></td>
                    <td>{n.online && n.health ? <>{n.health.activity}<div className="muted small">{n.health.screen}</div></> : <span className="muted">–</span>}</td>
                    <td>{n.online && n.health ? `${n.health.rssi} dBm` : "–"}</td>
                    <td>{n.online && n.health ? fmtUptime(n.health.up) : "–"}</td>
                    <td title={fmtAbs(n.lastSeen)}>{fmtAgo(n.lastBeat || n.lastSeen)}</td>
                  </tr>
                ))}
              </tbody>
            </table>
          </div>
        )}

        {nodes.length > 0 && (
          <div className="assign">
            <div className="row wrap">
              <strong>{selected.size} selected</strong>
              <select value={releaseId} onChange={(e) => setReleaseId(e.target.value ? Number(e.target.value) : "")} aria-label="Release to send">
                {releases.length === 0 && <option value="">No releases yet</option>}
                {releases.map((r) => <option key={r.id} value={r.id}>v{r.version} · {fmtBytes(r.size)}</option>)}
              </select>
              <button className="btn primary" disabled={busy || !release || selected.size === 0} onClick={() => release && assign(release)}>
                Send to selected
              </button>
              <button className="btn" disabled={busy || selected.size === 0} onClick={() => assign(null)}>Cancel pending update</button>
            </div>
            {release && selected.size > 0 && [...selected].some((id) => { const n = nodes.find((x) => x.deviceId === id); return n && !eligible(n); }) && (
              <p className="muted small">Nodes already on v{release.version} or newer will be skipped: kiosks never downgrade or reinstall.</p>
            )}
            <p className="muted small">Nothing is ever sent to every kiosk at once: only the nodes ticked above get the release.</p>
          </div>
        )}
        {msg && <p className={`alert ${msg.kind}`} role="status">{msg.text}</p>}
      </div>

      {detailId && nodes.some((n) => n.deviceId === detailId) && (
        <NodeDetailCard token={token} id={detailId} onClose={() => setDetailId(null)} onChanged={onChanged} onUnauthorized={onUnauthorized} />
      )}
    </section>
  );
}

function NodeDetailCard(props: { token: string; id: string; onClose: () => void; onChanged: () => void; onUnauthorized: () => void }) {
  const { token, id, onClose, onChanged, onUnauthorized } = props;
  const [d, setD] = useState<NodeDetail | null>(null);
  const [label, setLabel] = useState("");
  const [err, setErr] = useState<string | null>(null);

  useEffect(() => {
    let alive = true;
    const load = async () => {
      try {
        const r = await api<NodeDetail>(`/api/nodes/${id}`, token);
        if (!alive) return;
        setD(r);
        setLabel((cur) => (cur === "" && !d ? r.node.label : cur));
      } catch (e) {
        if (e instanceof ApiError && e.status === 401) onUnauthorized();
        else if (alive) setErr(e instanceof Error ? e.message : "Failed to load");
      }
    };
    load();
    const t = setInterval(load, 10_000);
    return () => { alive = false; clearInterval(t); };
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [id, token]);

  if (!d) return <section className="card"><p className="muted">{err ?? "Loading…"}</p></section>;
  const n = d.node;
  const h = n.health;

  async function saveLabel() {
    try {
      await api(`/api/nodes/${id}`, token, { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify({ label }) });
      onChanged();
    } catch (e) {
      setErr(e instanceof Error ? e.message : "Failed");
    }
  }
  async function remove() {
    if (!window.confirm(`Forget ${nodeName(n)}? It registers itself again the next time it contacts the server.`)) return;
    try {
      await api(`/api/nodes/${id}`, token, { method: "DELETE" });
      onClose();
      onChanged();
    } catch (e) {
      setErr(e instanceof Error ? e.message : "Failed");
    }
  }

  return (
    <section className="card stack">
      <div className="row between wrap">
        <h2>{nodeName(n)} <StateBadge n={n} /></h2>
        <button className="btn ghost small" onClick={onClose}>Close</button>
      </div>
      {n.issues.length > 0 && <ul className="alert warn issues">{n.issues.map((i) => <li key={i}>{i}</li>)}</ul>}
      <div className="grid two">
        <dl className="kv">
          <dt>Device ID</dt><dd className="mono">{n.deviceId}</dd>
          <dt>Machine</dt><dd>{n.machineId || "–"}</dd>
          <dt>Firmware</dt><dd>{n.fwVersion ? `v${n.fwVersion}` : "unknown"}</dd>
          <dt>Update</dt><dd><UpdateCell n={n} /></dd>
          <dt>First seen</dt><dd>{fmtAbs(n.firstSeen)}</dd>
          <dt>Last contact</dt><dd>{fmtAbs(n.lastSeen)} <span className="muted small">({n.lastIp})</span></dd>
          <dt>Restarts seen</dt><dd>{n.boots}</dd>
        </dl>
        <dl className="kv">
          <dt>Screen</dt><dd>{h?.screen ?? "–"}</dd>
          <dt>Activity</dt><dd>{h?.activity ?? "–"}</dd>
          <dt>Uptime</dt><dd>{h ? fmtUptime(h.up) : "–"}</dd>
          <dt>Last reset</dt><dd>{h?.resetReason ?? "–"}</dd>
          <dt>Wi-Fi signal</dt><dd>{h ? `${h.rssi} dBm` : "–"}</dd>
          <dt>Memory</dt><dd>{h ? `${fmtBytes(h.heap)} free · largest block ${fmtBytes(h.maxblk)}` : "–"}</dd>
          <dt>Relay controller</dt><dd>{h ? (h.relay ? `connected (fw ${h.relayfw || "?"}, last reset ${h.relayrst || "?"})` : "not responding") : "–"}</dd>
          <dt>OTA client</dt><dd>{h ? `${h.otaState}${h.latest ? `, newest seen v${h.latest}` : ""}` : "–"}</dd>
        </dl>
      </div>

      <div className="row wrap">
        <input type="text" value={label} maxLength={40} placeholder="Label (e.g. Warung Budi)" onChange={(e) => setLabel(e.target.value)} aria-label="Label" style={{ maxWidth: 280 }} />
        <button className="btn" onClick={saveLabel}>Save label</button>
        <button className="btn danger" onClick={remove}>Forget node</button>
      </div>
      {err && <p className="alert error">{err}</p>}

      <h2>Timeline</h2>
      {d.log.length === 0 ? <p className="muted">Nothing yet.</p> : (
        <ul className="timeline">
          {d.log.map((l) => (
            <li key={l.id}>
              <span className="muted small" title={fmtAbs(l.ts)}>{fmtAgo(l.ts)}</span>
              <span className={`badge ${l.kind === "crash" || l.kind === "download-failed" ? "bad" : l.kind === "assigned" || l.kind === "firmware" || l.kind === "download" ? "ok" : ""}`}>{l.kind}</span>
              <span>{l.detail}</span>
            </li>
          ))}
        </ul>
      )}
    </section>
  );
}
