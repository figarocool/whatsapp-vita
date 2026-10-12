import './prelude.js';
import * as fs from 'fs';
fs.rmSync('/tmp/rmt', { recursive: true, force: true }); console.log('esiste ancora:', fs.existsSync('/tmp/rmt'));
