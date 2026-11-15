import test from "node:test";
import assert from "node:assert/strict";
import { mkdirSync, mkdtempSync, rmSync, writeFileSync, symlinkSync } from "node:fs";
import os from "node:os";
import path from "node:path";
import { Workspace } from "../src/workspace.js";

function fixture() {
  const root = mkdtempSync(path.join(os.tmpdir(), "ws-test-"));
  const repo = path.join(root, "repo");
  const put = (rel: string, body: string) => {
    mkdirSync(path.dirname(path.join(repo, rel)), { recursive: true });
    writeFileSync(path.join(repo, rel), body);
  };
  put("esp32-kiosk/platformio.ini", "[env:x]\n");
  put("esp32-kiosk/src/main.cpp", "int main() { return 0; }\n");
  put("esp32-kiosk/src/version.h", '#define FW_VERSION "1.0.0"\n');
  put("esp32-kiosk/src/fonts/f_big.c", "// generated\n");
  put("esp32-kiosk/.pio/build/junk.bin", "x");
  put("shared/relay_proto/relay_proto.h", "// proto\n");
  put("tools/sim/web_main.cpp", "// sim\n");
  return { root, ws: new Workspace(repo, path.join(root, "data")), cleanup: () => rmSync(root, { recursive: true, force: true }) };
}

test("workspace: seeds from the repo, lists only editable sources, tracks changes against the pristine copy", async () => {
  const { ws, cleanup } = fixture();
  try {
    const files = await ws.list();
    assert.deepEqual(files.map((f) => f.path), ["esp32-kiosk/src/main.cpp", "esp32-kiosk/src/version.h", "shared/relay_proto/relay_proto.h"]); // no fonts, no .pio, no ini
    assert.ok(files.every((f) => !f.modified));

    await ws.write("esp32-kiosk/src/version.h", '#define FW_VERSION "1.3.0"\n');
    assert.equal((await ws.list()).find((f) => f.path.endsWith("version.h"))!.modified, true);
    const r = await ws.read("esp32-kiosk/src/version.h");
    assert.match(r.content, /1\.3\.0/);
    assert.match(r.base ?? "", /1\.0\.0/);

    const patch = await ws.patch();
    assert.match(patch, /^diff --git a\/esp32-kiosk\/src\/version\.h b\/esp32-kiosk\/src\/version\.h/m);
    assert.match(patch, /\+#define FW_VERSION "1\.3\.0"/);

    await ws.revert("esp32-kiosk/src/version.h");
    assert.equal((await ws.list()).every((f) => !f.modified), true);

    await ws.write("esp32-kiosk/src/main.cpp", "// edited\n");
    await ws.reset();
    assert.match((await ws.read("esp32-kiosk/src/main.cpp")).content, /int main/);
  } finally {
    cleanup();
  }
});

test("workspace: refuses paths outside the editable source roots", async () => {
  const { root, ws, cleanup } = fixture();
  try {
    await ws.ensure();
    for (const bad of ["../../etc/passwd", "/etc/passwd", "esp32-kiosk/platformio.ini", "esp32-kiosk/src/fonts/f_big.c", "esp32-kiosk/src/../platformio.ini", "esp32-kiosk/src/x.py", "esp32-kiosk\\src\\main.cpp", "tools/sim/web_main.cpp", "", "esp32-kiosk/src/main.cpp\0.h"]) {
      await assert.rejects(ws.read(bad), undefined, `read ${JSON.stringify(bad)}`);
      await assert.rejects(ws.write(bad, "x"), undefined, `write ${JSON.stringify(bad)}`);
    }
    await assert.rejects(ws.write("esp32-kiosk/src/main.cpp", 42 as unknown as string));
    await assert.rejects(ws.write("esp32-kiosk/src/main.cpp", "a\0b"));
    await assert.rejects(ws.write("esp32-kiosk/src/main.cpp", "x".repeat(600 * 1024)));

    // a symlink planted inside an editable folder must not be followed
    symlinkSync("/etc/hostname", path.join(root, "data", "workspace", "esp32-kiosk", "src", "evil.h"));
    await assert.rejects(ws.read("esp32-kiosk/src/evil.h"));
    await assert.rejects(ws.write("esp32-kiosk/src/evil.h", "x"));
    assert.ok(!(await ws.list()).some((f) => f.path.endsWith("evil.h")));
  } finally {
    cleanup();
  }
});
