const esbuild = require('esbuild');
const fs = require('fs');
const path = require('path');

const isWatch = process.argv.includes('--watch');
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
  fs.copyFileSync(
    path.join(__dirname, 'manifest.json'),
    path.join(distDir, 'manifest.json')
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
