"use client";

import { useRef, useState } from "react";
import { ApiError, uploadRelease, type Release, type Status } from "@/lib/api";
import { previewFirmware, type Preview } from "@/lib/firmware";
import { fmtBytes } from "@/lib/format";

export function UploadPanel(props: {
  token: string;
  status: Status;
  releases: Release[];
  onDone: () => void;
  onUnauthorized: () => void;
}) {
  const { token, status, releases, onDone, onUnauthorized } = props;
  const input = useRef<HTMLInputElement>(null);
  const [file, setFile] = useState<File | null>(null);
  const [preview, setPreview] = useState<Preview | null>(null);
  const [minVersion, setMinVersion] = useState("");
  const [notes, setNotes] = useState("");
  const [progress, setProgress] = useState<number | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [okMsg, setOkMsg] = useState<string | null>(null);
  const [drag, setDrag] = useState(false);

  async function pick(f: File | undefined) {
    setError(null);
    setOkMsg(null);
    setPreview(null);
    setFile(f ?? null);
    if (f) setPreview(await previewFirmware(f, status.limits));
  }

  function reset() {
    setFile(null);
    setPreview(null);
    setMinVersion("");
    setNotes("");
    setProgress(null);
    if (input.current) input.current.value = "";
  }

  const duplicate = preview?.version ? releases.some((r) => r.version === preview.version) : false;
  const canUpload = !!file && !!preview?.valid && !duplicate && progress === null;

  async function submit(e: React.FormEvent) {
    e.preventDefault();
    if (!file || !preview) return;
    setError(null);
    setProgress(0);
    try {
      const r = await uploadRelease(token, file, { version: preview.version ?? "", min_version: minVersion.trim(), notes: notes.trim() }, setProgress);
      setOkMsg(`v${r.version} uploaded. Pick the nodes that should get it on the Nodes tab.`);
      reset();
      onDone();
    } catch (err) {
      if (err instanceof ApiError && err.status === 401) onUnauthorized();
      setError(err instanceof Error ? err.message : "Upload failed");
      setProgress(null);
    }
  }

  return (
    <section className="card stack">
      <h2>Upload firmware</h2>
      <form className="stack" onSubmit={submit}>
        <div
          className={`drop ${drag ? "over" : ""}`}
          onDragOver={(e) => { e.preventDefault(); setDrag(true); }}
          onDragLeave={() => setDrag(false)}
          onDrop={(e) => { e.preventDefault(); setDrag(false); void pick(e.dataTransfer.files[0]); }}
          onClick={() => input.current?.click()}
          role="button"
          tabIndex={0}
          onKeyDown={(e) => (e.key === "Enter" || e.key === " ") && input.current?.click()}
        >
          <input ref={input} type="file" accept=".bin" hidden onChange={(e) => void pick(e.target.files?.[0])} />
          {file ? (
            <>
              <strong>{file.name}</strong>
              <span className="muted">{fmtBytes(file.size)} · click to choose another file</span>
            </>
          ) : (
            <>
              <strong>Drop firmware.bin here</strong>
              <span className="muted">from <code>.pio/build/esp32-3248s035/firmware.bin</code> or <code>dist/</code> after <code>tools/release.py</code></span>
            </>
          )}
        </div>

        {preview && (
          <div className="stack tight">
            <ul className="checks">
              {preview.checks.map((c) => (
                <li key={c.label} className={c.ok ? "ok" : "bad"}>
                  <span aria-hidden>{c.ok ? "✓" : "✕"}</span> {c.label}
                </li>
              ))}
              {duplicate && <li className="bad"><span aria-hidden>✕</span> v{preview.version} is already uploaded. Bump FW_VERSION and rebuild.</li>}
            </ul>
            {preview.sha256 && <p className="muted small mono">SHA-256 {preview.sha256}</p>}
          </div>
        )}

        <div className="grid two">
          <label className="field">
            <span>Version</span>
            <input value={preview?.version ? `v${preview.version}` : ""} placeholder="detected from the file" disabled />
          </label>
          <label className="field">
            <span>Minimum version <em>(optional)</em></span>
            <input value={minVersion} onChange={(e) => setMinVersion(e.target.value)} placeholder="e.g. 1.1.0" pattern="\d+\.\d+\.\d+" title="x.y.z" />
          </label>
        </div>
        <label className="field">
          <span>Release notes <em>(optional, stays on this server)</em></span>
          <textarea value={notes} onChange={(e) => setNotes(e.target.value)} rows={2} maxLength={500} />
        </label>

        {progress !== null && (
          <div className="progress" role="progressbar" aria-valuenow={progress} aria-valuemin={0} aria-valuemax={100}>
            <div style={{ width: `${progress}%` }} />
          </div>
        )}
        {error && <p className="alert error" role="alert">{error}</p>}
        {okMsg && <p className="alert ok" role="status">{okMsg}</p>}
        <div className="row">
          <button className="btn primary" disabled={!canUpload}>{progress !== null ? `Uploading ${progress}%` : "Upload"}</button>
          {file && progress === null && <button type="button" className="btn ghost" onClick={reset}>Clear</button>}
        </div>
      </form>
    </section>
  );
}
