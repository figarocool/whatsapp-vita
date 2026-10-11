import { promises } from './fs.js';
export const { open, readFile, writeFile, mkdir, readdir, unlink, stat, access, rm, rename, copyFile } = promises;
export default promises;
