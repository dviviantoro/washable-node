"use client";

import type { Release } from "@/lib/api";
import { fmtAbs, fmtBytes } from "@/lib/format";

export function ReleasesPanel(props: {
  releases: Release[];
  onDeploy: (r: Release) => void;
  onDownload: (r: Release) => void;
  onDelete: (r: Release) => void;
}) {
  const { releases, onDeploy, onDownload, onDelete } = props;
  return (
    <section className="card stack">
      <h2>Releases</h2>
      {releases.length === 0 ? (
        <p className="muted">No firmware yet. Upload a .bin below, or build one in the Code tab.</p>
      ) : (
        <div className="scroll flat">
          <table className="cards">
            <thead>
              <tr><th>Version</th><th>Assigned to</th><th className="num">Size</th><th>SHA-256</th><th>Min.</th><th>Added</th><th /></tr>
            </thead>
            <tbody>
              {releases.map((r) => (
                <tr key={r.id}>
                  <td className="head wrap">
                    <span className="mono strong">v{r.version}</span>
                    {r.notes && <div className="muted small">{r.notes}</div>}
                  </td>
                  <td data-label="Assigned to">{r.nodes ? <span className="badge warn">{r.nodes} node{r.nodes > 1 ? "s" : ""}</span> : <span className="badge">nobody</span>}</td>
                  <td className="num" data-label="Size">{fmtBytes(r.size)}</td>
                  <td className="mono" data-label="SHA-256" title={r.sha256}>{r.sha256.slice(0, 10)}…</td>
                  <td data-label="Min. version">{r.min_version || "–"}</td>
                  <td data-label="Added">{fmtAbs(r.created_at)}</td>
                  <td className="act">
                    <div className="actions">
                      <button className="btn small primary" onClick={() => onDeploy(r)}>Send to nodes…</button>
                      <button className="btn small" onClick={() => onDownload(r)}>Download</button>
                      <button className="btn small danger" disabled={r.nodes > 0} title={r.nodes ? "Unassign it from the nodes first" : ""} onClick={() => onDelete(r)}>Delete</button>
                    </div>
                  </td>
                </tr>
              ))}
            </tbody>
          </table>
        </div>
      )}
      <p className="muted small">
        A release only reaches the nodes you pick on the Nodes tab. Kiosks install a version <b>newer</b> than the one they run, so to roll back a bad release, build the previous code with a <b>higher</b> version number.
      </p>
    </section>
  );
}
