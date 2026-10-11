#ifndef WA_UPDATER_H
#define WA_UPDATER_H

// Aggiornamenti automatici dai release di GitHub (figarocool/whatsapp-vita).
// Schema ripreso da Desktop Mode: manifest JSON + SHA-256 + VPK installato da un'app "installatrice" separata,
// perche' un'app non puo' sovrascrivere se stessa mentre gira. A differenza dell'originale, la barra di avanzamento
// e' disegnata dall'interfaccia dell'app (non dalla finestra di sistema): compare sempre, anche al primo download.

typedef enum { UPD_IDLE = 0, UPD_CHECKING, UPD_AVAILABLE, UPD_UPTODATE, UPD_DOWNLOADING, UPD_VERIFYING, UPD_EXTRACTING, UPD_INSTALLING, UPD_ERROR } UpdState;

typedef struct {
  UpdState state;
  char current[48];            // versione installata (da assets/version.txt)
  char tag[48];                // versione trovata nel manifest
  char notes[200];             // note della release (facoltative)
  unsigned long long done, total;
  char msg[160];               // fase corrente / errore, gia' in italiano
  int can_install;             // 0 nella versione "sicura": non puo' installare pacchetti
  int quiet;                   // l'ultimo controllo era automatico: gli errori non vanno mostrati
  int must_exit;               // l'installatore e' partito: l'app deve chiudersi
  char last_fail_tag[48]; int last_fail_code;   // tentativo precedente fallito per questa versione
} UpdInfo;

void upd_init(void);               // legge la versione installata e ripulisce i resti di un aggiornamento precedente
void upd_cleanup_helper(void);     // da chiamare PRIMA di aprire la grafica (carica moduli di sistema)
void upd_check(int manual);        // controllo in un thread; manual=1 mostra anche "gia' aggiornato" e gli errori
void upd_install(void);            // scarica, verifica, prepara e avvia l'installazione (thread)
void upd_cancel(void);
void upd_retry(void);              // riprova l'installazione dopo un errore
void upd_snapshot(UpdInfo *out);
int upd_busy(void);                // 1 mentre scarica/installa: l'interfaccia mostra la schermata di avanzamento
void upd_dismiss(void);            // torna a IDLE (dopo errore/risultato mostrato)

#endif
