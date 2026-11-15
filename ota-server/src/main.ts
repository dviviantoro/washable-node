import path from "node:path";
import { loadConfig, loadDotEnv } from "./config.js";
import { Store } from "./db.js";
import { buildApp } from "./app.js";

loadDotEnv();
const config = loadConfig();
const store = new Store(path.join(config.dataDir, "ota.db"));
const app = await buildApp(config, store);

const prune = () => {
  const n = store.pruneEvents(new Date(Date.now() - config.eventRetentionDays * 864e5).toISOString());
  if (n) app.log.info(`pruned ${n} old events`);
};
prune();
setInterval(prune, 24 * 3600 * 1000).unref();

for (const sig of ["SIGINT", "SIGTERM"] as const) {
  process.on(sig, async () => {
    await app.close();
    store.close();
    process.exit(0);
  });
}

await app.listen({ host: config.host, port: config.port });
