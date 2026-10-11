// Client WhatsApp "leggero": solo per sapere che e' arrivato un messaggio (e da chi). Non decifra i messaggi e non li conferma,
// cosi' restano in coda per l'app. Collegamento TLS + WebSocket + Noise XX + login con le chiavi che l'app prepara.
#pragma once
#include <stdint.h>
#include <stddef.h>
typedef struct LiteClient LiteClient;
typedef struct { char tag[32]; char from[100], participant[100], notify[100], type[24], id[56], recipient[100], category[24]; } LiteNode;
typedef void (*LiteOnNode)(const LiteNode *n, void *ud);
enum { LITE_OK = 0, LITE_ERR_NET = -1, LITE_ERR_TLS = -2, LITE_ERR_WS = -3, LITE_ERR_NOISE = -4, LITE_ERR_LOGIN = -5, LITE_CLOSED = -6, LITE_CONFLICT = -7 };
LiteClient *lite_new(const uint8_t noise_priv[32], const uint8_t noise_pub[32], const uint8_t *login_payload, size_t payload_len, const char *cacert_pem_path);
int lite_connect(LiteClient *c);                                  // collega e fa il login; 0 ok, <0 errore
int lite_poll(LiteClient *c, int timeout_ms, LiteOnNode cb, void *ud);   // elabora i pacchetti in arrivo; <0 se la connessione e' caduta
void lite_close(LiteClient *c);
const char *lite_last_error(LiteClient *c);
