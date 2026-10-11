import { promises } from './fs.js';
export const { open, readFile, writeFile, appendFile, mkdir, readdir, unlink, stat, rename, copyFile, rm, access } = promises;
export default promises;
