"use client";

import dynamic from "next/dynamic";
import { loader } from "@monaco-editor/react";
import { useCallback, useEffect, useMemo, useRef, useState } from "react";
import { ApiError, api, downloadFile, type FileEntry, type JobInfo, type Release, type StudioStatus } from "@/lib/api";
import { fmtBytes } from "@/lib/format";
import { JobLog } from "./JobLog";

loader.config({ paths: { vs: "https://cdn.jsdelivr.net/npm/monaco-editor@0.52.2/min/vs" } });
const Editor = dynamic(() => import("@monaco-editor/react").then((m) => m.Editor), { ssr: false, loading: () => <p className="muted pad">Loading editor…</p> });
const DiffEditor = dynamic(() => import("@monaco-editor/react").then((m) => m.DiffEditor), { ssr: false, loading: () => <p className="muted pad">Loading editor…</p> });

interface OpenFile {
  path: string;
  content: string; // what is on screen
  saved: string; // what the server has
  base: string | null; // pristine copy from the repo
}

function useDark(): boolean {
  const [dark, setDark] = useState(false);
  useEffect(() => {
    const mq = window.matchMedia("(prefers-color-scheme: dark)");
    const set = () => setDark(mq.matches);
    set();
    mq.addEventListener("change", set);
    return () => mq.removeEventListener("change", set);
  }, []);
  return dark;
}

export function StudioPanel(props: { token: string; releases: Release[]; onReleased: () => void; onUnauthorized: () => void; onStudioChanged: () => void }) {
  const { token, releases, onReleased, onUnauthorized, onStudioChanged } = props;
  const dark = useDark();
  const [files, setFiles] = useState<FileEntry[]>([]);
  const [open, setOpen] = useState<OpenFile | null>(null);
  const [diff, setDiff] = useState(false);
  const [studio, setStudio] = useState<StudioStatus | null>(null);
  const [jobId, setJobId] = useState<string | null>(null);
  const [finished, setFinished] = useState<JobInfo | null>(null);
  const [notes, setNotes] = useState("");
  const [minVersion, setMinVersion] = useState("");
  const [msg, setMsg] = useState<{ kind: "ok" | "error"; text: string } | null>(null);
  const [busy, setBusy] = useState(false);
  const openRef = useRef(open);
  openRef.current = open;

  const fail = useCallback((e: unknown) => {
    if (e instanceof ApiError && e.status === 401) onUnauthorized();
    else setMsg({ kind: "error", text: e instanceof Error ? e.message : "Request failed" });
  }, [onUnauthorized]);

  const loadFiles = useCallback(async () => {
    try {
      setFiles((await api<{ files: FileEntry[] }>("/api/workspace/files", token)).files);
    } catch (e) {
      fail(e);
    }
  }, [token, fail]);

  const loadStudio = useCallback(async () => {
    try {
      const s = await api<StudioStatus>("/api/studio", token);
      setStudio(s);
      return s;
    } catch (e) {
      fail(e);
    }
  }, [token, fail]);

  useEffect(() => {
    loadFiles();
    loadStudio().then((s) => {
      if (s?.busy?.kind === "firmware") setJobId(s.busy.id);
      else if (s?.lastFirmwareJob) { setJobId(s.lastFirmwareJob.id); setFinished(s.lastFirmwareJob); }
    });
  }, [loadFiles, loadStudio]);

  useEffect(() => {
    if (open || !files.length) return;
    const main = files.find((f) => f.path === "esp32-kiosk/src/main.cpp") ?? files[0]!;
    void openFile(main.path);
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [files]);

  async function openFile(p: string) {
    const cur = openRef.current;
    if (cur && cur.content !== cur.saved && cur.path !== p && !window.confirm(`Discard unsaved changes in ${cur.path}?`)) return;
    try {
      const r = await api<{ path: string; content: string; base: string | null }>(`/api/workspace/file?path=${encodeURIComponent(p)}`, token);
      setOpen({ path: r.path, content: r.content, saved: r.content, base: r.base });
      setMsg(null);
    } catch (e) {
      fail(e);
    }
  }

  const save = useCallback(async () => {
    const cur = openRef.current;
    if (!cur || cur.content === cur.saved) return;
    try {
      await api(`/api/workspace/file`, token, { method: "PUT", headers: { "Content-Type": "application/json" }, body: JSON.stringify({ path: cur.path, content: cur.content }) });
      setOpen((o) => (o && o.path === cur.path ? { ...o, saved: cur.content } : o));
      setMsg({ kind: "ok", text: `Saved ${cur.path}` });
      loadFiles();
    } catch (e) {
      fail(e);
    }
  }, [token, fail, loadFiles]);

  async function revert() {
    if (!open || !window.confirm(`Restore ${open.path} to the repository version? Your edits to it are lost.`)) return;
    try {
      await api("/api/workspace/revert", token, { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify({ path: open.path }) });
      await openRefresh(open.path);
      loadFiles();
    } catch (e) {
      fail(e);
    }
  }
  async function openRefresh(p: string) {
    const r = await api<{ path: string; content: string; base: string | null }>(`/api/workspace/file?path=${encodeURIComponent(p)}`, token);
    setOpen({ path: r.path, content: r.content, saved: r.content, base: r.base });
  }

  async function resetAll() {
    if (!window.confirm("Throw away ALL edits and copy the code from the repository again?")) return;
    try {
      await api("/api/workspace/reset", token, { method: "POST" });
      setOpen(null);
      await loadFiles();
      setMsg({ kind: "ok", text: "Workspace reset to the repository code." });
    } catch (e) {
      fail(e);
    }
  }

  async function startBuild() {
    const cur = openRef.current;
    if (cur && cur.content !== cur.saved) await save();
    setBusy(true);
    setMsg(null);
    setFinished(null);
    try {
      const j = await api<JobInfo>("/api/build", token, { method: "POST" });
      setJobId(j.id);
      loadStudio();
      onStudioChanged();
    } catch (e) {
      fail(e);
    } finally {
      setBusy(false);
    }
  }

  const onDone = useCallback((j: JobInfo) => {
    setFinished(j);
    loadStudio();
    onStudioChanged();
  }, [loadStudio, onStudioChanged]);

  const res = finished?.status === "ok" ? finished.result : null;
  const version = typeof res?.version === "string" ? res.version : "";
  const releasable = res?.releasable === true;
  const exists = version ? releases.some((r) => r.version === version) : false;
  const building = studio?.busy?.kind === "firmware" || finished?.status === "running";
  const dirty = open ? open.content !== open.saved : false;
  const modifiedCount = files.filter((f) => f.modified).length;

  async function createRelease() {
    if (!finished) return;
    setBusy(true);
    setMsg(null);
    try {
      const r = await api<Release>(`/api/jobs/${finished.id}/release`, token, {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ notes: notes.trim(), min_version: minVersion.trim() }),
      });
      setMsg({ kind: "ok", text: `v${r.version} added to Releases. Pick the nodes that should get it on the Nodes tab.` });
      onReleased();
    } catch (e) {
      fail(e);
    } finally {
      setBusy(false);
    }
  }

  const tree = useMemo(() => {
    const groups = new Map<string, FileEntry[]>();
    for (const f of files) {
      const dir = f.path.slice(0, f.path.lastIndexOf("/"));
      groups.set(dir, [...(groups.get(dir) ?? []), f]);
    }
    return [...groups.entries()];
  }, [files]);

  const language = "cpp";

  return (
    <section className="stack">
      <div className="card stack">
        <div className="row between wrap">
          <h2>Firmware source</h2>
          <div className="row wrap">
            <span className="muted small">{modifiedCount ? `${modifiedCount} file${modifiedCount > 1 ? "s" : ""} changed from the repo` : "unchanged from the repo"}</span>
            <button className="btn small" onClick={() => downloadFile(token, "/api/workspace/patch", "dashboard-edits.patch").catch(fail)} disabled={!modifiedCount}>Download patch</button>
            <button className="btn small danger" onClick={resetAll} disabled={!!building}>Reset all</button>
          </div>
        </div>
        <p className="muted small">
          You are editing a private copy of <code>esp32-kiosk/</code> on the server, not the git repository. Use <b>Download patch</b> to bring your changes back with <code>git apply</code>.
          Set the version in <code>esp32-kiosk/src/version.h</code> before building: it must be higher than what the kiosks run.
        </p>

        <div className="studio">
          <nav className="files" aria-label="Files">
            {tree.map(([dir, list]) => (
              <div key={dir}>
                <div className="dir muted small">{dir}/</div>
                {list.map((f) => (
                  <button key={f.path} className={`file ${open?.path === f.path ? "on" : ""}`} onClick={() => openFile(f.path)}>
                    <span>{f.path.slice(dir.length + 1)}</span>
                    {f.modified && <i className="dot" title="changed from the repo" />}
                  </button>
                ))}
              </div>
            ))}
          </nav>

          <div className="editor">
            <div className="editor-bar row between wrap">
              <span className="mono small">{open?.path ?? "–"}{dirty ? " ●" : ""}</span>
              <span className="row wrap">
                <label className="check small"><input type="checkbox" checked={diff} onChange={(e) => setDiff(e.target.checked)} disabled={!open?.base} /> Diff vs repo</label>
                <button className="btn small" onClick={revert} disabled={!open || open.base === open.saved}>Revert file</button>
                <button className="btn small primary" onClick={save} disabled={!dirty}>Save (Ctrl+S)</button>
              </span>
            </div>
            <div className="editor-box">
              {open && !diff && (
                <Editor
                  height="100%"
                  path={open.path}
                  language={language}
                  theme={dark ? "vs-dark" : "light"}
                  value={open.content}
                  onChange={(v) => setOpen((o) => (o ? { ...o, content: v ?? "" } : o))}
                  onMount={(ed, monaco) => ed.addCommand(monaco.KeyMod.CtrlCmd | monaco.KeyCode.KeyS, () => void save())}
                  options={{ minimap: { enabled: false }, fontSize: 13, tabSize: 2, scrollBeyondLastLine: false, automaticLayout: true, wordWrap: "off" }}
                />
              )}
              {open && diff && open.base !== null && (
                <DiffEditor
                  height="100%"
                  language={language}
                  theme={dark ? "vs-dark" : "light"}
                  original={open.base}
                  modified={open.content}
                  keepCurrentOriginalModel
                  keepCurrentModifiedModel
                  options={{ readOnly: true, renderSideBySide: false, renderOverviewRuler: false, enableSplitViewResizing: false, scrollBeyondLastLine: false, minimap: { enabled: false }, fontSize: 13, automaticLayout: true }}
                />
              )}
              {!open && <p className="muted pad">Pick a file.</p>}
            </div>
          </div>
        </div>
        {msg && <p className={`alert ${msg.kind}`} role="status">{msg.text}</p>}
      </div>

      <div className="card stack">
        <div className="row between wrap">
          <h2>Build with PlatformIO</h2>
          <button className="btn primary" disabled={busy || !!building || studio?.toolchain.pio === false} onClick={startBuild}>
            {building ? "Building…" : dirty ? "Save and build" : "Build firmware"}
          </button>
        </div>
        {studio && !studio.toolchain.pio && <p className="alert error">PlatformIO is not installed on the server.</p>}
        <p className="muted small">The server compiles the saved files with <code>pio run</code> (environment esp32-3248s035). The first build downloads the ESP32 toolchain and takes several minutes; later builds take about a minute or two.</p>
        <JobLog token={token} jobId={jobId} onDone={onDone} onUnauthorized={onUnauthorized} />

        {res && (
          <div className="stack tight result">
            <div className="row wrap">
              <strong>firmware.bin</strong>
              <span className="mono">v{version || "?"}</span>
              <span className="muted small">{fmtBytes(Number(res.size))}</span>
              <span className="muted small mono" title={String(res.sha256)}>{String(res.sha256).slice(0, 12)}…</span>
            </div>
            {!releasable && <p className="alert error">Not releasable: {String(res.problem)}</p>}
            {releasable && exists && <p className="alert warn">Release v{version} already exists. Raise FW_VERSION in <code>version.h</code> and build again.</p>}
            {releasable && !exists && (
              <div className="row wrap">
                <input type="text" value={notes} onChange={(e) => setNotes(e.target.value)} placeholder="Release notes (optional)" maxLength={500} aria-label="Release notes" style={{ maxWidth: 320 }} />
                <input type="text" value={minVersion} onChange={(e) => setMinVersion(e.target.value)} placeholder="Min. version (optional)" pattern="\d+\.\d+\.\d+" aria-label="Minimum version" style={{ maxWidth: 190 }} />
                <button className="btn primary" disabled={busy} onClick={createRelease}>Add v{version} to releases</button>
              </div>
            )}
            <div className="row">
              <button className="btn small" onClick={() => downloadFile(token, `/api/jobs/${finished!.id}/firmware.bin`, `firmware-${version || finished!.id}.bin`).catch(fail)}>Download .bin</button>
            </div>
          </div>
        )}
      </div>
    </section>
  );
}
