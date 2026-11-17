/** @type {import('next').NextConfig} */
const nextConfig = {
  // tools/dashboard-shots builds into its own folder so it never touches the dev/Vercel build output
  distDir: process.env.NEXT_DIST_DIR || ".next",
  reactStrictMode: true,
  poweredByHeader: false,
};
export default nextConfig;
