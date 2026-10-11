// 'stream' di Node = readable-stream. CommonJS di proposito: chi fa require('stream') si aspetta la classe Stream stessa.
module.exports = require('readable-stream');

// il fetch del motore accetta direttamente un Readable come corpo: toWeb/fromWeb restituiscono lo stesso oggetto
module.exports.Readable.toWeb = (r) => r; module.exports.Readable.fromWeb = (r) => r;
