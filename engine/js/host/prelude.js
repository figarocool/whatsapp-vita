// Prima i globali (Buffer, process, timer...), poi quello che ne dipende (fetch, flussi).
import './globals.js';
import fetchImpl, { Response as ResponseImpl, Headers as HeadersImpl } from 'fetch';
globalThis.fetch = fetchImpl; globalThis.Response = ResponseImpl; globalThis.Headers = HeadersImpl;
