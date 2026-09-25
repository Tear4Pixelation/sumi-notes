import { createMDX } from 'fumadocs-mdx/next';

const withMDX = createMDX();

/** @type {import('next').NextConfig} */
const config = {
  output: 'export',
  // a static export has no server to resize images on request
  images: { unoptimized: true },
  reactStrictMode: true,
};

export default withMDX(config);
