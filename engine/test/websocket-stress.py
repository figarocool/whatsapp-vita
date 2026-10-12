import asyncio, ssl, subprocess, tempfile, pathlib, os
import websockets
async def main():
 with tempfile.TemporaryDirectory(prefix='wa-ws-test-') as td:
  d=pathlib.Path(td)
  subprocess.run(['openssl','req','-x509','-newkey','rsa:2048','-nodes','-keyout',str(d/'key.pem'),'-out',str(d/'cert.pem'),'-days','1','-subj','/CN=localhost','-addext','subjectAltName=IP:127.0.0.1'],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,check=True)
  ctx=ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER); ctx.load_cert_chain(d/'cert.pem',d/'key.pem')
  count=0
  pong_received=False
  async def check_ping(ws):
   nonlocal pong_received
   await asyncio.sleep(15)
   waiter=await ws.ping(b"probe")
   await asyncio.wait_for(waiter,15)
   pong_received=True
  async def echo(ws):
   nonlocal count
   ping_task=asyncio.create_task(check_ping(ws))
   try:
    async for payload in ws:
     assert len(payload)==196608 and payload==bytes([count%256])*196608
     count+=1
     if count == 650: await asyncio.sleep(9)
     await ws.send(payload)
   except websockets.ConnectionClosedOK: pass
   await ping_task
  async with websockets.serve(echo,'127.0.0.1',0,ssl=ctx,max_size=1024*1024,ping_interval=None,max_queue=1) as server:
   port=server.sockets[0].getsockname()[1]
   (d/'probe.js').write_text('''const n=globalThis.__nat; let sent=0, received=0;
const id=n.wsOpen('wss://127.0.0.1:PORT/',{},(ev,data)=>{if(ev==='message'){const b=new Uint8Array(data); if(b.length!==196608 || b[0]!==received%256 || b[b.length-1]!==received%256) throw new Error('payload corrotto'); received++;}});
const timer=setInterval(()=>{if(sent<850){const b=new Uint8Array(196608).fill(sent%256); if(!n.wsSend(id,b,true)) throw new Error('invio fallito'); sent++;}},20);
const done=setInterval(()=>{if(sent===850 && received===850){clearInterval(timer); clearInterval(done); console.log('STRESS RESULT',sent,received); n.wsClose(id);}},100);
'''.replace('PORT',str(port)))
   p=await asyncio.create_subprocess_exec(os.environ.get('WA_ENGINE_PC','engine/build/engine_pc'),str(d/'cert.pem'),td,str(d/'probe.js'),stdout=asyncio.subprocess.PIPE,stderr=asyncio.subprocess.STDOUT)
   try: output=(await asyncio.wait_for(p.communicate(),45))[0].decode()
   except asyncio.TimeoutError: p.kill(); await p.wait(); raise
   print(output)
   assert p.returncode==0 and 'STRESS RESULT 850 850' in output and pong_received and count==850
   print('OK: 850 frame da 192 KiB e ping concorrente, nessuna corruzione')
asyncio.run(main())
