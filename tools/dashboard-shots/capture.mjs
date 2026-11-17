// Drives the dashboard in a real browser and saves a screenshot of every page.
// usage (run.sh does all of this):  node capture.mjs build | pass <name> | kiosk
//   prepare          bumps FW_VERSION in the demo server's working copy (so the Code tab shows a changed file and a diff)
//   build            starts a real PlatformIO build from the Code tab and shoots it while it runs and when it is done, then adds the result to Releases
//   pass <name>      desktop-light | desktop-dark | mobile-light | mobile-dark: shoots every dashboard page in that setup
//   kiosk            shoots every kiosk page through the dashboard's Kiosk preview (Bahasa/English x dark/light) at native 320x480
// env: DASH_URL, API_URL, PASSWORD, OUT (output root), DATA_DIR (demo server data dir), CHROME_PATH (optional), SHOTS_BUILD=0 (skip the live build)
import { chromium } from "playwright-core";
import { existsSync, mkdirSync, renameSync, writeFileSync } from "node:fs";
import os from "node:os";
import path from "node:path";
import { fakeImage, findChrome } from "./lib.mjs";

const { DASH_URL = "http://localhost:3300", API_URL = "http://localhost:8099", PASSWORD, OUT, DATA_DIR } = process.env;
if (!PASSWORD || !OUT || !DATA_DIR) throw new Error("PASSWORD, OUT and DATA_DIR must be set (use run.sh)");

const SETUPS = {
  "desktop-light": { viewport: { width: 1280, height: 900 }, scheme: "light", dsf: 1, mobile: false },
  "desktop-dark": { viewport: { width: 1280, height: 900 }, scheme: "dark", dsf: 1, mobile: false },
  "mobile-light": { viewport: { width: 390, height: 844 }, scheme: "light", dsf: 2, mobile: true },
  "mobile-dark": { viewport: { width: 390, height: 844 }, scheme: "dark", dsf: 2, mobile: true },
};

const browser = await chromium.launch({ executablePath: findChrome(), args: ["--no-sandbox"] });
const problems = [];
const leaks = []; // real hostnames, paths or addresses that must never end up in a published screenshot
// Always checked: loopback, home paths, e-mail. Add your own real names with LEAK_WORDS="my-domain.com,my-login" (comma separated; the login name and host name of this machine are added automatically).
const esc = (w) => w.replace(/[.*+?^${}()|[\]\\]/g, "\\$&");
const leakWords = [...(process.env.LEAK_WORDS || "").split(","), os.userInfo().username, os.hostname()].map((w) => w.trim()).filter((w) => w.length > 2);
const LEAK = new RegExp(["localhost", "127\\.0\\.0\\.1", "gmail\\.com", "\\/home\\/", ...leakWords.map(esc)].join("|"), "i");
const log = (m) => console.log(`[shots] ${m}`);

async function open(name) {
  const s = SETUPS[name];
  const ctx = await browser.newContext({ viewport: s.viewport, deviceScaleFactor: s.dsf, isMobile: s.mobile, hasTouch: s.mobile, colorScheme: s.scheme });
  const page = await ctx.newPage();
  page.on("dialog", (d) => d.accept());
  page.on("pageerror", (e) => problems.push(`${name} pageerror: ${e.message}`));
  page.on("console", (m) => m.type() === "error" && !/401|Failed to load resource/.test(m.text()) && problems.push(`${name} console: ${m.text().slice(0, 160)}`));
  return { ctx, page, name };
}

async function login(page) {
  await page.goto(DASH_URL);
  await page.waitForSelector("input[type=password], nav.tabs", { timeout: 30000 });
  if (await page.locator("input[type=password]").isVisible().catch(() => false)) {
    await page.locator("input[type=password]").fill(PASSWORD);
    await page.getByRole("button", { name: "Sign in" }).click();
  }
  await page.waitForSelector("nav.tabs", { timeout: 30000 });
}

const tab = async (page, label) => {
  await page.locator("nav.tabs button", { hasText: label }).click();
  await page.waitForTimeout(400);
};

const slug = (s) => s.toLowerCase().replace(/[^a-z0-9]+/g, "-").replace(/^-|-$/g, "");

async function shot(page, dir, name, { full = true } = {}) {
  mkdirSync(dir, { recursive: true });
  await page.evaluate(() => window.scrollTo(0, 0)); // a scrolled page would put the sticky header in the middle of a full-page capture
  const text = await page.evaluate(() => document.body.innerText);
  const hit = LEAK.exec(text);
  if (hit) leaks.push(`${path.basename(dir)}/${name}: the page shows "${hit[0]}"`);
  await page.waitForTimeout(600);
  await page.screenshot({ path: path.join(dir, `${name}.png`), fullPage: full });
  log(`${path.basename(dir)}/${name}`);
}

async function waitMonaco(page, needle = "washable") {
  await page.waitForFunction((n) => document.querySelector(".view-lines")?.textContent?.includes(n), needle, { timeout: 60000 });
}

async function waitCanvas(page) {
  await page.waitForFunction(() => {
    const c = document.querySelector("canvas");
    if (!c) return false;
    const d = c.getContext("2d").getImageData(0, 0, 320, 480).data;
    let n = 0;
    for (let i = 0; i < d.length; i += 16) if (d[i] + d[i + 1] + d[i + 2] > 40) n++;
    return n > 1500;
  }, null, { timeout: 60000 });
  await page.waitForTimeout(500);
}

const token = (page) => page.evaluate(() => localStorage.getItem("washable-ota-token"));
async function apiPut(page, p, body) {
  const t = await token(page);
  const r = await page.evaluate(async ({ api, p, body, t }) => (await fetch(api + p, { method: "PUT", headers: { "Content-Type": "application/json", Authorization: `Bearer ${t}` }, body: JSON.stringify(body) })).status, { api: API_URL, p, body, t });
  if (r >= 300) throw new Error(`PUT ${p} -> ${r}`);
}

// ====================================================================================== prepare
// The next release number goes into the working copy first: the Code tab then shows a changed file, "1 file changed from the repo" and a real diff.
async function prepare() {
  const { ctx, page } = await open("desktop-light");
  await login(page);
  await apiPut(page, "/api/workspace/file", {
    path: "esp32-kiosk/src/version.h",
    content: '#pragma once\n// Bumped by tools/release.py / the release workflow for every published firmware.\n#define FW_VERSION "1.3.0"\n',
  });
  log("FW_VERSION bumped to 1.3.0 in the working copy");
  await ctx.close();
}

// ====================================================================================== live build
async function build() {
  const dirs = { "desktop-light": ["14-code-building", "15-code-built"], "desktop-dark": ["14-code-building", "15-code-built"], "mobile-light": ["14-code-building", "15-code-built"] };
  const sessions = [];
  for (const name of Object.keys(dirs)) {
    const s = await open(name);
    await login(s.page);
    sessions.push(s);
  }
  const [lt] = sessions;
  for (const s of sessions) {
    await s.page.reload();
    await s.page.waitForSelector("nav.tabs");
    await tab(s.page, "Code & build");
    await waitMonaco(s.page);
  }
  await lt.page.getByRole("button", { name: /Build firmware/ }).click();
  log("build started");
  await lt.page.waitForTimeout(2500);
  for (const s of sessions.slice(1)) {
    await s.page.reload();
    await s.page.waitForSelector("nav.tabs");
    await waitMonaco(s.page);
  }
  // let the compiler get going so the log has something to show
  await lt.page.waitForFunction(() => /Compiling/.test(document.querySelector("pre.log")?.textContent ?? ""), null, { timeout: 240000 });
  await lt.page.waitForTimeout(25000);
  for (const s of sessions) {
    await s.page.locator("pre.log").scrollIntoViewIfNeeded();
    await shot(s.page, path.join(OUT, s.name), dirs[s.name][0]);
  }
  await lt.page.getByText("Succeeded").first().waitFor({ timeout: 15 * 60_000 });
  log("build finished");
  for (const s of sessions.slice(1)) {
    await s.page.reload();
    await s.page.waitForSelector("nav.tabs");
    await waitMonaco(s.page);
    await s.page.getByText("Succeeded").first().waitFor({ timeout: 30000 });
  }
  for (const s of sessions) {
    await s.page.locator(".result").scrollIntoViewIfNeeded();
    await shot(s.page, path.join(OUT, s.name), dirs[s.name][1]);
  }
  await lt.page.getByRole("button", { name: /Add v1\.3\.0 to releases/ }).click();
  await lt.page.getByText("added to Releases").waitFor({ timeout: 30000 });
  log("v1.3.0 added to releases");
  for (const s of sessions) await s.ctx.close();
}

// ====================================================================================== one pass over every dashboard page
async function pass(name) {
  const dir = path.join(OUT, name);
  const mobile = SETUPS[name].mobile;
  const { ctx, page } = await open(name);
  const tmp = path.join(OUT, ".tmp");
  mkdirSync(tmp, { recursive: true });
  const goodFile = path.join(tmp, "firmware-1.4.0.bin");
  const badFile = path.join(tmp, "firmware-nomarker.bin");
  writeFileSync(goodFile, fakeImage("1.4.0", 1_742_000));
  writeFileSync(badFile, fakeImage("1.4.1", 1_300_000, { marker: false }));

  // ---- sign in
  await page.goto(DASH_URL);
  await page.evaluate(() => localStorage.clear());
  await page.reload();
  await page.waitForSelector("input[type=password]");
  const hostLabel = () => page.evaluate(() => { for (const p of document.querySelectorAll("p.small")) p.textContent = p.textContent.replace(/localhost:\d+/, "ota.example.com"); });
  await hostLabel();
  await shot(page, dir, "01-login", { full: false });
  await page.locator("input[type=password]").fill("not-the-password");
  await page.getByRole("button", { name: "Sign in" }).click();
  await page.getByText("Wrong password").waitFor();
  await hostLabel();
  await shot(page, dir, "02-login-error", { full: false });
  await page.locator("input[type=password]").fill(PASSWORD);
  await page.getByRole("button", { name: "Sign in" }).click();
  await page.waitForSelector("nav.tabs");

  // ---- overview
  await tab(page, "Overview");
  await page.getByText("Need attention").waitFor();
  await shot(page, dir, "03-overview");

  // ---- nodes
  await tab(page, "Nodes");
  await page.getByRole("button", { name: "Warung Budi" }).waitFor();
  await shot(page, dir, "04-nodes");
  await page.getByRole("button", { name: "Warung Budi" }).click();
  await page.getByText("Timeline").first().waitFor();
  await shot(page, dir, "05-node-healthy");
  await page.getByRole("button", { name: "Close" }).click();
  await page.getByRole("button", { name: "Kost Mawar" }).click();
  await page.getByText("Timeline").first().waitFor();
  await shot(page, dir, "06-node-problem");
  await page.getByRole("button", { name: "Close" }).click();

  const pickRelease = async (version) => {
    const value = await page.locator("select[aria-label='Release to send']").evaluate((sel, v) => [...sel.options].find((o) => o.textContent.startsWith(`v${v}`))?.value, version);
    if (!value) throw new Error(`release ${version} is not in the list`);
    await page.locator("select[aria-label='Release to send']").selectOption(value);
  };
  const tick = async (...labels) => { for (const l of labels) await page.getByRole("checkbox", { name: `Select ${l}` }).check(); };

  // one node is already on 1.2.0 and is skipped: kiosks never downgrade or reinstall
  await tick("Warung Budi", "Kost Mawar", "Kampus Barat");
  await pickRelease("1.2.0");
  await page.getByRole("button", { name: "Send to selected" }).click();
  await page.locator(".alert.warn", { hasText: "skipped" }).waitFor();
  await shot(page, dir, "07-nodes-skipped");

  if (process.env.SHOTS_BUILD !== "0") {
    await page.reload();
    await page.getByRole("button", { name: "Warung Budi" }).waitFor();
    await tick("Warung Budi", "Laundry Melati");
    await pickRelease("1.3.0");
    await shot(page, dir, "08-nodes-selected");
    await page.getByRole("button", { name: "Send to selected" }).click();
    await page.locator(".alert.ok", { hasText: "assigned to 2 nodes" }).waitFor();
    await shot(page, dir, "09-nodes-assigned");
  }

  // ---- releases
  await tab(page, "Releases");
  await page.locator("table").first().waitFor();
  await shot(page, dir, "10-releases");
  await page.setInputFiles("input[type=file]", goodFile);
  await page.getByText(/firmware version 1\.4\.0/).waitFor();
  await page.getByLabel("Minimum version", { exact: false }).first().fill("1.2.0").catch(() => {});
  await page.locator("textarea").fill("Relay controller timeout fix, LED self-test on boot");
  await shot(page, dir, "11-releases-upload");
  await page.setInputFiles("input[type=file]", badFile);
  await page.getByText("OTA client marker not found").waitFor();
  await shot(page, dir, "12-releases-upload-invalid");

  // ---- code & build
  await tab(page, "Code & build");
  await waitMonaco(page);
  await shot(page, dir, "13-code-editor");
  await page.locator("button.file", { hasText: "version.h" }).click();
  await waitMonaco(page, "FW_VERSION");
  await page.getByLabel("Diff vs repo").check();
  await page.locator(".monaco-diff-editor").waitFor();
  await page.waitForTimeout(600);
  await shot(page, dir, "16-code-diff");
  await page.getByLabel("Diff vs repo").uncheck();

  // ---- kiosk preview: before the first build, then a few kiosk pages
  const current = path.join(DATA_DIR, "preview", "current");
  const hidden = path.join(DATA_DIR, "preview", "current.hidden");
  if (existsSync(current)) {
    renameSync(current, hidden);
    await tab(page, "Kiosk preview");
    await page.reload();
    await page.waitForSelector("nav.tabs");
    await page.getByText("No preview yet").waitFor();
    await shot(page, dir, "17-preview-not-built");
    renameSync(hidden, current);
    await page.reload();
    await page.waitForSelector("nav.tabs");
  }
  await tab(page, "Kiosk preview");
  await page.getByText("Jump to screen").waitFor();
  await waitCanvas(page);
  const scene = async (label) => {
    await page.getByRole("button", { name: label, exact: true }).click();
    await page.waitForTimeout(1600);
  };
  await shot(page, dir, "18-preview-idle");
  await scene("Payment (QRIS)");
  await shot(page, dir, "19-preview-payment");
  await scene("Wash running");
  await shot(page, dir, "20-preview-running");
  await page.getByRole("button", { name: "English" }).click();
  await page.getByRole("button", { name: "Light" }).click();
  await waitCanvas(page);
  await scene("Service list");
  await shot(page, dir, "21-preview-english-light");

  // ---- activity
  await tab(page, "Activity");
  await page.getByText("Node timeline").waitFor();
  await shot(page, dir, "22-activity");
  await page.getByRole("button", { name: "Downloads" }).click();
  await shot(page, dir, "23-activity-downloads");

  await ctx.close();
}

// ====================================================================================== every kiosk page through the preview
async function kiosk() {
  const { ctx, page } = await open("desktop-light");
  await login(page);
  await tab(page, "Kiosk preview");
  await page.getByText("Jump to screen").waitFor();
  await page.getByRole("button", { name: "1×", exact: true }).click(); // native 320x480
  await waitCanvas(page);
  const names = await page.locator(".chips button").allInnerTexts();
  log(`${names.length} kiosk pages`);
  for (const [lang, langLabel] of [["id", "Bahasa"], ["en", "English"]]) {
    for (const [theme, themeLabel] of [["dark", "Dark"], ["light", "Light"]]) {
      await page.getByRole("button", { name: langLabel, exact: true }).click();
      await page.getByRole("button", { name: themeLabel, exact: true }).click();
      await waitCanvas(page);
      const dir = path.join(OUT, "kiosk", `${lang}-${theme}`);
      mkdirSync(dir, { recursive: true });
      for (const [i, n] of names.entries()) {
        await page.getByRole("button", { name: n, exact: true }).click();
        await page.waitForTimeout(1500);
        // the canvas' own pixels (exactly 320x480, no CSS scaling or border rounding), which also lets the QR check decode the payment page
        const b64 = await page.evaluate(() => document.querySelector("canvas").toDataURL("image/png").split(",")[1]);
        writeFileSync(path.join(dir, `${String(i + 1).padStart(2, "0")}-${slug(n)}.png`), Buffer.from(b64, "base64"));
        if (n === "Payment (QRIS)") {
          // frames for the privacy check (sheets.py): the kiosk animates a scan line over the QR, which can hide a finder pattern in any single frame
          const qd = path.join(OUT, ".qr", `${lang}-${theme}`);
          mkdirSync(qd, { recursive: true });
          for (let k = 0; k < 10; k++) {
            await page.waitForTimeout(230);
            const f = await page.evaluate(() => document.querySelector("canvas").toDataURL("image/png").split(",")[1]);
            writeFileSync(path.join(qd, `${k}.png`), Buffer.from(f, "base64"));
          }
        }
      }
      log(`kiosk ${lang}-${theme}: ${names.length} pages`);
    }
  }
  await ctx.close();
}

const [cmd, arg] = process.argv.slice(2);
try {
  if (cmd === "prepare") await prepare();
  else if (cmd === "build") await build();
  else if (cmd === "pass") await pass(arg);
  else if (cmd === "kiosk") await kiosk();
  else throw new Error("usage: capture.mjs prepare | build | pass <desktop-light|desktop-dark|mobile-light|mobile-dark> | kiosk");
} finally {
  await browser.close();
}
if (problems.length) console.log("[shots] browser warnings (not fatal):\n  " + [...new Set(problems)].join("\n  "));
if (leaks.length) {
  console.log("[shots] PRIVACY: a page shows something that must not be published:\n  " + leaks.join("\n  "));
  process.exitCode = 1;
}
