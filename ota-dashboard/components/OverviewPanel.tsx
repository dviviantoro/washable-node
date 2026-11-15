"use client";

import { useState } from "react";
import type { Status } from "@/lib/api";
import { fmtAbs, fmtBytes } from "@/lib/format";

export function OverviewPanel({ status, onPause }: { status: Status; onPause: () => void }) {
  const [copied, setCopied] = useState(false);
  const rel = status.active;

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
    <section className="grid two">
      <div className="card stack">
        <div className="row between">
          <h2>Published release</h2>
          {rel ? <span className="badge ok">Live</span> : <span className="badge warn">Paused</span>}
        </div>
        {rel ? (
          <>
            <p className="big">v{rel.version}</p>
            <dl className="kv">
              <dt>Size</dt><dd>{fmtBytes(rel.size)}</dd>
              <dt>SHA-256</dt><dd className="mono" title={rel.sha256}>{rel.sha256.slice(0, 16)}…</dd>
              <dt>Min. version</dt><dd>{rel.min_version || "any"}</dd>
              <dt>Uploaded</dt><dd>{fmtAbs(rel.created_at)}</dd>
            </dl>
            <button className="btn" onClick={() => window.confirm("Pause the rollout? Kiosks keep the firmware they have and report “up to date”.") && onPause()}>Pause rollout</button>
          </>
        ) : (
          <p className="muted">No release is published. Kiosks are told they are up to date. Upload a firmware below and publish it.</p>
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
        <p className="muted small">Kiosks check about 2 minutes after boot and every 6 hours, and install only when idle during the night window.</p>
      </div>
    </section>
  );
}
