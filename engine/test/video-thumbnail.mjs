import fs from 'node:fs';
import vm from 'node:vm';
import assert from 'node:assert/strict';
const source = fs.readFileSync(new URL('../../bridge/server.js', import.meta.url), 'utf8');
const start = source.indexOf('async function mediaForRaw(');
const end = source.indexOf('\nasync function sendText', start);
const thumbnail = Buffer.from('preview');
const video = { jpegThumbnail: thumbnail, mediaKey: Buffer.from('key') };
const message = { key: { remoteJid: 'group@g.us' }, message: { videoMessage: video } };
let remote = 0, cached = 0;
const context = {
  Buffer, mediaCache: new Map(), mediaFile: () => '/unused',
  raws: new Map([['group@g.us', new Map([['id', message]])]]),
  describe: () => ({ kind: 'video' }), unwrap: m => m.message, thumbBuf: b => b,
  download() { throw Error('must not download full video for preview'); },
  cacheSet() { cached++; }, log() {}, VIDEO_PLACEHOLDER: Buffer.from('placeholder').toString('base64'),
  fs: { readFileSync() { throw Error('not cached'); }, writeFileSync() {} },
  sharp: b => ({ resize() { return this; }, jpeg() { return this; }, async toBuffer() { return b; } }),
  B: { async downloadContentFromMessage(media, type) {
    remote++; assert.equal(type, 'thumbnail-video'); assert.equal(media.directPath, '/preview');
    return (async function* () { yield thumbnail; })();
  } }
};
const get = vm.runInNewContext(source.slice(start, end) + '\nmediaForRaw', context);
assert.deepEqual((await get('group@g.us', 'id')).buf, thumbnail);
assert.equal(remote, 0);
video.jpegThumbnail = Buffer.alloc(0);
video.thumbnailDirectPath = '/preview';
assert.deepEqual((await get('group@g.us', 'id')).buf, thumbnail);
assert.equal(remote, 1);
delete video.thumbnailDirectPath;
assert.equal((await get('group@g.us', 'id')).buf.toString(), 'placeholder');
assert.equal(cached, 2, 'do not persist placeholders');
const prefetchStart = source.indexOf('function prefetchChat(');
const prefetchEnd = source.indexOf('\n// la miniatura', prefetchStart);
let prefetched = 0;
vm.runInNewContext(source.slice(prefetchStart, prefetchEnd) + "\nprefetchChat('group@g.us', 4)", {
  raws: context.raws, num: n => Number(n || 0), describe: context.describe,
  prefetchMedia() { prefetched++; }
});
assert.equal(prefetched, 1, 'video previews must prefetch when entering a chat');
console.log('OK: video preview embedded, separate, missing; chat video prefetch');
