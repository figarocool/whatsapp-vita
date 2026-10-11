#include "i18n.h"
#include <psp2/apputil.h>
#include <psp2/system_param.h>
#include <psp2/io/fcntl.h>
#include <string.h>
#include <stdio.h>

extern const char *const I18N_KEYS[];   // frasi italiane, in ordine alfabetico per la ricerca binaria
extern const int I18N_COUNT;
extern const char *const I18N_VAL[][L_COUNT];

static int cur = L_IT, auto_mode = 1;
#define LANG_FILE "ux0:data/whatsapp/lang.txt"
static const char *const CODES[L_COUNT] = { "it", "en", "fr", "de", "es", "pt", "pt-BR", "nl", "da", "fi", "no", "sv", "pl", "tr", "ru", "ja", "ko", "zh-Hans", "zh-Hant" };
static const char *const NAMES[L_COUNT] = { "Italiano", "English", "Fran\xC3\xA7" "ais", "Deutsch", "Espa\xC3\xB1ol", "Portugu\xC3\xAA" "s", "Portugu\xC3\xAA" "s (Brasil)", "Nederlands", "Dansk", "Suomi", "Norsk", "Svenska", "Polski", "T\xC3\xBCrk\xC3\xA7" "e",
  "\xD0\xA0\xD1\x83\xD1\x81\xD1\x81\xD0\xBA\xD0\xB8\xD0\xB9", "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E", "\xED\x95\x9C\xEA\xB5\xAD\xEC\x96\xB4", "\xE7\xAE\x80\xE4\xBD\x93\xE4\xB8\xAD\xE6\x96\x87", "\xE7\xB9\x81\xE9\xAB\x94\xE4\xB8\xAD\xE6\x96\x87" };
static const char *const FONTS[L_COUNT] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, "app0:assets/font-ja.ttf", "app0:assets/font-ko.ttf", "app0:assets/font-zhs.ttf", "app0:assets/font-zht.ttf" };

const char *tr(const char *s) {
  if (!s || cur == L_IT) return s;
  int lo = 0, hi = I18N_COUNT - 1;
  while (lo <= hi) {
    int mid = (lo + hi) / 2, c = strcmp(s, I18N_KEYS[mid]);
    if (!c) { const char *v = I18N_VAL[mid][cur]; if (!v) v = I18N_VAL[mid][L_EN]; return v ? v : s; }
    if (c < 0) hi = mid - 1; else lo = mid + 1;
  }
  return s;   // non e' una frase dell'interfaccia (nome, messaggio...): resta com'e'
}
int i18n_system_lang(void) {
  static int ready;
  if (!ready) { SceAppUtilInitParam ip; SceAppUtilBootParam bp; memset(&ip, 0, sizeof ip); memset(&bp, 0, sizeof bp); sceAppUtilInit(&ip, &bp); ready = 1; }
  int v = 1; sceAppUtilSystemParamGetInt(SCE_SYSTEM_PARAM_ID_LANG, &v);
  switch (v) {   // identificativi della Vita -> il nostro elenco
    case SCE_SYSTEM_PARAM_LANG_JAPANESE: return L_JA;               case SCE_SYSTEM_PARAM_LANG_ENGLISH_US: return L_EN;
    case SCE_SYSTEM_PARAM_LANG_FRENCH: return L_FR;                 case SCE_SYSTEM_PARAM_LANG_SPANISH: return L_ES;
    case SCE_SYSTEM_PARAM_LANG_GERMAN: return L_DE;                 case SCE_SYSTEM_PARAM_LANG_ITALIAN: return L_IT;
    case SCE_SYSTEM_PARAM_LANG_DUTCH: return L_NL;                  case SCE_SYSTEM_PARAM_LANG_PORTUGUESE_PT: return L_PT;
    case SCE_SYSTEM_PARAM_LANG_RUSSIAN: return L_RU;                case SCE_SYSTEM_PARAM_LANG_KOREAN: return L_KO;
    case SCE_SYSTEM_PARAM_LANG_CHINESE_T: return L_ZHT;             case SCE_SYSTEM_PARAM_LANG_CHINESE_S: return L_ZHS;
    case SCE_SYSTEM_PARAM_LANG_FINNISH: return L_FI;                case SCE_SYSTEM_PARAM_LANG_SWEDISH: return L_SV;
    case SCE_SYSTEM_PARAM_LANG_DANISH: return L_DA;                 case SCE_SYSTEM_PARAM_LANG_NORWEGIAN: return L_NO;
    case SCE_SYSTEM_PARAM_LANG_POLISH: return L_PL;                 case SCE_SYSTEM_PARAM_LANG_PORTUGUESE_BR: return L_PTBR;
    case SCE_SYSTEM_PARAM_LANG_ENGLISH_GB: return L_EN;             case SCE_SYSTEM_PARAM_LANG_TURKISH: return L_TR;
    default: return L_EN;
  }
}
void i18n_init(void) {
  char b[24] = { 0 }; SceUID f = sceIoOpen(LANG_FILE, SCE_O_RDONLY, 0);
  if (f >= 0) { int n = sceIoRead(f, b, sizeof b - 1); sceIoClose(f); if (n < 0) n = 0; b[n] = 0; for (char *p = b; *p; p++) if (*p == '\n' || *p == '\r' || *p == ' ') { *p = 0; break; } }
  auto_mode = 1; cur = i18n_system_lang();
  for (int i = 0; i < L_COUNT; i++) if (!strcmp(b, CODES[i])) { auto_mode = 0; cur = i; break; }
}
int i18n_lang(void) { return cur; }
int i18n_is_auto(void) { return auto_mode; }
void i18n_set(int lang) {
  const char *code = "auto";
  if (lang < 0 || lang >= L_COUNT) { auto_mode = 1; cur = i18n_system_lang(); } else { auto_mode = 0; cur = lang; code = CODES[lang]; }
  sceIoMkdir("ux0:data", 0777); sceIoMkdir("ux0:data/whatsapp", 0777);
  SceUID f = sceIoOpen(LANG_FILE, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666); if (f >= 0) { sceIoWrite(f, code, strlen(code)); sceIoClose(f); }
}
const char *i18n_code(int lang) { return lang >= 0 && lang < L_COUNT ? CODES[lang] : "en"; }
const char *i18n_native_name(int lang) { return lang >= 0 && lang < L_COUNT ? NAMES[lang] : ""; }
const char *i18n_font_path(int lang) { return lang >= 0 && lang < L_COUNT ? FONTS[lang] : 0; }
