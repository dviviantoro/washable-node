"use client";

import type { Status } from "@/lib/api";
import { fmtAgo } from "@/lib/format";

export function FleetPanel({ status }: { status: Status }) {
  const versions = status.versions7d;
  const maxChecks = Math.max(1, ...status.daily.map((d) => d.checks));
  const total = versions.reduce((n, v) => n + v.sources, 0);

  return (
    <section className="grid two">
      <div className="card stack">
        <h2>Kiosks seen in the last 7 days</h2>
        {versions.length === 0 ? (
          <p className="muted">No kiosk has contacted the server yet. Set the OTA URL on a kiosk and wait for its next check.</p>
        ) : (
          <table>
            <thead>
              <tr><th>Running</th><th className="num">Sources</th><th className="num">Checks</th><th>Last seen</th></tr>
            </thead>
            <tbody>
              {versions.map((v) => (
                <tr key={v.version}>
                  <td>
                    <span className="mono">{v.version === "unknown" ? "unknown" : `v${v.version}`}</span>
                    {status.active && v.version === status.active.version && <span className="badge ok inline">current</span>}
                  </td>
                  <td className="num">{v.sources}</td>
                  <td className="num">{v.checks}</td>
                  <td title={v.last_seen}>{fmtAgo(v.last_seen)}</td>
                </tr>
              ))}
            </tbody>
          </table>
        )}
        <p className="muted small">
          “Sources” counts distinct public IP addresses ({total} total), so kiosks behind one router count once. Kiosks do not identify themselves yet.
        </p>
      </div>

      <div className="card stack">
        <h2>Activity, last 14 days</h2>
        {status.daily.length === 0 ? (
          <p className="muted">Nothing yet.</p>
        ) : (
          <div className="bars" role="img" aria-label={`Manifest checks per day, peak ${maxChecks}`}>
            {status.daily.map((d) => (
              <div className="bar-col" key={d.day} title={`${d.day}: ${d.checks} checks, ${d.downloads} completed downloads`}>
                <div className="bar" style={{ height: `${Math.max(4, (d.checks / maxChecks) * 100)}%` }} />
                <span className="bar-label">{d.day.slice(8)}</span>
              </div>
            ))}
          </div>
        )}
        <p className="muted small">
          Bars: manifest checks per day. Completed firmware downloads in this window:{" "}
          <b>{status.daily.reduce((n, d) => n + d.downloads, 0)}</b>.
        </p>
      </div>
    </section>
  );
}
