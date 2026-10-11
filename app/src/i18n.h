#ifndef WA_I18N_H
#define WA_I18N_H
// Lingue dell'interfaccia. Le frasi nel codice sono scritte in italiano e T("...") le traduce nella lingua scelta
// (se manca la traduzione resta l'italiano). Le tabelle vengono da tools/i18n/*.py (generate in i18n_data.c da tools/gen_i18n.py).

typedef enum { L_IT, L_EN, L_FR, L_DE, L_ES, L_PT, L_PTBR, L_NL, L_DA, L_FI, L_NO, L_SV, L_PL, L_TR, L_RU, L_JA, L_KO, L_ZHS, L_ZHT, L_COUNT } Lang;

const char *tr(const char *italiano);   // traduce una frase italiana nella lingua corrente
#define T(s) tr(s)
void i18n_init(void);                   // legge la scelta salvata (ux0:data/whatsapp/lang.txt) oppure la lingua di sistema della Vita
int i18n_lang(void);                    // lingua in uso
int i18n_is_auto(void);                 // 1 = segue la lingua di sistema
void i18n_set(int lang);                // sceglie una lingua; -1 = automatica (lingua di sistema); la salva
int i18n_system_lang(void);             // lingua di sistema della Vita, tradotta nel nostro elenco
const char *i18n_code(int lang);        // codice della lingua ("it", "en", "pt-BR"...)
const char *i18n_native_name(int lang); // nome della lingua scritto nella lingua stessa
const char *i18n_font_path(int lang);   // font dedicato (giapponese, coreano, cinese) oppure NULL per Roboto
#endif
