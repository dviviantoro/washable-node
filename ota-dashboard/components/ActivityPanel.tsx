"use client";

import { useState } from "react";
import type { DeviceEvent } from "@/lib/api";
import { fmtAbs, fmtAgo } from "@/lib/format";

export function ActivityPanel({ events }: { events: DeviceEvent[] }) {
  const [kind, setKind] = useState<"all" | "manifest" | "firmware">("all");
  const rows = events.filter((e) => kind === "all" || e.kind === kind).slice(0, 60);

  return (
    <section className="card stack">
      <div className="row between wrap">
        <h2>Kiosk activity</h2>
        <div className="seg" role="group" aria-label="Filter">
          {(["all", "manifest", "firmware"] as const).map((k) => (
            <button key={k} className={kind === k ? "on" : ""} onClick={() => setKind(k)}>
              {k === "all" ? "All" : k === "manifest" ? "Checks" : "Downloads"}
            </button>
          ))}
        </div>
      </div>
      {rows.length === 0 ? (
        <p className="muted">No requests yet.</p>
      ) : (
        <div className="scroll">
          <table>
            <thead>
              <tr><th>When</th><th>Event</th><th>Kiosk runs</th><th>Server offered</th><th>Result</th><th>Source IP</th></tr>
            </thead>
            <tbody>
              {rows.map((e) => (
                <tr key={e.id}>
                  <td title={fmtAbs(e.ts)}>{fmtAgo(e.ts)}</td>
                  <td>{e.kind === "manifest" ? "Update check" : "Firmware download"}</td>
                  <td className="mono">{e.device_version === "unknown" ? "?" : `v${e.device_version}`}</td>
                  <td className="mono">{e.served_version === "none" ? "–" : `v${e.served_version}`}</td>
                  <td>
                    {e.kind === "firmware" ? (
                      e.status !== 200 ? <span className="badge bad">HTTP {e.status}</span> : e.complete ? <span className="badge ok">Complete</span> : <span className="badge warn">Interrupted</span>
                    ) : (
                      <span className="badge">HTTP {e.status}</span>
                    )}
                  </td>
                  <td className="mono">{e.ip}</td>
                </tr>
              ))}
            </tbody>
          </table>
        </div>
      )}
    </section>
  );
}
