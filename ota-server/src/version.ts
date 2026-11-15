const SEMVER = /^(\d+)\.(\d+)\.(\d+)$/;

export function isSemver(v: string): boolean {
  return SEMVER.test(v);
}

export function cmpVer(a: string, b: string): number {
  const x = SEMVER.exec(a);
  const y = SEMVER.exec(b);
  if (!x || !y) return 0;
  for (let i = 1; i <= 3; i++) {
    const d = Number(x[i]) - Number(y[i]);
    if (d !== 0) return d;
  }
  return 0;
}
