"use client";

import { useEffect, useState } from "react";
import { API_URL, DEMO_TOKEN, isDemoToken, loadToken, resetDemo, saveToken } from "@/lib/api";
import { Dashboard } from "@/components/Dashboard";
import { LoginForm } from "@/components/LoginForm";

export default function Home() {
  const [token, setToken] = useState<string | null | undefined>(undefined);

  useEffect(() => setToken(loadToken()), []);

  const update = (t: string | null) => {
    if (!t || isDemoToken(t)) resetDemo();   // every visit to the demo starts from the same sample data
    saveToken(t);
    setToken(t);
  };

  if (!API_URL && !isDemoToken(token)) {
    return (
      <main className="center">
        <div className="card narrow">
          <h1>Not configured</h1>
          <p>
            Set <code>NEXT_PUBLIC_API_URL</code> to the public https URL of the OTA backend (the Cloudflare tunnel) and redeploy.
          </p>
          <button className="btn" onClick={() => update(DEMO_TOKEN)}>Open the demo instead</button>
        </div>
      </main>
    );
  }
  if (token === undefined) return <main className="center"><p className="muted">Loading…</p></main>;
  if (!token) return <LoginForm onLogin={update} />;
  return <Dashboard token={token} onLogout={() => update(null)} />;
}
