"use client";

import type { Release } from "@/lib/api";
import { fmtAbs, fmtBytes } from "@/lib/format";

export function ReleasesPanel({ releases, onActivate, onDelete }: { releases: Release[]; onActivate: (r: Release) => void; onDelete: (r: Release) => void }) {
  return (
    <section className="card stack">
      <h2>Releases</h2>
      {releases.length === 0 ? (
        <p className="muted">No firmware uploaded yet.</p>
      ) : (
        <div className="scroll">
          <table>
            <thead>
              <tr><th>Version</th><th>Status</th><th className="num">Size</th><th>SHA-256</th><th>Min.</th><th>Uploaded</th><th /></tr>
            </thead>
            <tbody>
              {releases.map((r) => (
                <tr key={r.id}>
                  <td>
                    <span className="mono strong">v{r.version}</span>
                    {r.notes && <div className="muted small">{r.notes}</div>}
                  </td>
                  <td>{r.active ? <span className="badge ok">Live</span> : <span className="badge">Stored</span>}</td>
                  <td className="num">{fmtBytes(r.size)}</td>
                  <td className="mono" title={r.sha256}>{r.sha256.slice(0, 10)}…</td>
                  <td>{r.min_version || "–"}</td>
                  <td>{fmtAbs(r.created_at)}</td>
                  <td className="actions">
                    {!r.active && <button className="btn small" onClick={() => onActivate(r)}>Publish</button>}
                    {!r.active && <button className="btn small danger" onClick={() => onDelete(r)}>Delete</button>}
                  </td>
                </tr>
              ))}
            </tbody>
          </table>
        </div>
      )}
      <p className="muted small">
        Kiosks only install a version <b>newer</b> than the one they run. To roll back a bad release, build the previous code with a <b>higher</b> version number and publish that.
      </p>
    </section>
  );
}
