const fs = require('fs');
const path = require('path');
const { execSync } = require('child_process');

const distDir = path.join(__dirname, 'dist');
const releaseDir = path.join(__dirname, 'release');

const args = process.argv.slice(2);
const updateUrl = args.find(arg => arg.startsWith('--update-url='))?.split('=')[1];

function ensureDir(dir) {
  if (!fs.existsSync(dir)) {
    fs.mkdirSync(dir, { recursive: true });
  }
}

function getVersion() {
  const manifest = JSON.parse(fs.readFileSync(path.join(__dirname, 'manifest.json'), 'utf8'));
  return manifest.version;
}

function createZip(sourceDir, outputPath) {
  if (fs.existsSync(outputPath)) {
    fs.unlinkSync(outputPath);
  }
  execSync(`cd "${sourceDir}" && zip -r "${outputPath}" .`, { stdio: 'inherit' });
}

function patchManifestForSelfHosting(updateUrl) {
  const manifestPath = path.join(distDir, 'manifest.json');
  const manifest = JSON.parse(fs.readFileSync(manifestPath, 'utf8'));
  manifest.update_url = updateUrl;
  fs.writeFileSync(manifestPath, JSON.stringify(manifest, null, 2));
}

async function main() {
  console.log('Building extension...');
  execSync('npm run build', { cwd: __dirname, stdio: 'inherit' });

  ensureDir(releaseDir);
  const version = getVersion();

  console.log(`\nPackaging version ${version}...`);

  const webStoreZip = path.join(releaseDir, `readstr-${version}-webstore.zip`);
  createZip(distDir, webStoreZip);
  console.log(`Created: ${webStoreZip}`);

  if (updateUrl) {
    console.log(`\nCreating self-hosted version with update_url: ${updateUrl}`);
    patchManifestForSelfHosting(updateUrl);

    const selfHostedZip = path.join(releaseDir, `readstr-${version}-selfhosted.zip`);
    createZip(distDir, selfHostedZip);
    console.log(`Created: ${selfHostedZip}`);

    execSync('npm run build', { cwd: __dirname, stdio: 'pipe' });
  }

  console.log('\nPackaging complete!');
  console.log(`\nRelease files in: ${releaseDir}`);
}

main().catch(err => {
  console.error('Packaging failed:', err);
  process.exit(1);
});
