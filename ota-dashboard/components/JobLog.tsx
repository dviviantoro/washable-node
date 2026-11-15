"use client";

import { useEffect, useRef, useState } from "react";
import { ApiError, api, type JobInfo } from "@/lib/api";

/** Live log of a server-side job (firmware build or preview build): polls the offset-based log endpoint until the job ends. */
export function JobLog({ token, jobId, onDone, onUnauthorized }: { token: string; jobId: string | null; onDone?: (j: JobInfo) => void; onUnauthorized: () => void }) {
  const [log, setLog] = useState("");
  const [job, setJob] = useState<JobInfo | null>(null);
  const box = useRef<HTMLPreElement>(null);
  const stick = useRef(true);
  const done = useRef(onDone);
  done.current = onDone;

  useEffect(() => {
    setLog("");
    setJob(null);
    if (!jobId) return;
    let alive = true;
    let from = 0;
    let timer: ReturnType<typeof setTimeout>;
    const tick = async () => {
      try {
        const r = await api<{ job: JobInfo; log: string; next: number }>(`/api/jobs/${jobId}?from=${from}`, token);
        if (!alive) return;
        from = r.next;
        if (r.log) setLog((l) => (l + r.log).slice(-400_000));
        setJob(r.job);
        if (r.job.status === "running" || r.log.length >= 200_000) timer = setTimeout(tick, r.job.status === "running" ? 1000 : 50);
        else done.current?.(r.job);
      } catch (e) {
        if (e instanceof ApiError && e.status === 401) onUnauthorized();
        else if (alive) timer = setTimeout(tick, 3000);
      }
    };
    tick();
    return () => {
      alive = false;
      clearTimeout(timer);
    };
  }, [jobId, token, onUnauthorized]);

  useEffect(() => {
    const el = box.current;
    if (el && stick.current) el.scrollTop = el.scrollHeight;
  }, [log]);

  if (!jobId) return null;
  const running = job?.status === "running";
  return (
    <div className="stack tight">
      <div className="row between wrap">
        <span className="row">
          <strong>Build log</strong>
          {job && (
            <span className={`badge ${job.status === "ok" ? "ok" : job.status === "running" ? "warn" : "bad"}`}>
              {job.status === "ok" ? "Succeeded" : job.status === "running" ? "Running…" : job.status === "canceled" ? "Canceled" : "Failed"}
            </span>
          )}
        </span>
        {running && (
          <button className="btn small danger" onClick={() => api(`/api/jobs/${jobId}/cancel`, token, { method: "POST" }).catch(() => {})}>Cancel</button>
        )}
      </div>
      <pre
        className="log"
        ref={box}
        onScroll={(e) => {
          const el = e.currentTarget;
          stick.current = el.scrollHeight - el.scrollTop - el.clientHeight < 40;
        }}
      >{log || "Starting…"}</pre>
      {job && job.status !== "running" && job.status !== "ok" && job.error && <p className="alert error">{job.error}</p>}
    </div>
  );
}
