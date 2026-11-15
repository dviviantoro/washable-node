"use client";

import { useState } from "react";
import type { NodeInfo, Status } from "@/lib/api";
import { fmtAgo } from "@/lib/format";

export function OverviewPanel({ status, nodes, onOpenNodes }: { status: Status; nodes: NodeInfo[]; onOpenNodes: () => void }) {
  const [copied, setCopied] = useState(false);
  const n = status.nodes;
  const needAttention = nodes.filter((x) => x.online && (x.level === "warn" || x.level === "bad"));

  async function copy() {
    try {
      await navigator.clipboard.writeText(status.manifestUrl);
      setCopied(true);
      setTimeout(() => setCopied(false), 1500);
    } catch {
      window.prompt("Copy this URL", status.manifestUrl);
    }
  }

  return (
    <section className="stack">
      <div className="grid four">
        <div className="card stat"><span className="muted small">Nodes</span><span className="big">{n.total}</span></div>
        <div className="card stat"><span className="muted small">Online</span><span className="big ok-text">{n.online}</span></div>
        <div className="card stat"><span className="muted small">Need attention</span><span className={`big ${n.attention ? "warn-text" : ""}`}>{n.attention}</span></div>
        <div className="card stat"><span className="muted small">Updates pending</span><span className="big">{n.pendingUpdates}</span></div>
      </div>

      <div className="grid two">
        <div className="card stack">
          <div className="row between">
            <h2>Fleet</h2>
            <button className="btn small" onClick={onOpenNodes}>Open nodes</button>
          </div>
          {nodes.length === 0 ? (
            <p className="muted">No kiosk has reported in yet. Flash firmware ≥ 1.3.0 once and set the OTA URL on the right.</p>
          ) : needAttention.length === 0 && n.offline === 0 ? (
            <p className="alert ok">All {n.total} node{n.total > 1 ? "s are" : " is"} online and healthy.</p>
          ) : (
            <ul className="issuelist">
              {nodes.filter((x) => !x.online || x.level !== "ok").map((x) => (
                <li key={x.deviceId}>
                  <strong>{x.label || (x.machineId ? `Mesin ${x.machineId}` : x.deviceId)}</strong>
                  <span className={`badge ${!x.online ? "" : x.level === "bad" ? "bad" : "warn"}`}>{!x.online ? "offline" : x.level}</span>
                  <span className="muted small">{x.issues.join("; ")}{!x.online ? ` (last seen ${fmtAgo(x.lastBeat || x.lastSeen)})` : ""}</span>
                </li>
              ))}
            </ul>
          )}
        </div>

        <div className="card stack">
          <h2>Kiosk OTA URL</h2>
          <p className="muted small">Enter this in each kiosk (touch Settings or the PC web form, field “URL manifest OTA”). It never changes between releases.</p>
          <div className="copybox">
            <code className="mono">{status.manifestUrl}</code>
            <button className="btn" onClick={copy}>{copied ? "Copied" : "Copy"}</button>
          </div>
          {!status.manifestUrl.startsWith("https://") && (
            <p className="alert warn">Kiosks only accept <b>https://</b>. Open the dashboard through the tunnel URL, or set PUBLIC_URL on the server.</p>
          )}
          <p className="muted small">
            Kiosks send a heartbeat every 5 minutes and check for updates about 2 minutes after boot and every 6 hours. An update is installed only on the nodes you assign it to, when idle during the night window (01:00–05:00 WIB).
          </p>
        </div>
      </div>
    </section>
  );
}
