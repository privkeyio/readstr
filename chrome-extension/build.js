const esbuild = require('esbuild');
const fs = require('fs');
const path = require('path');

const isWatch = process.argv.includes('--watch');
const isDev = process.env.EXTENSION_DEV === '1';
const devPort = process.env.EXTENSION_DEV_PORT || '3000';
if (isDev && !/^\d+$/.test(devPort)) {
  throw new Error(`EXTENSION_DEV_PORT must be numeric, got: ${devPort}`);
}
const devTrustedHosts = isDev ? [`localhost:${devPort}`, `127.0.0.1:${devPort}`] : [];
const devMatchPatterns = devTrustedHosts.map((host) => `http://${host}/*`);
const distDir = path.join(__dirname, 'dist');
const publicDir = path.join(__dirname, 'public');

if (!fs.existsSync(distDir)) {
  fs.mkdirSync(distDir, { recursive: true });
}

function copyPublicFiles() {
  const files = fs.readdirSync(publicDir);
  for (const file of files) {
    const srcPath = path.join(publicDir, file);
    const destPath = path.join(distDir, file);
    const stat = fs.statSync(srcPath);
    if (stat.isDirectory()) {
      fs.cpSync(srcPath, destPath, { recursive: true });
    } else {
      fs.copyFileSync(srcPath, destPath);
    }
  }
}

function copyManifest() {
  const manifest = JSON.parse(
    fs.readFileSync(path.join(__dirname, 'manifest.json'), 'utf8')
  );
  if (devMatchPatterns.length > 0) {
    manifest.host_permissions.push(...devMatchPatterns);
    for (const script of manifest.content_scripts) {
      script.matches.push(...devMatchPatterns);
    }
  }
  fs.writeFileSync(
    path.join(distDir, 'manifest.json'),
    JSON.stringify(manifest, null, 2)
  );
}

const buildOptions = {
  entryPoints: [
    'src/background.ts',
    'src/content.ts',
    'src/page-signer.ts',
    'src/popup.tsx',
    'src/options.tsx'
  ],
  bundle: true,
  outdir: distDir,
  format: 'iife',
  target: 'chrome120',
  sourcemap: isWatch ? 'inline' : false,
  minify: !isWatch,
  jsx: 'automatic',
  define: {
    __EXTENSION_DEV_MATCHES__: JSON.stringify(devMatchPatterns),
    __EXTENSION_DEV_HOSTS__: JSON.stringify(devTrustedHosts),
  },
};

async function build() {
  try {
    copyManifest();
    copyPublicFiles();

    if (isWatch) {
      const ctx = await esbuild.context(buildOptions);
      await ctx.watch();
      console.log('Watching for changes...');
    } else {
      await esbuild.build(buildOptions);
      console.log('Build complete');
    }
  } catch (error) {
    console.error('Build failed:', error);
    process.exit(1);
  }
}

build();
