// Motore JavaScript (QuickJS) con la parte nativa per far girare Baileys senza Node: timer, cifratura, WebSocket sicuro, file, zlib.
// Portabile: gira su PC (prove) e sulla Vita (VitaSDK, mbedTLS, BSD sockets).
#pragma once
#include "quickjs.h"

typedef struct Engine Engine;
typedef void (*EngineLog)(const char *line);

Engine *eng_new(const char *cacert_path, const char *data_dir, EngineLog log);
int eng_run_module(Engine *e, const char *path);     // carica ed esegue un modulo ES; 0 ok
int eng_loop_step(Engine *e, int max_wait_ms);       // un giro del ciclo eventi: 1 se c'e' ancora lavoro, 0 se tutto fermo, <0 errore
void eng_free(Engine *e);

// ---- richieste "HTTP" verso il motore (stesso schema del bridge) ----
// Dalla app (altro thread): blocca finche' il bridge JS ha risposto. 0 = ok.
typedef struct { int status; char *headers_json; unsigned char *body; size_t len; } EngResponse;
int eng_request(Engine *e, const char *method, const char *url, const char *headers_json, const void *body, size_t blen, int timeout_ms, EngResponse *out);
void eng_response_free(EngResponse *r);
int eng_listen_tcp(Engine *e, int port);   // solo per prove: espone lo stesso schema su un socket TCP locale
