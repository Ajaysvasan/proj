// Marks the compiled CLI entry point executable (tsc does not preserve modes).
import { chmodSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
chmodSync(fileURLToPath(new URL('../dist/src/index.js', import.meta.url)), 0o755);
