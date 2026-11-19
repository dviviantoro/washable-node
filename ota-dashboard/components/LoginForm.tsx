"use client";

import { useState } from "react";
import { API_URL, DEMO_TOKEN, api } from "@/lib/api";

export function LoginForm({ onLogin }: { onLogin: (token: string) => void }) {
  const [password, setPassword] = useState("");
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<string | null>(null);

  async function submit(e: React.FormEvent) {
    e.preventDefault();
    setBusy(true);
    setError(null);
    try {
      const r = await api<{ token: string }>("/api/login", null, {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ password }),
      });
      onLogin(r.token);
    } catch (err) {
      setError(err instanceof Error ? err.message : "Login failed");
    } finally {
      setBusy(false);
    }
  }

  return (
    <main className="center">
      <form className="card narrow stack" onSubmit={submit}>
        <div>
          <h1>Washable OTA</h1>
          <p className="muted">Firmware releases for the laundry kiosks.</p>
        </div>
        <label className="field">
          <span>Admin password</span>
          <input type="password" autoFocus autoComplete="current-password" value={password} onChange={(e) => setPassword(e.target.value)} />
        </label>
        {error && <p className="alert error" role="alert">{error}</p>}
        <button className="btn primary" disabled={busy || !password}>{busy ? "Signing in…" : "Sign in"}</button>
        <div className="demo-cta stack tight">
          <button type="button" className="btn" onClick={() => onLogin(DEMO_TOKEN)}>Try the demo (sample data)</button>
          <p className="muted small">Explore every tab with made-up nodes, releases, a code editor and the interactive kiosk. Nothing is sent to a server.</p>
        </div>
        <p className="muted small">Server: {new URL(API_URL).host}</p>
      </form>
    </main>
  );
}
