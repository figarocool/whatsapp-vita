#include <psp2/sysmodule.h>
#ifndef SAFE_BUILD
#include <psp2/notificationutil.h>
#endif
// Schermate e azioni aggiuntive: schede in basso, ricerca, nuova chat/gruppo, info contatto, media, aggiornamenti,
// community, chiamate, menu della chat e popup. Incluso da main.c subito prima di main().

static float misc_scroll, misc_vel;
static void query2(const char *tag, const char *path); static unsigned my_q_frame; static jv *qres2_arr(const char *tag);
static char view_jid[160]; static int view_is_video;
static void vp_want(const char *jid, const char *id); static int vp_draw(float x, float y, float w, float h);
static char search_q[120], search_scope[160]; static int search_from = S_LIST;
static char nc_number[40]; static int nc_check; static char open_after[160]; static int open_after_t;
static char comm_sel[160], comm_name_tmp[120]; static int comm_sel_idx; static unsigned comm_refresh_at;
static int ime_next;                       // contesto IME da aprire al prossimo frame
static int contact_from = S_CHAT; static char contact_jid[160];
static char media_jid[160]; static int media_tab;
static int st_idx, st_item;
static int theme_idx;
static char gsubject[100]; static char gsel[40][160]; static int ngsel;
static int ncidx[400], ncn;
static int popup_open;

static const unsigned THEMES[6] = { RGBA8(0x0B,0x14,0x1A,255), RGBA8(0x0E,0x24,0x1F,255), RGBA8(0x0E,0x1C,0x30,255), RGBA8(0x22,0x16,0x2E,255), RGBA8(0x2A,0x1F,0x16,255), RGBA8(0x1C,0x1C,0x1C,255) };
static const unsigned THEMES_L[6] = { RGBA8(0xEF,0xEA,0xE2,255), RGBA8(0xE3,0xF1,0xE7,255), RGBA8(0xE3,0xED,0xF7,255), RGBA8(0xEF,0xE6,0xF5,255), RGBA8(0xF3,0xEA,0xE0,255), RGBA8(0xE6,0xE6,0xE6,255) };
static const char *THEME_NAMES[6] = { "Predefinito", "Verde", "Blu", "Viola", "Marrone", "Grafite" };

// ---------- query al bridge ----------
static void query(const char *tag, const char *path) {
  Job *j = job_new(J_QUERY, NULL); if (!j) { toast_set(T("Occupato, riprova")); return; }
  snprintf(j->id, sizeof j->id, "%s", tag); snprintf(j->path, sizeof j->path, "%s", path); job_commit();
  snprintf(qwant, sizeof qwant, "%s", tag); qwait = 1; q_frame = frame_no;
}
static void fire(const char *path, const char *body, const char *ok) {
  Job *j = job_new(J_QUERY, NULL); if (!j) { toast_set(T("Occupato, riprova")); return; }
  snprintf(j->path, sizeof j->path, "%s", path); snprintf(j->text, sizeof j->text, "%s", body); snprintf(j->extra, sizeof j->extra, "%s", ok); job_commit();
}
static void query_jid(const char *tag, const char *prefix, const char *jid, const char *suffix) { char e[400], p[600]; url_enc(jid, e, sizeof e); snprintf(p, sizeof p, "%s%s%s", prefix, e, suffix); query(tag, p); }
static void fire_jid(const char *path, const char *jid, const char *more, const char *ok) { char b[700]; snprintf(b, sizeof b, "{\"jid\":\"%s\"%s}", jid, more); fire(path, b, ok); }
static jv *qarr(const char *tag) { return (qres && !strcmp(qtag, tag) && qres->t == J_ARR) ? qres : NULL; }
static jv *qobj(const char *tag) { return (qres && !strcmp(qtag, tag) && qres->t == J_OBJ) ? qres : NULL; }

static int chat_index(const char *jid) { for (int i = 0; i < nchats; i++) if (!strcmp(chats[i].jid, jid)) return i; return -1; }
static void open_by_jid(const char *jid) { int i = chat_index(jid); if (i >= 0) open_chat(i); else toast_set(T("Chat non trovata")); }

// ---------- helper grafici ----------
static void misc_clamp(float total, float viewh) { float mx = total > viewh ? total - viewh : 0; if (misc_scroll < 0) { misc_scroll = 0; misc_vel = 0; } if (misc_scroll > mx) { misc_scroll = mx; misc_vel = 0; } }
static void top_bar(const char *title) { CHROME_BEGIN(); vita2d_draw_rectangle(0, 0, SW, 60, C_BAR); back_arrow(); btn(0, 0, 70, 60, B_BACK, 0); char t[200]; fit(title, 780, 22, t, sizeof t); textb(70, 16, C_TEXT, 22, t); CHROME_END(); }
static void people_row(float y, const char *jid, const char *name, const char *l1, const char *l2, const char *right, int ring) {
  if (ring) { vita2d_draw_fill_circle(44, y + ROWH / 2, 30, C_GREEN2); vita2d_draw_fill_circle(44, y + ROWH / 2, 28, C_BG); }
  draw_avatar(jid, name, 44, y + ROWH / 2, ring ? 25 : 26);
  char a[300]; fit(l1, SW - 88 - 90, 20, a, sizeof a); text(88, (int)y + (l2 && l2[0] ? 12 : 26), C_TEXT, 20, a);
  if (l2 && l2[0]) { char b[300]; fit(l2, SW - 88 - 70, 16, b, sizeof b); text(88, (int)y + 42, C_SUB, 16, b); }
  if (right && right[0]) text(SW - 16 - tw(13, right), (int)y + 14, C_SUB, 13, right);
}
static void label_row(float y, const char *t) { text(24, (int)y + 8, C_GREEN2, 16, t); }
static void icon_circle(float cx, float cy, unsigned col) { vita2d_draw_fill_circle(cx, cy, 26, col); }
static void wrap_text(const char *s, int x, int y, int w, int size, unsigned col, int lh, int center) {
  char line[400] = ""; const char *p = s;
  while (*p) {
    char word[200]; int wl = 0; while (*p && *p != ' ' && *p != '\n' && wl < 190) word[wl++] = *p++; word[wl] = 0;
    char cand[620]; snprintf(cand, sizeof cand, "%s%s%s", line, line[0] ? " " : "", word);
    if (line[0] && tw(size, cand) > w) { text(center ? x + (w - tw(size, line)) / 2 : x, y, col, size, line); y += lh; snprintf(line, sizeof line, "%s", word); }
    else snprintf(line, sizeof line, "%s", cand);
    if (*p == '\n') { text(center ? x + (w - tw(size, line)) / 2 : x, y, col, size, line); y += lh; line[0] = 0; p++; } else if (*p == ' ') p++;
  }
  if (line[0]) text(center ? x + (w - tw(size, line)) / 2 : x, y, col, size, line);
}

// ---------- schede in basso ----------
static int is_tab(int s) { return s == S_LIST || s == S_UPDATES || s == S_COMM || s == S_CALLS; }
// selezione con i tasti direzionali negli altri tab (Aggiornamenti, Community, Chiamate): ogni riga si registra con selrow()
typedef struct { int id, arg; } SelRow;
static SelRow srows[900]; static int nsr, tsel, tnav, tsel_dirty; static float sel_top, sel_vh;
static void sel_begin(float top) { nsr = 0; sel_top = top; sel_vh = SH - top - TABH; }
static void selrow(float y, float h, int id, int arg) {
  if (nsr >= 900) return; int i = nsr++; srows[i].id = id; srows[i].arg = arg;
  if (!tnav || i != tsel) return;
  if (tsel_dirty) { tsel_dirty = 0; misc_vel = 0; float rel = y + misc_scroll - sel_top; if (rel < misc_scroll) misc_scroll = rel; else if (rel + h > misc_scroll + sel_vh) misc_scroll = rel + h - sel_vh; }
  vita2d_draw_rectangle(0, y, SW, h, RGBA8(0x2B, 0x7B, 0xFF, 64)); vita2d_draw_rectangle(0, y, 6, h, RGBA8(0x2B, 0x7B, 0xFF, 255));   // riga selezionata: azzurro come nella lista chat
}
static void goto_tab(int i) {
  tsel = 0; tnav = 0; tsel_dirty = 0; misc_scroll = 0; misc_vel = 0; menu_open = 0;
  if (i == 0) screen = S_LIST;
  else if (i == 1) { screen = S_UPDATES; query("updates", "/updates"); query2("mystatus", "/mystatus"); my_q_frame = frame_no; }
  else if (i == 2) { screen = S_COMM; query("comm", "/communities"); }
  else { screen = S_CALLS; query("calls", "/calls"); }
}
static void draw_tabs(void) {
  CHROME_BEGIN();
  int y = SH - TABH; vita2d_draw_rectangle(0, y, SW, TABH, C_BAR);
  const char *lab[4] = { T("Chat"), T("Aggiornamenti"), T("Community"), T("Chiamate") };
  int cur = screen == S_LIST ? 0 : screen == S_UPDATES ? 1 : screen == S_COMM ? 2 : 3;
  int unread = 0, ch_unread = 0;   // il numero su 'Chat' conta solo le chat vere (come la lista); i canali stanno in Aggiornamenti e hanno il loro numero
  for (int i = 0; i < nchats; i++) if (chats[i].unread > 0 && !chats[i].archived) { if (chats[i].channel) ch_unread++; else unread++; }
  for (int i = 0; i < 4; i++) {
    int x = i * SW / 4, cx = x + SW / 8; unsigned col = i == cur ? C_GREEN2 : C_SUB;
    if (i == cur) rrect(cx - 28, y + 4, 56, 26, 13, C_CHIPSEL);
    if (i == 0) { rrect(cx - 10, y + 10, 20, 14, 4, col); vita2d_draw_rectangle(cx - 8, y + 22, 6, 5, col); }
    else if (i == 1) { vita2d_draw_fill_circle(cx, y + 17, 10, col); vita2d_draw_fill_circle(cx, y + 17, 7, i == cur ? C_CHIPSEL : C_BAR); vita2d_draw_fill_circle(cx, y + 17, 3, col); }
    else if (i == 2) { vita2d_draw_fill_circle(cx - 6, y + 14, 5, col); vita2d_draw_fill_circle(cx + 6, y + 14, 5, col); rrect(cx - 12, y + 20, 24, 7, 3, col); }
    else { rrect(cx - 5, y + 8, 10, 19, 3, col); vita2d_draw_rectangle(cx - 3, y + 11, 6, 10, i == cur ? C_CHIPSEL : C_BAR); }
    int w = tw(12, lab[i]); text(cx - w / 2, y + 36, col, 12, lab[i]);
    { int nb = i == 0 ? unread : i == 1 ? ch_unread : 0; if (nb > 0) { char u[8]; snprintf(u, sizeof u, "%d", nb > 99 ? 99 : nb); int uw = tw(11, u) + 10; if (uw < 18) uw = 18; rrect(cx + 8, y + 2, uw, 18, 9, C_GREEN2); text(cx + 8 + (uw - tw(11, u)) / 2, y + 4, C_BG, 11, u); } }
    if (0 && unread > 0) { char u[8]; snprintf(u, sizeof u, "%d", unread > 99 ? 99 : unread); int uw = tw(11, u) + 10; if (uw < 18) uw = 18; rrect(cx + 8, y + 2, uw, 18, 9, C_GREEN2); text(cx + 8 + (uw - tw(11, u)) / 2, y + 4, C_BG, 11, u); }
    btn(x, y, SW / 4, TABH, B_TAB, i);
  }
  CHROME_END();
}

// ---------- aggiornamenti: storie (stati) e canali ----------
static void fmt_ago(long ts, char *o, size_t n) { fmt_time(ts, o, n, 1); }
static jv *qres2_arr(const char *tag) { return (qres2 && !strcmp(qtag2, tag) && qres2->t == J_ARR) ? qres2 : NULL; }
static void query2(const char *tag, const char *path) {     // seconda area risultati (mie storie, visualizzazioni)
  Job *j = job_new(J_QUERY, NULL); if (!j) return;
  snprintf(j->id, sizeof j->id, "%s", tag); snprintf(j->path, sizeof j->path, "%s", path); job_commit();
  snprintf(qwant2, sizeof qwant2, "%s", tag);
}
static unsigned refresh_my_at, my_q_frame; static int viewers_wait, receipts_on = 1, st_mine;
static char story_text[1000];
// storie gia' viste (memorizzate per non ripresentarle come nuove)
static unsigned seen_h[800]; static int nseen;
static unsigned hid(const char *s) { unsigned h = 5381; while (*s) h = h * 33 + (unsigned char)*s++; return h ? h : 1; }
static int is_seen(const char *id) { unsigned h = hid(id); for (int i = 0; i < nseen; i++) if (seen_h[i] == h) return 1; return 0; }
static void seen_save(void) { SceUID f = sceIoOpen("ux0:data/whatsapp/seen.bin", SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777); if (f >= 0) { sceIoWrite(f, seen_h, nseen * sizeof(unsigned)); sceIoClose(f); } }
static void mark_seen(const char *id) {
  if (is_seen(id)) return;
  if (nseen >= 800) { memmove(seen_h, seen_h + 100, (nseen - 100) * sizeof(unsigned)); nseen -= 100; }
  seen_h[nseen++] = hid(id); seen_save();
}
static int contact_seen(jv *o) { jv *its = json_get(o, "items"); if (!its || its->count == 0) return 1; for (int i = 0; i < its->count; i++) if (!is_seen(json_str(its->items[i], "id", ""))) return 0; return 1; }

enum { U_LABEL, U_MY, U_CONTACT, U_CHANNEL, U_FOLLOW };
typedef struct { int type, idx; } URow;
static URow urows[900]; static int nur;
static void draw_updates(void) {
  { CHROME_BEGIN(); textb(20, 14, C_GREEN2, 30, T("Aggiornamenti")); CHROME_END(); }
  jv *a = qarr("updates"); jv *my = qres2_arr("mystatus"); int na = a ? a->count : 0;
  nur = 0; urows[nur++] = (URow){ U_LABEL, 0 }; urows[nur++] = (URow){ U_MY, 0 };
  int ns = 0; for (int i = 0; i < na && nur < 880; i++) if (!contact_seen(a->items[i])) { if (!ns++) urows[nur++] = (URow){ U_LABEL, 1 }; urows[nur++] = (URow){ U_CONTACT, i }; }
  int nv = 0; for (int i = 0; i < na && nur < 880; i++) if (contact_seen(a->items[i])) { if (!nv++) urows[nur++] = (URow){ U_LABEL, 2 }; urows[nur++] = (URow){ U_CONTACT, i }; }
  urows[nur++] = (URow){ U_LABEL, 3 };
  for (int i = 0; i < nchats && nur < 880; i++) if (chats[i].channel) urows[nur++] = (URow){ U_CHANNEL, i };
  urows[nur++] = (URow){ U_FOLLOW, 0 };
  float top = HDR + 4, total = 0; for (int i = 0; i < nur; i++) total += urows[i].type == U_LABEL ? 34 : ROWH;
  misc_clamp(total, SH - top - TABH);
  vita2d_enable_clipping(); vita2d_set_clip_rectangle(0, top, SW, SH - TABH); clip_y0 = top; clip_y1 = SH - TABH;
  float y = top - misc_scroll; sel_begin(top);
  for (int r = 0; r < nur; r++) {
    int h = urows[r].type == U_LABEL ? 34 : ROWH;
    { URow *s_ = &urows[r]; if (s_->type == U_MY) selrow(y, h, B_UPD_MY, 0); else if (s_->type == U_CONTACT) selrow(y, h, B_UPD_ROW, s_->idx); else if (s_->type == U_CHANNEL) selrow(y, h, B_ROW, s_->idx); else if (s_->type == U_FOLLOW) selrow(y, h, B_FOLLOW_CH, 0); }
    if (y + h >= top && y <= SH) {
      URow *u = &urows[r];
      if (u->type == U_LABEL) { const char *lb[4] = { T("Stato"), T("Recenti"), T("Visti"), T("Canali") }; label_row(y, lb[u->idx]); }
      else if (u->type == U_MY) {
        int cnt = my ? my->count : 0; int views = 0; long lastts = 0; for (int i = 0; i < cnt; i++) { views += (int)json_num(my->items[i], "views", 0); lastts = (long)json_num(my->items[i], "ts", 0); }
        char sub[120], tm[16] = ""; if (cnt) { fmt_ago(lastts, tm, sizeof tm); snprintf(sub, sizeof sub, "%d aggiorname%s - %s - visualizzazioni: %d", cnt, cnt == 1 ? "nto" : "nti", tm, views); } else snprintf(sub, sizeof sub, T("Tocca + per aggiungere un aggiornamento"));
        if (cnt) { vita2d_draw_fill_circle(44, y + ROWH / 2, 30, C_GREEN2); vita2d_draw_fill_circle(44, y + ROWH / 2, 28, C_BG); }
        draw_avatar(prof.jid[0] ? prof.jid : "me", prof.name, 44, y + ROWH / 2, cnt ? 25 : 26);
        char nm[100]; snprintf(nm, sizeof nm, T("La mia storia")); text(88, (int)y + 12, C_TEXT, 20, nm); char sb[200]; fit(sub, SW - 88 - 100, 15, sb, sizeof sb); text(88, (int)y + 42, C_SUB, 15, sb);
        btn(0, (int)y, SW, ROWH, B_UPD_MY, 0);
        vita2d_draw_fill_circle(SW - 44, y + ROWH / 2, 24, C_GREEN); vita2d_draw_rectangle(SW - 56, y + ROWH / 2 - 2, 24, 4, C_WHITE); vita2d_draw_rectangle(SW - 46, y + ROWH / 2 - 12, 4, 24, C_WHITE);
        btn(SW - 80, (int)y, 80, ROWH, B_STORY_NEW, 0);
      } else if (u->type == U_CONTACT) {
        jv *o = a->items[u->idx]; jv *its = json_get(o, "items"); int cnt = its ? its->count : 0;
        char sub[100], tm[16] = ""; if (cnt) fmt_ago((long)json_num(its->items[cnt - 1], "ts", 0), tm, sizeof tm);
        snprintf(sub, sizeof sub, "%d aggiorname%s - %s", cnt, cnt == 1 ? "nto" : "nti", tm);
        int seen = contact_seen(o); unsigned rc = seen ? C_SUB : C_GREEN2; const char *jd = json_str(o, "jid", ""), *nm = json_str(o, "name", "");
        vita2d_draw_fill_circle(44, y + ROWH / 2, 30, rc); vita2d_draw_fill_circle(44, y + ROWH / 2, 28, C_BG); draw_avatar(jd, nm, 44, y + ROWH / 2, 25);
        char n2[200]; fit(nm, SW - 88 - 40, 20, n2, sizeof n2); text(88, (int)y + 10, C_TEXT, 20, n2); text(88, (int)y + 38, C_SUB, 16, sub);
        btn(0, (int)y, SW, ROWH, B_UPD_ROW, u->idx);
      } else if (u->type == U_CHANNEL) {
        Chat *c = &chats[u->idx]; draw_avatar(c->jid, c->name, 44, y + ROWH / 2, 26);
        char n2[200]; fit(c->name, SW - 88 - 90, 20, n2, sizeof n2); text(88, (int)y + 10, C_TEXT, 20, n2);
        char ls[300]; fit(c->last[0] ? c->last : T("Canale"), SW - 88 - 70, 16, ls, sizeof ls); text(88, (int)y + 38, C_SUB, 16, ls);
        char tm[16] = ""; if (c->ts) fmt_time(c->ts, tm, sizeof tm, 1); text(SW - 16 - tw(13, tm), (int)y + 14, c->unread > 0 ? C_GREEN2 : C_SUB, 13, tm);
        btn(0, (int)y, SW, ROWH, B_ROW, u->idx);
      } else { icon_circle(44, y + ROWH / 2, C_GREEN); text(35, (int)y + ROWH / 2 - 18, C_WHITE, 34, "+"); text(88, (int)y + 26, C_TEXT, 19, T("Segui un canale (incolla il link)")); btn(0, (int)y, SW, ROWH, B_FOLLOW_CH, 0); }
    }
    y += h;
  }
  vita2d_disable_clipping(); clip_y0 = 0; clip_y1 = SH;
  if (!a && qwait) text(88, 90, C_SUB, 14, T("Caricamento..."));
  draw_tabs();
  if (!qwait && frame_no - q_frame > 900) query("updates", "/updates");
  if (frame_no - my_q_frame > 900) { my_q_frame = frame_no; query2("mystatus", "/mystatus"); }
}
static void draw_statusview(void) {
  jv *its = NULL, *o = NULL; const char *nm = "", *jd = "";
  if (st_mine) { its = qres2_arr("mystatus"); nm = T("La mia storia"); jd = prof.jid; }
  else { jv *a = qarr("updates"); o = a && st_idx < a->count ? a->items[st_idx] : NULL; its = o ? json_get(o, "items") : NULL; nm = o ? json_str(o, "name", "") : ""; jd = o ? json_str(o, "jid", "") : ""; }
  if (!its || its->count == 0) { screen = S_UPDATES; return; }
  if (st_item >= its->count) st_item = its->count - 1; if (st_item < 0) st_item = 0;
  jv *it = its->items[st_item]; const char *kind = json_str(it, "kind", "text"), *id = json_str(it, "id", ""), *txt = json_str(it, "text", "");
  int img = !strcmp(kind, "image") || !strcmp(kind, "video");
  if (!st_mine) { static char last[64]; if (strcmp(last, id)) { snprintf(last, sizeof last, "%s", id); mark_seen(id); if (receipts_on) { char b[200]; snprintf(b, sizeof b, "{\"id\":\"%s\"}", id); fire("/status/view", b, ""); } } }
  vita2d_draw_rectangle(0, 0, SW, SH, img ? RGBA8(0, 0, 0, 255) : hash_col(nm));
  if (img) {
    Media *md = media_get("status@broadcast", id);
    if (md && md->tex) { float w0 = vita2d_texture_get_width(md->tex), h0 = vita2d_texture_get_height(md->tex); float s = fminf((SW - 40) / w0, (SH - 170) / h0); vita2d_draw_texture_scale(md->tex, (SW - w0 * s) / 2, 80 + (SH - 170 - h0 * s) / 2, s, s); }
    else text(SW / 2 - 40, SH / 2 - 10, C_SUB, 20, md && md->state == 2 ? T("Errore") : "...");
    if (!strcmp(kind, "video")) { vp_want("status@broadcast", id); vp_draw(0, 0, SW, SH); }
    if (txt[0]) { vita2d_draw_rectangle(0, SH - 90, SW, 90, RGBA8(0, 0, 0, 150)); wrap_text(txt, 40, SH - 78, SW - 80, 20, C_WHITE, 26, 1); }
  } else wrap_text(txt, 80, 150, SW - 160, 32, C_WHITE, 42, 1);
  int n = its->count, seg = (SW - 40 - (n - 1) * 4) / n;
  for (int i = 0; i < n; i++) vita2d_draw_rectangle(20 + i * (seg + 4), 10, seg, 4, i <= st_item ? C_WHITE : RGBA8(255, 255, 255, 90));
  draw_avatar(jd, nm, 44, 44, 18); textb(74, 26, C_WHITE, 20, nm);
  char tm[16]; fmt_ago((long)json_num(it, "ts", 0), tm, sizeof tm); text(74, 50, RGBA8(255, 255, 255, 200), 13, tm);
  btn(0, 0, SW / 3, SH, B_ST_PREV, 0); btn(SW / 3, 0, SW - SW / 3, SH, B_ST_NEXT, 0);
  if (st_mine) {
    char v[60]; snprintf(v, sizeof v, T("Visto da %d"), (int)json_num(it, "views", 0)); int w = tw(18, v) + 50;
    rrect((SW - w) / 2, SH - 62, w, 44, 22, RGBA8(32, 32, 32, 255)); text((SW - w) / 2 + 25, SH - 51, C_WHITE, 18, v); btn((SW - w) / 2, SH - 62, w, 44, B_ST_VIEWERS, 0);
  }
}

// ---------- community ----------
static void draw_comm(void) {
  { CHROME_BEGIN(); textb(20, 14, C_GREEN2, 30, T("Community")); rrect(SW - 190, 14, 170, 40, 20, C_GREEN2); text(SW - 190 + (170 - tw(17, T("Nuova"))) / 2, 24, C_WHITE, 17, T("Nuova")); btn(SW - 190, 14, 170, 40, B_COMM_NEW, 0); CHROME_END(); }
  jv *a = qarr("comm"); float top = HDR + 6; int rows = 0;
  if (a) for (int i = 0; i < a->count; i++) { jv *g = json_get(a->items[i], "groups"); rows += 1 + (g ? g->count : 0); }
  misc_clamp(rows * 64, SH - top - TABH);
  vita2d_enable_clipping(); vita2d_set_clip_rectangle(0, top, SW, SH - TABH); clip_y0 = top; clip_y1 = SH - TABH;
  float y = top - misc_scroll; sel_begin(top);
  if (a) for (int i = 0; i < a->count; i++) {
    jv *c = a->items[i]; jv *g = json_get(c, "groups");
    selrow(y, 64, B_COMM_HEAD, i);
    if (y + 64 >= top && y <= SH) { rrect(24, y + 8, 48, 48, 14, hash_col(json_str(c, "subject", ""))); char sub[60]; snprintf(sub, sizeof sub, "%d membri", (int)json_num(c, "size", 0)); char nm[200]; fit(json_str(c, "subject", ""), 640, 20, nm, sizeof nm); text(88, (int)y + 8, C_TEXT, 20, nm); text(88, (int)y + 36, C_SUB, 14, sub); btn(0, (int)y, SW, 64, B_COMM_HEAD, i); }
    y += 64;
    for (int k = 0; g && k < g->count; k++) {
      selrow(y, 64, B_COMM_ROW, i * 100 + k);
      if (y + 64 >= top && y <= SH) { char nm[200]; fit(json_str(g->items[k], "subject", ""), 600, 18, nm, sizeof nm); draw_avatar(json_str(g->items[k], "id", ""), nm, 120, y + 32, 20); text(156, (int)y + 20, C_TEXT, 18, nm); btn(0, (int)y, SW, 64, B_COMM_ROW, i * 100 + k); }
      y += 64;
    }
  }
  vita2d_disable_clipping(); clip_y0 = 0; clip_y1 = SH;
  if (!a || a->count == 0) { text(60, 200, C_SUB, 20, qwait ? T("Caricamento...") : T("Nessuna community")); text(60, 236, C_SUB, 14, T("Qui compaiono le community a cui partecipi. Con Nuova ne crei una.")); }
  draw_tabs();
  if (comm_refresh_at && frame_no >= comm_refresh_at) { comm_refresh_at = 0; query("comm", "/communities"); }
  if (!qwait && frame_no - q_frame > 1800) query("comm", "/communities");
}

// ---------- chiamate ----------
static void draw_calls(void) {
  { CHROME_BEGIN(); textb(20, 14, C_GREEN2, 30, T("Chiamate")); CHROME_END(); }
  text(20, HDR + 2, C_SUB, 14, T("Dalla Vita non si fanno chiamate: qui vedi il registro di quelle registrate."));
  jv *a = qarr("calls"); float top = HDR + 30; int n = a ? a->count : 0;
  misc_clamp(n * ROWH, SH - top - TABH);
  vita2d_enable_clipping(); vita2d_set_clip_rectangle(0, top, SW, SH - TABH); clip_y0 = top; clip_y1 = SH - TABH;
  sel_begin(top);
  for (int i = 0; i < n; i++) {
    float y = top + i * ROWH - misc_scroll; selrow(y, ROWH, B_CALL_ROW, i); if (y + ROWH < top || y > SH) continue;
    jv *o = a->items[i]; const char *s = json_str(o, "status", "");
    const char *st = !strcmp(s, "timeout") ? T("Persa") : !strcmp(s, "reject") ? T("Rifiutata") : !strcmp(s, "accept") ? T("Risposta") : !strcmp(s, "terminate") ? T("Terminata") : T("In arrivo");
    char sub[96], tm[16]; int dur = (int)json_num(o, "dur", 0); char ds[16] = ""; if (dur > 0) snprintf(ds, sizeof ds, " (%d:%02d)", dur / 60, dur % 60); snprintf(sub, sizeof sub, "%s%s - %s%s", json_bool(o, "out", 0) ? T("In uscita: ") : "", json_bool(o, "video", 0) ? T("Video") : T("Voce"), st, ds); fmt_ago((long)json_num(o, "ts", 0), tm, sizeof tm);
    people_row(y, json_str(o, "jid", ""), json_str(o, "name", ""), json_str(o, "name", ""), sub, tm, 0);
  }
  vita2d_disable_clipping(); clip_y0 = 0; clip_y1 = SH;
  if (n == 0) text(60, 220, C_SUB, 20, qwait ? T("Caricamento...") : T("Nessuna chiamata registrata"));
  draw_tabs();
  if (!qwait && frame_no - q_frame > 900) query("calls", "/calls");
}

// ---------- ricerca ----------
static void draw_search(void) {
  CHROME_BEGIN();
  vita2d_draw_rectangle(0, 0, SW, 60, C_BAR); back_arrow(); btn(0, 0, 70, 60, B_BACK, 0);
  rrect(76, 10, SW - 96, 40, 20, C_CHIP);
  char q[200]; fit(search_q[0] ? search_q : (search_scope[0] ? T("Cerca nella chat...") : T("Cerca...")), SW - 150, 19, q, sizeof q); text(98, 19, search_q[0] ? C_TEXT : C_SUB, 19, q); btn(76, 0, SW - 96, 60, B_SEARCH_BOX, 0);
  CHROME_END();
  jv *o = qobj("search"); jv *cs = o ? json_get(o, "chats") : NULL, *ms = o ? json_get(o, "messages") : NULL;
  int nc = cs ? cs->count : 0, nm = ms ? ms->count : 0;
  float total = (nc ? 34 + nc * ROWH : 0) + (nm ? 34 + nm * ROWH : 0); misc_clamp(total, SH - HDR);
  vita2d_enable_clipping(); vita2d_set_clip_rectangle(0, HDR, SW, SH); clip_y0 = HDR;
  float y = HDR - misc_scroll;
  if (nc) { label_row(y, T("Chat")); y += 34; for (int i = 0; i < nc; i++, y += ROWH) if (y + ROWH >= HDR && y <= SH) { people_row(y, json_str(cs->items[i], "jid", ""), json_str(cs->items[i], "name", ""), json_str(cs->items[i], "name", ""), NULL, NULL, 0); btn(0, (int)y, SW, ROWH, B_SEARCH_ROW, i); } }
  if (nm) { label_row(y, T("Messaggi")); y += 34; for (int i = 0; i < nm; i++, y += ROWH) if (y + ROWH >= HDR && y <= SH) { jv *r = ms->items[i]; char tm[16]; fmt_ago((long)json_num(r, "ts", 0), tm, sizeof tm); people_row(y, json_str(r, "jid", ""), json_str(r, "chat", ""), json_str(r, "chat", ""), json_str(r, "text", ""), tm, 0); btn(0, (int)y, SW, ROWH, B_SEARCH_ROW, 1000 + i); } }
  vita2d_disable_clipping(); clip_y0 = 0;
  if (!nc && !nm) text(60, 200, C_SUB, 20, qwait ? T("Ricerca...") : (o ? T("Nessun risultato") : T("Tocca la barra per cercare")));
}

// ---------- nuova chat / nuovo gruppo ----------
static int cmp_chat_name(const void *a, const void *b) { return strcasecmp(chats[*(const int *)a].name, chats[*(const int *)b].name); }
static void build_contacts(void) { if (chat_limit < 400) chat_limit = 400; ncn = 0; for (int i = 0; i < nchats && ncn < 400; i++) if (!chats[i].group && !chats[i].channel) ncidx[ncn++] = i; qsort(ncidx, ncn, sizeof(int), cmp_chat_name); }
static void draw_newchat(void) {
  top_bar(T("Nuova chat"));
  float total = 2 * ROWH + 34 + ncn * ROWH; misc_clamp(total, SH - HDR);
  vita2d_enable_clipping(); vita2d_set_clip_rectangle(0, HDR, SW, SH); clip_y0 = HDR;
  float y = HDR - misc_scroll;
  icon_circle(44, y + ROWH / 2, C_GREEN); vita2d_draw_fill_circle(36, y + ROWH / 2 - 4, 6, C_WHITE); vita2d_draw_fill_circle(52, y + ROWH / 2 - 4, 6, C_WHITE); rrect(28, y + ROWH / 2 + 4, 32, 9, 4, C_WHITE);
  text(88, (int)y + 26, C_TEXT, 20, T("Nuovo gruppo")); btn(0, (int)y, SW, ROWH, B_NC_GROUP, 0); y += ROWH;
  icon_circle(44, y + ROWH / 2, C_GREEN); text(35, (int)y + ROWH / 2 - 18, C_WHITE, 34, "+");
  text(88, (int)y + 26, C_TEXT, 20, T("Nuovo contatto (inserisci numero)")); btn(0, (int)y, SW, ROWH, B_NC_CONTACT, 0); y += ROWH;
  label_row(y, T("Contatti su WhatsApp")); y += 34;
  for (int i = 0; i < ncn; i++, y += ROWH) if (y + ROWH >= HDR && y <= SH) { Chat *c = &chats[ncidx[i]]; people_row(y, c->jid, c->name, c->name, NULL, NULL, 0); btn(0, (int)y, SW, ROWH, B_NC_ROW, ncidx[i]); }
  vita2d_disable_clipping(); clip_y0 = 0;
}
static int gsel_has(const char *jid) { for (int i = 0; i < ngsel; i++) if (!strcmp(gsel[i], jid)) return i; return -1; }
static void draw_group(void) {
  top_bar(T("Nuovo gruppo"));
  rrect(20, HDR + 10, SW - 40, 48, 12, C_BAR); char s[200]; fit(gsubject[0] ? gsubject : T("Nome del gruppo (tocca per scrivere)"), SW - 90, 19, s, sizeof s); text(40, HDR + 22, gsubject[0] ? C_TEXT : C_SUB, 19, s); btn(20, HDR + 10, SW - 40, 48, B_GR_SUBJECT, 0);
  char cnt[60]; snprintf(cnt, sizeof cnt, T("Partecipanti: %d (max 40)"), ngsel); text(24, HDR + 68, C_SUB, 15, cnt);
  float top = HDR + 94; misc_clamp(ncn * ROWH, SH - top);
  vita2d_enable_clipping(); vita2d_set_clip_rectangle(0, top, SW, SH); clip_y0 = top;
  for (int i = 0; i < ncn; i++) {
    float y = top + i * ROWH - misc_scroll; if (y + ROWH < top || y > SH) continue;
    Chat *c = &chats[ncidx[i]]; people_row(y, c->jid, c->name, c->name, NULL, NULL, 0);
    int on = gsel_has(c->jid) >= 0; rrect(SW - 56, y + ROWH / 2 - 14, 28, 28, 6, on ? C_GREEN : C_BAR);
    if (on) { tline(SW - 50, y + ROWH / 2, SW - 44, y + ROWH / 2 + 6, C_WHITE); tline(SW - 44, y + ROWH / 2 + 6, SW - 34, y + ROWH / 2 - 6, C_WHITE); }
    btn(0, (int)y, SW, ROWH, B_GR_ROW, ncidx[i]);
  }
  vita2d_disable_clipping(); clip_y0 = 0;
  int ok = gsubject[0] && ngsel > 0; vita2d_draw_fill_circle(SW - 50, SH - 50, 30, ok ? C_GREEN : C_BAR);
  tline(SW - 62, SH - 50, SW - 54, SH - 42, C_WHITE); tline(SW - 54, SH - 42, SW - 38, SH - 60, C_WHITE); btn(SW - 85, SH - 85, 70, 70, B_GR_CREATE, 0);
}

// ---------- info contatto / gruppo ----------
static void draw_contact(void) {
  top_bar(T("Info"));
  jv *o = qobj("contact"); int group = o && json_bool(o, "group", 0);
  jv *ps = o ? json_get(o, "participants") : NULL; int np = ps ? ps->count : 0;
  float total = 300 + (group ? 80 + np * 48 : 90); misc_clamp(total, SH - HDR);
  vita2d_enable_clipping(); vita2d_set_clip_rectangle(0, HDR, SW, SH); clip_y0 = HDR;
  float y = HDR - misc_scroll;
  const char *nm = o ? json_str(o, "name", "") : ""; draw_avatar(contact_jid, nm, SW / 2, y + 90, 70);
  char n2[200]; fit(nm, 700, 28, n2, sizeof n2); text((SW - tw(28, n2)) / 2, (int)y + 172, C_TEXT, 28, n2);
  if (o) {
    const char *ph = json_str(o, "phone", ""); if (ph[0]) text((SW - tw(18, ph)) / 2, (int)y + 212, C_SUB, 18, ph);
    const char *ab = group ? json_str(o, "desc", "") : json_str(o, "about", ""); y += 250;
    if (ab[0]) { text(30, (int)y, C_GREEN2, 14, group ? T("Descrizione") : T("Info")); wrap_text(ab, 30, (int)y + 22, SW - 60, 18, C_TEXT, 24, 0); }
    y += 80;
    if (group) { char hd[60]; snprintf(hd, sizeof hd, "%d partecipanti", np); text(30, (int)y, C_GREEN2, 14, hd); y += 28; for (int i = 0; i < np; i++, y += 48) if (y + 48 >= HDR && y <= SH) { char pn[200]; fit(json_str(ps->items[i], "name", ""), 600, 18, pn, sizeof pn); text(40, (int)y + 10, C_TEXT, 18, pn); if (json_bool(ps->items[i], "admin", 0)) text(SW - 150, (int)y + 12, C_GREEN2, 14, T("Amministratore")); } }
    else { int bl = json_bool(o, "blocked", 0); text(30, (int)y + 10, C_RED, 20, bl ? T("Sblocca contatto") : T("Blocca contatto")); btn(0, (int)y, SW, 48, B_CT_BLOCK, bl); }
  } else text(SW / 2 - 50, (int)y + 250, C_SUB, 18, qwait ? T("Caricamento...") : T("Dati non disponibili"));
  vita2d_disable_clipping(); clip_y0 = 0;
}

// ---------- media, documenti, link ----------
static void media_query(void) { const char *k = media_tab == 0 ? "media" : media_tab == 1 ? "docs" : "links"; char s[40]; snprintf(s, sizeof s, "&kind=%s", k); char e[400], p[600]; url_enc(media_jid, e, sizeof e); snprintf(p, sizeof p, "/list/%s?%s", e, s + 1); query("list", p); }
static void draw_media(void) {
  top_bar(T("Media, link e documenti"));
  const char *tabs[3] = { T("Media"), T("Documenti"), T("Link") }; int cx = 16;
  for (int i = 0; i < 3; i++) { int w = tw(16, tabs[i]) + 28; rrect(cx, HDR + 6, w, 34, 17, media_tab == i ? C_CHIPSEL : C_CHIP); text(cx + 14, HDR + 12, media_tab == i ? C_GREEN2 : C_SUB, 16, tabs[i]); btn(cx, HDR, w, 46, B_MEDIA_TAB, i); cx += w + 8; }
  jv *a = qarr("list"); int n = a ? a->count : 0; float top = HDR + 52;
  int cols = UK >= 1.8f ? 3 : UK >= 1.3f ? 4 : 5; int cell = (SW - 24) / cols; int thsz = cell - 8; float total = media_tab == 0 ? ((n + cols - 1) / cols) * cell : n * 64.0f; misc_clamp(total, SH - top);
  vita2d_enable_clipping(); vita2d_set_clip_rectangle(0, top, SW, SH); clip_y0 = top;
  for (int i = 0; i < n; i++) {
    jv *o = a->items[i];
    if (media_tab == 0) {
      float x = 12 + (i % cols) * cell, y = top + (i / cols) * cell - misc_scroll; if (y + cell < top || y > SH) continue;
      Media *md = media_get(media_jid, json_str(o, "id", ""));
      if (md && md->tex) { float w0 = vita2d_texture_get_width(md->tex), h0 = vita2d_texture_get_height(md->tex); float sc = fminf(thsz / w0, thsz / h0); vita2d_draw_texture_scale(md->tex, x + (thsz - w0 * sc) / 2, y + (thsz - h0 * sc) / 2, sc, sc); }
      else rrect(x, y, thsz, thsz, 6, RGBA8(0, 0, 0, 90));
      if (!strcmp(json_str(o, "kind", ""), "video")) { vita2d_draw_fill_circle(x + thsz / 2, y + thsz / 2, 22, RGBA8(0, 0, 0, 150)); text((int)x + thsz / 2 - 7, (int)y + thsz / 2 - 13, C_WHITE, 24, ">"); }
      btn((int)x, (int)y, thsz, thsz, B_MEDIA_ITEM, i);
    } else {
      float y = top + i * 64 - misc_scroll; if (y + 64 < top || y > SH) continue;
      char tm[16]; fmt_ago((long)json_num(o, "ts", 0), tm, sizeof tm);
      if (media_tab == 1) { rrect(24, y + 10, 34, 42, 5, C_RED); text(28, (int)y + 24, C_WHITE, 14, "DOC"); char fn[200]; fit(json_str(o, "fname", ""), 700, 18, fn, sizeof fn); text(76, (int)y + 10, C_TEXT, 18, fn); char sz[60]; snprintf(sz, sizeof sz, "%.1f KB - %s", json_num(o, "size", 0) / 1024.0, tm); text(76, (int)y + 36, C_SUB, 13, sz); btn(0, (int)y, SW, 64, B_DOC_ITEM, i); }
      else { char ln[300]; fit(json_str(o, "text", ""), SW - 120, 17, ln, sizeof ln); text(30, (int)y + 10, C_BLUE, 17, ln); text(30, (int)y + 36, C_SUB, 13, tm); }
    }
  }
  vita2d_disable_clipping(); clip_y0 = 0;
  if (n == 0) text(60, 220, C_SUB, 20, qwait ? T("Caricamento...") : (media_tab == 0 ? T("Nessun media") : media_tab == 1 ? T("Nessun documento") : T("Nessun link")));
}

// ---------- visualizzatore immagine (chat e media) ----------
static void draw_viewer(void) {
  static int wo;
  if (!viewer_open) { wo = 0; return; }
  if (!wo) { wo = 1; vz = 1; vpx = vpy = 0; vpinch = vtouch = vmoved = 0; }
  btn_reset();
  Media *md = media_get(view_jid, view_key_id);
  if (md && !view_is_video && md->tex && md->bstate == 0) md->bstate = 4;   // chiede al bridge la versione grande
  vita2d_draw_rectangle(0, 0, SW, SH, RGBA8(0, 0, 0, 255));
  vita2d_texture *vt = md ? (md->big ? md->big : md->tex) : NULL;
  if (vt) {
    float tw0 = vita2d_texture_get_width(vt), th0 = vita2d_texture_get_height(vt);
    float s = fminf((SW - 40) / tw0, (SH - 140) / th0) * vz, w = tw0 * s, h = th0 * s;
    float ccx = SW / 2.0f, ccy = 36 + (SH - 140) / 2.0f;
    if (w > SW) { float mx = (w - SW) / 2; if (vpx > mx) vpx = mx; if (vpx < -mx) vpx = -mx; } else vpx = 0;   // non si esce dai bordi dello schermo
    if (h > SH) { float hi = h / 2 - ccy, lo = SH - ccy - h / 2; if (vpy > hi) vpy = hi; if (vpy < lo) vpy = lo; } else vpy = 0;
    if (vz <= 1.01f) { vpx = 0; vpy = 0; }
    vita2d_draw_texture_scale(vt, ccx + vpx - w / 2, ccy + vpy - h / 2, s / 1.0f, s / 1.0f);
    if (md && !md->big && (md->bstate == 4 || md->bstate == 3)) text(SW / 2 - 70, 36 + (SH - 140) / 2.0f - 10, RGBA8(210, 210, 210, 255), 16, T("Carico la foto..."));
  }
  if (view_is_video) { vp_want(view_jid, view_key_id); vp_draw(0, 0, SW, SH - 70); }
  btn(0, 0, SW, SH, B_VIEW_CLOSE, 0);
  { const char *lab[3] = { view_is_video ? T("Salva video") : T("Salva foto"), T("Rispondi"), T("Reagisci") }; int ids[3] = { B_VIEW_SAVE, B_VIEW_REPLY, B_VIEW_REACT }; int gap = 10, bw = (SW - 4 * gap) / 3; if (bw > 210) bw = 210; int x0 = (SW - 3 * bw - 2 * gap) / 2;
    for (int i = 0; i < 3; i++) { int bx = x0 + i * (bw + gap); rrect(bx, SH - 60, bw, 44, 22, RGBA8(40, 40, 40, 255)); text(bx + (bw - tw(18, lab[i])) / 2, SH - 49, C_WHITE, 18, lab[i]); btn(bx, SH - 60, bw, 44, ids[i], 0); } }
  text(SW / 2 - 130, 14, RGBA8(200, 200, 200, 255), 15, T("Tocca fuori dai pulsanti o premi Cerchio per chiudere"));
}

enum { A_CLOSE = 1, A_NEW_GROUP, A_VIEW_CONTACT, A_SEARCH, A_MEDIA, A_MUTE_MENU, A_MUTE_8H, A_MUTE_1W, A_MUTE_ALWAYS, A_UNMUTE, A_DIS_MENU, A_DIS_OFF, A_DIS_24H, A_DIS_7D, A_DIS_90D, A_THEME_MENU, A_MORE, A_BLOCK, A_UNBLOCK, A_CLEAR, A_DELETE, A_EXPORT, A_ARCHIVE, A_DO_BLOCK, A_DO_CLEAR, A_DO_DELETE, A_LIST_ARCHIVED, A_LIST_PROFILE, A_LOGOUT, A_DO_LOGOUT, A_QUIT, A_INSTALL_PROBES, A_TOGGLE_SCALE, A_ABOUT_CUSTOM, A_REACT_VIEW, A_TOGGLE_UI, A_STORY_TEXT, A_STORY_PHOTO, A_TOGGLE_RECEIPTS, A_UNFOLLOW, A_DO_UNFOLLOW, A_INSTALL_PLUGIN, A_DECLINE_PLUGIN, A_REBOOT, A_TOGGLE_NOTIF, A_UNINSTALL_PLUGIN, A_DO_UNINSTALL, A_COMM_GROUP, A_COMM_RENAME, A_COMM_DESC, A_COMM_LEAVE, A_DO_COMM_LEAVE, A_UPD_NOW, A_UPD_LATER, A_UPD_ERR, A_UPD_RETRY, A_THEME_BASE = 100, A_STORY_COL_BASE = 200, A_ABOUT_BASE = 300, A_REACT_BASE = 400 };
static void popup_begin(const char *title); static void popup_add(const char *label, int act, int red);
// ---------- installazione plugin notifiche (taiHEN) ----------
#define PLUGIN_LINE "ur0:tai/wa_notify.suprx"
#define NOTIF_FLAG "ur0:tai/wa_notif_off.txt"
static int plugin_cache = -1;
static int write_file(const char *path, const void *buf, size_t len) {
  SceUID f = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777); if (f < 0) return -1;
  size_t w = 0; while (w < len) { int r = sceIoWrite(f, (const char *)buf + w, len - w); if (r <= 0) break; w += r; }
  sceIoClose(f); return w == len ? 0 : -1;
}
// taiHEN legge ux0:tai/config.txt se esiste e solo altrimenti ur0:tai/config.txt: va modificato quello davvero usato
static const char *tai_config_path(void) {
  SceUID f = sceIoOpen("ux0:tai/config.txt", SCE_O_RDONLY, 0); if (f >= 0) { sceIoClose(f); return "ux0:tai/config.txt"; }
  f = sceIoOpen("ur0:tai/config.txt", SCE_O_RDONLY, 0); if (f >= 0) { sceIoClose(f); return "ur0:tai/config.txt"; }
  return NULL;
}
static int plugin_installed(void) {
  const char *cp = tai_config_path(); if (!cp) return 0;
  size_t l; unsigned char *c = read_file(cp, &l); if (!c) return 0;
  char *t = malloc(l + 1); memcpy(t, c, l); t[l] = 0; free(c);
  int ok = strstr(t, "wa_notify.suprx") != NULL; free(t);
  SceUID f = sceIoOpen("ur0:tai/wa_notify.suprx", SCE_O_RDONLY, 0); if (f < 0) ok = 0; else sceIoClose(f);
  return ok;
}
// aggiunge una riga (percorso di un plugin) sotto *main nel file di configurazione usato da taiHEN (con copia di sicurezza)
static int cfg_add_line(const char *line) {
  const char *cp = tai_config_path(); if (!cp) cp = "ur0:tai/config.txt";
  size_t cl = 0; unsigned char *c = read_file(cp, &cl);
  char *t = malloc(cl + 256); if (c) { memcpy(t, c, cl); free(c); } t[cl] = 0;
  if (strstr(t, line)) { free(t); return 1; }
  if (cl) { char bak[64]; snprintf(bak, sizeof bak, "%s.bak-wa", cp); write_file(bak, t, cl); }
  char *m = t; char *found = NULL;
  while ((m = strstr(m, "*main")) != NULL) { if (m == t || m[-1] == '\n') { found = m; break; } m += 5; }
  char *out = malloc(cl + 256); size_t ol = 0;
  if (found) {   // in fondo alla sezione *main (dopo i plugin gia' presenti, prima della sezione successiva)
    char *eol = strchr(found, '\n'); size_t pos = eol ? (size_t)(eol - t) + 1 : cl, ins = pos;
    while (pos < cl) {
      size_t e = pos; while (e < cl && t[e] != '\n') e++;
      if (t[pos] == '*') break;
      int nonblank = 0; for (size_t k = pos; k < e; k++) if (t[k] != ' ' && t[k] != '\t' && t[k] != '\r') { nonblank = 1; break; }
      if (nonblank) ins = e < cl ? e + 1 : e;
      pos = e < cl ? e + 1 : cl;
    }
    memcpy(out, t, ins); ol = ins; if (ol > 0 && out[ol - 1] != '\n') out[ol++] = '\n';
    ol += sprintf(out + ol, "%s\n", line); memcpy(out + ol, t + ins, cl - ins); ol += cl - ins;
  } else { memcpy(out, t, cl); ol = cl; ol += sprintf(out + ol, "%s*main\n%s\n", (cl && t[cl - 1] != '\n') ? "\n" : "", line); }
  int r = write_file(cp, out, ol); free(out); free(t); return r == 0;
}
static int install_plugin(void) {
#ifdef SAFE_BUILD
  toast_set(T("Serve la versione completa (WhatsApp notifiche)")); return 0;
#endif
  size_t pl; unsigned char *plug = read_file("app0:plugin/wa_notify.suprx", &pl);
  if (!plug) { toast_set(T("Plugin non incluso nel VPK")); return 0; }
  sceIoMkdir("ur0:tai", 0777);
  if (write_file("ur0:tai/wa_notify.suprx", plug, pl)) { free(plug); toast_set(T("Impossibile scrivere: installa il VPK in modalita' unsafe")); return 0; }
  free(plug);
  if (!cfg_add_line(PLUGIN_LINE)) { toast_set("config.txt non scrivibile: modalita' unsafe necessaria"); return 0; }
  toast_set(T("Plugin installato")); return 1;
}
// quattro plugin di prova a livelli: dopo il riavvio i loro file di registro dicono fin dove arriva il caricamento nella shell
static int install_probes(void) {
#ifdef SAFE_BUILD
  toast_set(T("Serve la versione completa")); return 0;
#endif
  sceIoMkdir("ur0:tai", 0777); int ok = 0;
  for (int n = 0; n < 4; n++) {
    char src[64], dst[64], line[64], lg[64]; snprintf(src, sizeof src, "app0:plugin/wa_probe%d.suprx", n); snprintf(dst, sizeof dst, "ur0:tai/wa_probe%d.suprx", n); snprintf(line, sizeof line, "ur0:tai/wa_probe%d.suprx", n);
    snprintf(lg, sizeof lg, "ux0:data/whatsapp/probe_s%d.log", n); sceIoRemove(lg);
    size_t pl; unsigned char *pb = read_file(src, &pl); if (!pb) continue;
    int w = write_file(dst, pb, pl); free(pb); if (w) continue;
    if (cfg_add_line(line)) ok++;
  }
  crumb("plugin di prova installati: %d su 4", ok);
  if (ok == 4) { toast_set("Plugin di prova installati: riavvia la Vita"); return 1; }
  toast_set("Plugin di prova non installati (serve unsafe homebrew)"); return 0;
}
static int notif_enabled(void) { SceUID f = sceIoOpen(NOTIF_FLAG, SCE_O_RDONLY, 0); if (f >= 0) { sceIoClose(f); return 0; } return 1; }
static int uninstall_one(const char *cp) {
  size_t cl; unsigned char *c = read_file(cp, &cl); if (!c) return 0;
  char *out = malloc(cl + 1); size_t ol = 0; const char *p = (const char *)c, *end = p + cl; int removed = 0;
  while (p < end) { const char *eol = memchr(p, '\n', end - p); size_t ln = eol ? (size_t)(eol - p) + 1 : (size_t)(end - p); char tmp[300]; size_t cp2 = ln < 299 ? ln : 299; memcpy(tmp, p, cp2); tmp[cp2] = 0; if (!strstr(tmp, "wa_notify.suprx") && !strstr(tmp, "wa_probe")) { memcpy(out + ol, p, ln); ol += ln; } else removed = 1; p += ln; }
  if (removed) { char bak[64]; snprintf(bak, sizeof bak, "%s.bak-wa", cp); write_file(bak, c, cl); write_file(cp, out, ol); }
  free(c); free(out); return removed;
}
static int uninstall_plugin(void) {
  int a = uninstall_one("ux0:tai/config.txt"), b = uninstall_one("ur0:tai/config.txt");
  sceIoRemove("ur0:tai/wa_notify.suprx"); for (int n = 0; n < 4; n++) { char d[48]; snprintf(d, sizeof d, "ur0:tai/wa_probe%d.suprx", n); sceIoRemove(d); }
  if (!a && !b) { toast_set(T("Riga del plugin non trovata nella configurazione")); return 0; }
  toast_set(T("Plugin rimosso")); return 1;
}
static void ask_reboot(void) { popup_begin(T("Riavviare la Vita ora?")); popup_add(T("Riavvia ora"), A_REBOOT, 0); popup_add(T("Piu' tardi"), A_CLOSE, 0); }
// il plugin installato e' diverso da quello incluso nel VPK? (aggiornamento automatico)
static int plugin_differs(void) {
  size_t a, b; unsigned char *x = read_file("app0:plugin/wa_notify.suprx", &a); if (!x) return 0;
  unsigned char *y = read_file("ur0:tai/wa_notify.suprx", &b);
  int diff = !y || a != b || memcmp(x, y, a) != 0; free(x); free(y); return diff;
}
static int plugin_upd_checked;
static int plugin_checked;

// ---------- popup (menu a tre puntini e conferme) ----------
typedef struct { char label[60]; int act; int red; } PItem;
static PItem pit[16]; static int npit; static char ptitle[80];
static void popup_begin(const char *title) { npit = 0; snprintf(ptitle, sizeof ptitle, "%s", title); popup_open = 1; }
static void popup_add(const char *label, int act, int red) { if (npit < 16) { snprintf(pit[npit].label, 60, "%s", label); pit[npit].act = act; pit[npit].red = red; npit++; } }
static void draw_popup(void) {
  if (!popup_open) return;
  float uk_save = UK; { float need = (ptitle[0] ? 52 : 8) + npit * 50 + 8 + 12; float k = 520.0f / need; if (k < UK) UK = k; if (UK < 0.8f) UK = 0.8f; }   // il popup si rimpicciolisce se ha molte voci
  btn_reset(); vita2d_draw_rectangle(0, 0, SW, SH, RGBA8(0, 0, 0, 150)); btn(0, 0, SW, SH, B_POPUP_CLOSE, 0);
  int mw = 440, mx = (SW - mw) / 2, th = ptitle[0] ? 52 : 8, mh = th + npit * 50 + 8; if (mh > SH - 20) mh = SH - 20; int my = (SH - mh) / 2;
  rrect(mx, my, mw, mh, 14, C_BAR); btn(mx, my, mw, mh, B_NONE, 0);
  if (ptitle[0]) textb(mx + 24, my + 14, C_TEXT, 20, ptitle);
  for (int k = 0; k < npit; k++) { int iy = my + th + k * 50; if (k || ptitle[0]) vita2d_draw_rectangle(mx + 16, iy, mw - 32, 1, C_DIV); text(mx + 28, iy + 13, pit[k].red ? C_RED : C_TEXT, 19, pit[k].label); btn(mx, iy, mw, 50, B_POPUP_ITEM, k); }
  UK = uk_save;
}
static void plugin_prompt_once(void) {
  if (plugin_checked) return; plugin_checked = 1;
#ifdef SAFE_BUILD
  return;
#endif
  SceUID f = sceIoOpen("ux0:data/whatsapp/plugin_declined.txt", SCE_O_RDONLY, 0); if (f >= 0) { sceIoClose(f); return; }
  if (plugin_installed()) return;
  popup_begin(T("Ricevere le notifiche?")); popup_add(T("Installa plugin (poi riavvia)"), A_INSTALL_PLUGIN, 0); popup_add(T("Non ora"), A_DECLINE_PLUGIN, 0);
}
static void popup_open_about(void) {
  static const char *ab[8] = { "Disponibile", "Occupato", "A scuola", "Al cinema", "Al lavoro", "In palestra", "In riunione", "Non posso parlare, solo WhatsApp" };
  popup_begin(T("Info")); for (int i = 0; i < 8; i++) popup_add(T(ab[i]), A_ABOUT_BASE + i, 0); popup_add(T("Scrivi tu..."), A_ABOUT_CUSTOM, 0);
}
static void popup_chat_menu(void) {
  Chat *c = &chats[cur_chat]; popup_begin("");
  if (c->channel) { popup_add(T("Smetti di seguire"), A_UNFOLLOW, 1); return; }
  popup_add(T("Nuovo gruppo"), A_NEW_GROUP, 0); popup_add(c->group ? T("Info gruppo") : T("Visualizza contatto"), A_VIEW_CONTACT, 0); popup_add(T("Cerca"), A_SEARCH, 0);
  popup_add(T("Media, link e documenti"), A_MEDIA, 0); popup_add(c->muted ? T("Riattiva notifiche") : T("Silenzia notifiche"), c->muted ? A_UNMUTE : A_MUTE_MENU, 0);
  popup_add(T("Messaggi effimeri"), A_DIS_MENU, 0); popup_add(T("Tema della chat"), A_THEME_MENU, 0); popup_add(T("Altro"), A_MORE, 0);
}
static void popup_confirm(const char *title, const char *yes, int act) { popup_begin(title); popup_add(yes, act, 1); popup_add(T("Annulla"), A_CLOSE, 0); }
static void popup_act(int act) {
  crumb("azione popup %d", act);
  Chat *c = (cur_chat >= 0 && cur_chat < nchats) ? &chats[cur_chat] : NULL;
  char more[160];
  popup_open = 0;
  switch (act) {
    case A_NEW_GROUP: build_contacts(); ngsel = 0; gsubject[0] = 0; misc_scroll = 0; screen = S_GROUP; break;
    case A_VIEW_CONTACT: if (c) { snprintf(contact_jid, sizeof contact_jid, "%s", c->jid); contact_from = S_CHAT; screen = S_CONTACT; misc_scroll = 0; qres ? (json_free(qres), qres = NULL) : (void)0; query_jid("contact", "/contact/", c->jid, ""); } break;
    case A_SEARCH: if (c) { snprintf(search_scope, sizeof search_scope, "%s", c->jid); search_q[0] = 0; search_from = S_CHAT; screen = S_SEARCH; misc_scroll = 0; qtag[0] = 0; ime_next = IME_SEARCH; } break;
    case A_MEDIA: if (c) { snprintf(media_jid, sizeof media_jid, "%s", c->jid); media_tab = 0; misc_scroll = 0; screen = S_MEDIA; media_query(); } break;
    case A_MUTE_MENU: popup_begin(T("Silenzia notifiche")); popup_add(T("8 ore"), A_MUTE_8H, 0); popup_add(T("1 settimana"), A_MUTE_1W, 0); popup_add(T("Sempre"), A_MUTE_ALWAYS, 0); break;
    case A_MUTE_8H: case A_MUTE_1W: case A_MUTE_ALWAYS: case A_UNMUTE:
      if (c) { long long ms = act == A_MUTE_8H ? 28800000LL : act == A_MUTE_1W ? 604800000LL : act == A_MUTE_ALWAYS ? 3153600000000LL : 0; snprintf(more, sizeof more, ",\"action\":\"mute\",\"until\":%lld", ms); fire_jid("/chatmod", c->jid, more, act == A_UNMUTE ? T("Notifiche riattivate") : T("Chat silenziata")); c->muted = act != A_UNMUTE; } break;
    case A_DIS_MENU: popup_begin(T("Messaggi effimeri")); popup_add(T("Disattivati"), A_DIS_OFF, 0); popup_add(T("24 ore"), A_DIS_24H, 0); popup_add(T("7 giorni"), A_DIS_7D, 0); popup_add(T("90 giorni"), A_DIS_90D, 0); break;
    case A_DIS_OFF: case A_DIS_24H: case A_DIS_7D: case A_DIS_90D:
      if (c) { int sec = act == A_DIS_24H ? 86400 : act == A_DIS_7D ? 604800 : act == A_DIS_90D ? 7776000 : 0; snprintf(more, sizeof more, ",\"seconds\":%d", sec); fire_jid("/disappear", c->jid, more, T("Messaggi effimeri aggiornati")); } break;
    case A_THEME_MENU: popup_begin(T("Tema della chat")); for (int i = 0; i < 6; i++) popup_add(T(THEME_NAMES[i]), A_THEME_BASE + i, 0); break;
    case A_MORE: if (c) { popup_begin(""); if (!c->group) popup_add(c->blocked ? T("Sblocca") : T("Blocca"), c->blocked ? A_UNBLOCK : A_BLOCK, 1); popup_add(T("Svuota chat"), A_CLEAR, 1); popup_add(T("Elimina chat"), A_DELETE, 1); popup_add(T("Esporta chat"), A_EXPORT, 0); popup_add(c->archived ? T("Rimuovi dall'archivio") : T("Archivia chat"), A_ARCHIVE, 0); } break;
    case A_BLOCK: popup_confirm(T("Bloccare questo contatto?"), T("Blocca"), A_DO_BLOCK); break;
    case A_CLEAR: popup_confirm(T("Svuotare la chat?"), T("Svuota chat"), A_DO_CLEAR); break;
    case A_DELETE: popup_confirm(T("Eliminare la chat?"), T("Elimina chat"), A_DO_DELETE); break;
    case A_DO_BLOCK: if (c) { fire_jid("/block", c->jid, ",\"block\":true", T("Contatto bloccato")); c->blocked = 1; } break;
    case A_UNBLOCK: if (c) { fire_jid("/block", c->jid, ",\"block\":false", T("Contatto sbloccato")); c->blocked = 0; } break;
    case A_DO_CLEAR: if (c) { fire_jid("/chatmod", c->jid, ",\"action\":\"clear\"", T("Chat svuotata")); free_msgs(); c->last[0] = 0; } break;
    case A_DO_DELETE: if (c) { fire_jid("/chatmod", c->jid, ",\"action\":\"delete\"", T("Chat eliminata")); close_chat(); } break;
    case A_ARCHIVE: if (c) { fire_jid("/chatmod", c->jid, c->archived ? ",\"action\":\"unarchive\"" : ",\"action\":\"archive\"", c->archived ? T("Rimossa dall'archivio") : T("Chat archiviata")); c->archived = !c->archived; close_chat(); } break;
    case A_EXPORT: if (c) { Job *j = job_new(J_EXPORT, c->jid); if (j) { snprintf(j->text, sizeof j->text, "%s", c->name); job_commit(); toast_set(T("Esportazione...")); } } break;
    case A_LIST_ARCHIVED: show_archived = !show_archived; list_scroll = 0; break;
    case A_LIST_PROFILE: screen = S_PROFILE; break;
    case A_QUIT: quit_req = 1; break;
    case A_INSTALL_PROBES: if (install_probes()) ask_reboot(); break;
    case A_TOGGLE_SCALE: { ui_k = ui_k > 1.8f ? 1.0f : ui_k > 1.2f ? 2.0f : 1.5f; ui_gen++; SceUID sf = sceIoOpen("ux0:data/whatsapp/ui_scale.txt", SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777); if (sf >= 0) { char d = ui_k < 1.2f ? '0' : ui_k < 1.8f ? '1' : '2'; sceIoWrite(sf, &d, 1); sceIoClose(sf); } toast_set(ui_k < 1.2f ? T("Dimensione: normale") : ui_k < 1.8f ? T("Dimensione: grande") : T("Dimensione: molto grande")); break; }
    case A_ABOUT_CUSTOM: ime_open(IME_ABOUT, T("Info"), prof.about); break;
    case A_STORY_TEXT: ime_open(IME_STORYTEXT, T("Testo della storia"), ""); break;
    case A_STORY_PHOTO: pick_mode = 2; screen = S_PICKER; pick_load(""); break;
    case A_TOGGLE_RECEIPTS: { receipts_on = !receipts_on; if (receipts_on) sceIoRemove("ux0:data/whatsapp/receipts_off.txt"); else write_file("ux0:data/whatsapp/receipts_off.txt", "1", 1); toast_set(receipts_on ? T("Conferma di visualizzazione: attiva") : T("Conferma di visualizzazione: disattivata")); break; }
    case A_UPD_NOW: upd_install(); break;
    case A_UPD_LATER: break;
    case A_UPD_ERR: upd_dismiss(); break;
    case A_UPD_RETRY: upd_retry(); break;
    case A_COMM_GROUP: ime_open(IME_COMM_GNAME, T("Nome del nuovo gruppo"), ""); break;
    case A_COMM_RENAME: ime_open(IME_COMM_RENAME, T("Nuovo nome della community"), ""); break;
    case A_COMM_DESC: ime_open(IME_COMM_DESC2, T("Descrizione della community"), ""); break;
    case A_COMM_LEAVE: popup_confirm(T("Uscire dalla community?"), T("Esci dalla community"), A_DO_COMM_LEAVE); break;
    case A_DO_COMM_LEAVE: { char b[260]; snprintf(b, sizeof b, "{\"id\":\"%s\"}", comm_sel); fire("/community/leave", b, T("Sei uscito dalla community")); comm_refresh_at = frame_no + 240; break; }
    case A_UNFOLLOW: popup_confirm(T("Smettere di seguire questo canale?"), T("Smetti di seguire"), A_DO_UNFOLLOW); break;
    case A_DO_UNFOLLOW: if (c) { fire_jid("/channel/unfollow", c->jid, "", T("Non segui piu' il canale")); close_chat(); } break;
    case A_TOGGLE_UI: { apply_ui_theme(!ui_dark); vita2d_set_clear_color(C_BG); SceUID tf = sceIoOpen("ux0:data/whatsapp/theme_ui.txt", SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777); if (tf >= 0) { char d = ui_dark ? '1' : '0'; sceIoWrite(tf, &d, 1); sceIoClose(tf); } toast_set(ui_dark ? T("Tema scuro") : T("Tema chiaro")); break; }
    case A_INSTALL_PLUGIN: if (install_plugin()) { plugin_cache = -1; ask_reboot(); } break;
    case A_REBOOT: scePowerRequestColdReset(); break;
    case A_TOGGLE_NOTIF: if (notif_enabled()) { write_file(NOTIF_FLAG, "1", 1); toast_set(T("Notifiche disattivate")); } else { sceIoRemove(NOTIF_FLAG); toast_set(T("Notifiche attivate")); } break;
    case A_UNINSTALL_PLUGIN: popup_confirm(T("Rimuovere il plugin notifiche?"), T("Rimuovi"), A_DO_UNINSTALL); break;
    case A_DO_UNINSTALL: if (uninstall_plugin()) { plugin_cache = -1; ask_reboot(); } break;
    case A_DECLINE_PLUGIN: write_file("ux0:data/whatsapp/plugin_declined.txt", "1", 1); break;
    case A_LOGOUT: popup_confirm(T("Disconnettere WhatsApp da questa Vita?"), T("Disconnetti"), A_DO_LOGOUT); break;
    case A_DO_LOGOUT: fire("/logout", "{}", T("Disconnesso")); nchats = 0; free_msgs(); screen = S_LOGIN; break;
    default:
      if (act >= A_REACT_BASE && act < A_REACT_BASE + 6) {
        static const unsigned em[6] = { 0x1F44D, 0x2764, 0x1F602, 0x1F62E, 0x1F622, 0x1F64F }; char t[16] = ""; append_cp(t, sizeof t, em[act - A_REACT_BASE]);
        Job *j = job_new(J_REACT, view_jid); if (j) { snprintf(j->id, sizeof j->id, "%s", view_key_id); snprintf(j->text, sizeof j->text, "%s", t); job_commit(); toast_set(T("Reazione inviata")); } break;
      }
      if (act >= A_ABOUT_BASE && act < A_ABOUT_BASE + 8) {
        static const char *ab[8] = { "Disponibile", "Occupato", "A scuola", "Al cinema", "Al lavoro", "In palestra", "In riunione", "Non posso parlare, solo WhatsApp" };
        const char *t = T(ab[act - A_ABOUT_BASE]); Job *j = job_new(J_PROFILE, NULL); if (j) { snprintf(j->kind, sizeof j->kind, "about"); snprintf(j->text, sizeof j->text, "%s", t); job_commit(); snprintf(prof.about, sizeof prof.about, "%s", t); } break;
      }
      if (act >= A_STORY_COL_BASE && act < A_STORY_COL_BASE + 6) {
        static const char *cols[6] = { "#128C7E", "#1F6FEB", "#7E3FF2", "#D93A4F", "#E8741D", "#4B5663" };
        char t[2100], body[2300]; jesc(story_text, t, sizeof t); snprintf(body, sizeof body, "{\"text\":\"%s\",\"bg\":\"%s\",\"font\":1}", t, cols[act - A_STORY_COL_BASE]);
        fire("/status/post", body, T("Storia pubblicata")); refresh_my_at = frame_no + 240; break;
      }
      if (act >= A_THEME_BASE && act < A_THEME_BASE + 6) { theme_idx = act - A_THEME_BASE; SceUID f = sceIoOpen("ux0:data/whatsapp/theme.txt", SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777); if (f >= 0) { char d = '0' + theme_idx; sceIoWrite(f, &d, 1); sceIoClose(f); } }
      break;
  }
}


// ---------- fotocamera (frontale e posteriore): la foto scattata passa dall'anteprima e dall'invio di sempre ----------
// Stessa tecnica del lettore QR di VitaShell: la fotocamera scrive i fotogrammi direttamente in una texture della GPU.
#define CAM_W 640
#define CAM_H 480
static vita2d_texture *cam_tex; static int cam_dev, cam_on, cam_fail; static SceCameraInfo cam_info; static SceCameraRead cam_read;
static int cam_start(int dev) {
  if (!cam_tex) {
    SceKernelMemBlockType orig = vita2d_texture_get_alloc_memblock_type();
    vita2d_texture_set_alloc_memblock_type(SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW);
    cam_tex = vita2d_create_empty_texture(CAM_W, CAM_H);
    vita2d_texture_set_alloc_memblock_type(orig);
    if (!cam_tex) { crumb("fotocamera: texture non creata"); return -1; }
  }
  memset(&cam_info, 0, sizeof cam_info); cam_info.size = sizeof cam_info; cam_info.format = SCE_CAMERA_FORMAT_ABGR; cam_info.resolution = SCE_CAMERA_RESOLUTION_640_480;
  cam_info.framerate = SCE_CAMERA_FRAMERATE_30_FPS; cam_info.pitch = vita2d_texture_get_stride(cam_tex) - (CAM_W << 2); cam_info.sizeIBase = (CAM_W * CAM_H) << 2; cam_info.pIBase = vita2d_texture_get_datap(cam_tex);
  memset(&cam_read, 0, sizeof cam_read); cam_read.size = sizeof cam_read;
  int r = sceCameraOpen(dev, &cam_info); crumb("fotocamera %d: apertura -> 0x%x", dev, r); if (r < 0) return r;
  r = sceCameraStart(dev); crumb("fotocamera %d: avvio -> 0x%x", dev, r); if (r < 0) { sceCameraClose(dev); return r; }
  cam_dev = dev; cam_on = 1; cam_fail = 0; return 0;
}
static void cam_stop(void) { if (cam_on) { sceCameraStop(cam_dev); sceCameraClose(cam_dev); cam_on = 0; } }
static int cam_save_jpeg(const char *path) {
  size_t bytes = (size_t)vita2d_texture_get_stride(cam_tex) * CAM_H; unsigned char *copy = malloc(bytes); if (!copy) return -1;
  memcpy(copy, vita2d_texture_get_datap(cam_tex), bytes);   // si copia in RAM: leggere pixel per pixel dalla memoria video e' lentissimo
  int stride = vita2d_texture_get_stride(cam_tex) / 4;
  struct jpeg_compress_struct c; struct jpeg_error_mgr jerr; c.err = jpeg_std_error(&jerr); jpeg_create_compress(&c);
  unsigned char *out = NULL; unsigned long outlen = 0; jpeg_mem_dest(&c, &out, &outlen);
  c.image_width = CAM_W; c.image_height = CAM_H; c.input_components = 3; c.in_color_space = JCS_RGB; jpeg_set_defaults(&c); jpeg_set_quality(&c, 90, TRUE); jpeg_start_compress(&c, TRUE);
  unsigned char *row = malloc(CAM_W * 3);
  while (c.next_scanline < c.image_height) {
    const uint32_t *p = (const uint32_t *)copy + (size_t)c.next_scanline * stride;
    for (int x = 0; x < CAM_W; x++) { row[x * 3] = p[x] & 255; row[x * 3 + 1] = (p[x] >> 8) & 255; row[x * 3 + 2] = (p[x] >> 16) & 255; }
    JSAMPROW rp = row; jpeg_write_scanlines(&c, &rp, 1);
  }
  jpeg_finish_compress(&c); int r = out ? write_file(path, out, outlen) : -1; crumb("fotocamera: foto salvata (%lu byte) -> %d", outlen, r);
  jpeg_destroy_compress(&c); free(out); free(row); free(copy); return r;
}
static void cam_shoot(void) {
  if (!cam_on) return;
  sceIoMkdir("ux0:data/whatsapp", 0777);
  if (cam_save_jpeg("ux0:data/whatsapp/camera.jpg") != 0) { toast_set(T("Impossibile salvare la foto")); return; }
  cam_stop();
  snprintf(sel_path, sizeof sel_path, "ux0:data/whatsapp/camera.jpg"); snprintf(sel_kind, sizeof sel_kind, "image"); caption[0] = 0;
  if (prev_tex) { tex_retire(prev_tex); prev_tex = NULL; }
  { size_t l; unsigned char *b = read_file(sel_path, &l); if (b) { prev_tex = decode_jpeg(b, l, 960, 0); free(b); } }
  screen = S_PREVIEW;
}
static void draw_camera(void) {
  vita2d_draw_rectangle(0, 0, SW, SH, RGBA8(0, 0, 0, 255));
  if (cam_on && sceCameraIsActive(cam_dev) > 0) {
    int r = sceCameraRead(cam_dev, &cam_read);
    if (r < 0) { if (++cam_fail > 90) { int d = cam_dev; cam_stop(); crumb("fotocamera: nessun fotogramma, riapro"); if (cam_start(d) < 0) { toast_set(T("Fotocamera non disponibile")); screen = S_PICKER; return; } } } else cam_fail = 0;
  }
  if (cam_tex) { float s = fminf(SW / (float)CAM_W, (SH - 150) / (float)CAM_H), w = CAM_W * s, h = CAM_H * s; vita2d_draw_texture_scale(cam_tex, (SW - w) / 2, 10 + (SH - 150 - h) / 2, s, s); }
  if (!cam_on) text(SW / 2 - 90, SH / 2 - 20, RGBA8(210, 210, 210, 255), 20, T("Fotocamera non attiva"));
  vita2d_draw_rectangle(0, SH - 130, SW, 130, RGBA8(0, 0, 0, 255));
  vita2d_draw_fill_circle(SW / 2, SH - 65, 44, RGBA8(255, 255, 255, 255)); vita2d_draw_fill_circle(SW / 2, SH - 65, 36, RGBA8(0x25, 0xD3, 0x66, 255)); btn(SW / 2 - 50, SH - 115, 100, 100, B_CAM_SHOT, 0);
  const char *fl = cam_dev == 0 ? T("Frontale") : T("Posteriore");
  rrect(SW - 230, SH - 90, 200, 50, 25, RGBA8(50, 50, 50, 255)); text(SW - 230 + (200 - tw(18, fl)) / 2, SH - 77, C_WHITE, 18, fl); btn(SW - 230, SH - 90, 200, 50, B_CAM_FLIP, 0);
  vita2d_draw_rectangle(0, 0, SW, 0, 0); back_arrow(); btn(0, 0, 70, 60, B_BACK, 0);
  text(SW / 2 - 150, SH - 120 - 1, RGBA8(170, 170, 170, 255), 13, T("X = scatta   Triangolo = cambia fotocamera   Cerchio = esci"));
}


// ---------- aggiornamento dell'app: schermata di avanzamento ----------
// Disegnata dall'interfaccia (non dalla finestra di sistema): compare subito, per ogni download, senza schermo nero.
static void draw_update_overlay(void) {
  if (!upd_busy()) return;
  UpdInfo u; upd_snapshot(&u);
  vita2d_draw_rectangle(0, 0, SW, SH, C_BG); btn(0, 0, SW, SH, B_UPD_BLOCK, 0);
  textb((SW - tw(30, T("Aggiornamento"))) / 2, 90, C_GREEN2, 30, T("Aggiornamento"));
  char vt[120]; snprintf(vt, sizeof vt, "%s  ->  %s", u.current, u.tag); text((SW - tw(20, vt)) / 2, 142, C_TEXT, 20, vt);
  const char *stage = u.state == UPD_DOWNLOADING ? T("Scarico il nuovo pacchetto") : u.state == UPD_VERIFYING ? T("Verifico il pacchetto") : u.state == UPD_EXTRACTING ? T("Preparo l'installazione") : T("Installo");
  text((SW - tw(22, stage)) / 2, 200, C_TEXT, 22, stage);
  float frac = u.total > 0 ? (float)u.done / (float)u.total : 0; if (frac > 1) frac = 1;
  float bw = 640, bx = (SW - bw) / 2, by = 260;
  rrect(bx, by, bw, 26, 13, C_DIV);
  if (u.total > 0) { float fw = bw * frac; if (fw < 26) fw = 26; rrect(bx, by, fw, 26, 13, C_GREEN2); }
  else { float ph = (float)(frame_no % 90) / 90.0f; float fw = 160, fx = bx + (bw - fw) * (ph < 0.5f ? ph * 2 : 2 - ph * 2); rrect(fx, by, fw, 26, 13, C_GREEN2); }   // attesa senza percentuale: barra che scorre
  char pc[96];
  if (u.state == UPD_DOWNLOADING && u.total > 0) snprintf(pc, sizeof pc, "%d%%   -   %.1f / %.1f MB", (int)(frac * 100), u.done / 1048576.0, u.total / 1048576.0);
  else if (u.state == UPD_DOWNLOADING) snprintf(pc, sizeof pc, T("%.1f MB scaricati"), u.done / 1048576.0);
  else if (u.state == UPD_EXTRACTING && u.total > 0) snprintf(pc, sizeof pc, "%d%%   -   file %d di %d", (int)(frac * 100), (int)u.done, (int)u.total);
  else snprintf(pc, sizeof pc, "%s", u.msg);
  text((SW - tw(18, pc)) / 2, by + 44, C_SUB, 18, pc);
  if (u.state == UPD_INSTALLING) text((SW - tw(16, T("L'app si chiude e si riapre da sola: non spegnere la Vita"))) / 2, 380, C_SUB, 16, T("L'app si chiude e si riapre da sola: non spegnere la Vita"));
  if (u.state == UPD_DOWNLOADING) text((SW - tw(15, T("L'aggiornamento e' obbligatorio: attendi il termine"))) / 2, 440, C_SUB, 15, T("L'aggiornamento e' obbligatorio: attendi il termine"));
}


// ---------- scelta della lingua ----------
static vita2d_font *lang_fonts[L_COUNT], *lang_roboto;   // il nome di ogni lingua si scrive col suo font, qualunque sia la lingua dell'interfaccia
static vita2d_font *lang_font(int l) {
  const char *p = i18n_font_path(l);
  if (!p) { if (!lang_roboto) lang_roboto = vita2d_load_font_file(ROBOTO_PATH); return lang_roboto; }
  if (!lang_fonts[l]) lang_fonts[l] = vita2d_load_font_file(p);
  return lang_fonts[l] ? lang_fonts[l] : lang_font(L_EN);
}
static void lang_select(int i) {   // 0 = automatica, 1.. = lingue
  i18n_set(i == 0 ? -1 : i - 1); ui_gen++; memset(wcache, 0, sizeof wcache); screen = S_PROFILE; misc_scroll = 0;
  crumb("lingua scelta: %s (%d)", i == 0 ? "automatica" : "manuale", i18n_lang());
}
static void draw_lang(void) {
  vita2d_draw_rectangle(0, 0, SW, 60, C_BAR); back_arrow(); btn(0, 0, 70, 60, B_BACK, 0); textb(70, 14, C_TEXT, 26, T("Scegli la lingua"));
  const float top = 60, rowh = 60; const int n = L_COUNT + 1;
  misc_clamp(n * rowh, SH - top);
  vita2d_enable_clipping(); vita2d_set_clip_rectangle(0, top, SW, SH); clip_y0 = top; clip_y1 = SH;
  int sys = i18n_system_lang();
  for (int i = 0; i < n; i++) {
    float y = top + i * rowh - misc_scroll; if (y + rowh < top || y > SH) continue;
    int sel = i == 0 ? i18n_is_auto() : (!i18n_is_auto() && i18n_lang() == i - 1);
    if (sel) rrect(10, y + 4, SW - 20, rowh - 8, 12, C_BAR);
    if (i == 0) {
      char nm[160]; snprintf(nm, sizeof nm, T("Automatica (%s)"), i18n_native_name(sys));
      { int old_cjk = cjk_ok; cjk_ok = i18n_font_path(sys) != NULL; font_override = lang_font(sys); text(30, (int)y + 8, C_TEXT, 20, nm); font_override = NULL; cjk_ok = old_cjk; }
      text(30, (int)y + 34, C_SUB, 14, T("Segue la lingua di sistema della Vita"));
    } else {
      { int old_cjk = cjk_ok; cjk_ok = i18n_font_path(i - 1) != NULL; font_override = lang_font(i - 1); text(30, (int)y + 16, C_TEXT, 22, i18n_native_name(i - 1)); font_override = NULL; cjk_ok = old_cjk; }
    }
    if (sel) vita2d_draw_fill_circle(SW - 50, y + rowh / 2, 12, C_GREEN2);
    vita2d_draw_rectangle(30, y + rowh - 1, SW - 60, 1, C_DIV); btn(0, (int)y, SW, rowh, B_LANG_ROW, i);
  }
  vita2d_disable_clipping(); clip_y0 = 0; clip_y1 = SH;
}


// ---------- storie: avanti e indietro (tocco, L1/R1, croce) ----------
static void st_step(int dir) {
  jv *a = qarr("updates"); jv *its = st_mine ? qres2_arr("mystatus") : (a && st_idx < a->count ? json_get(a->items[st_idx], "items") : NULL);
  if (dir < 0) {
    if (st_item > 0) st_item--;
    else if (!st_mine && a && st_idx > 0) { st_idx--; jv *p = json_get(a->items[st_idx], "items"); st_item = p && p->count ? p->count - 1 : 0; }   // indietro: ultima storia del contatto precedente
  } else {
    if (its && st_item + 1 < its->count) st_item++;
    else if (!st_mine && a && st_idx + 1 < a->count) { st_idx++; st_item = 0; }   // avanti: prima storia del contatto successivo
    else screen = S_UPDATES;
  }
}
// ---------- trasferimenti (video, audio, file) ----------
static void start_prep(const char *jid, const char *id, const char *kind, const char *fname) {
  if (xfer.active) return;
  Job *j = job_new(J_PREP, jid); if (!j) { toast_set(T("Coda piena, riprova")); return; }
  snprintf(j->id, sizeof j->id, "%s", id); snprintf(j->kind, sizeof j->kind, "%s", kind); snprintf(j->text, sizeof j->text, "%s", fname ? fname : ""); job_commit();
  pthread_mutex_lock(&mu); xfer.active = 1; xfer.cancel = 0; xfer.done = xfer.total = 0; xfer.title[0] = 0; xfer.phase[0] = 0; pthread_mutex_unlock(&mu);   // l'overlay compare subito
}
static int vp_touched;
static void vp_stop(void);
static void vp_stop(void) { if (vp.key[0]) { vp.gen++; vp.key[0] = 0; vp.state = 0; } vp.audio = 0; player_close(); }
static void vp_open_ready(void) {
  if (vp.key[0] && vp.state == 2 && !vp.opened) { vp.opened = 1; int pr = player_open(vp.path, !vp.audio); crumb("lettore: apertura %s -> 0x%x", vp.path, (unsigned)pr); if (pr != 0) vp.state = 3; }
}
static void vp_want(const char *jid, const char *id) {
  vp_touched = 1; if (vp.audio && vp.key[0]) vp_stop();  /* un video prende il posto di un audio in corso */ char k[260]; snprintf(k, sizeof k, "%s|%s", jid, id);
  if (strcmp(k, vp.key)) {
    player_close(); snprintf(vp.key, sizeof vp.key, "%s", k); vp.gen++; vp.state = 1; vp.pct = 0; vp.phase = 0; vp.opened = 0;
    char cf[300]; SceIoStat st; vp_cachefile(id, vp.audio, cf, sizeof cf);
    if (sceIoGetstat(cf, &st) >= 0 && st.st_size > 0) { snprintf(vp.path, sizeof vp.path, "%s", cf); vp.state = 2; }
    else { Job *j = job_new(J_VPREP, jid); if (j) { snprintf(j->id, sizeof j->id, "%s", id); snprintf(j->kind, sizeof j->kind, "%s", vp.audio ? "audio" : "video"); snprintf(j->extra, sizeof j->extra, "%d", vp.gen); job_commit(); } else vp.state = 3; }
  }
  vp_open_ready();
}
// audio in chat: un tocco avvia (dopo la conversione in MP3 sul bridge), un altro tocco ferma
static void aud_toggle(const char *jid, const char *id) {
  char k[260]; snprintf(k, sizeof k, "%s|%s", jid, id);
  if (vp.audio && !strcmp(k, vp.key)) { vp_stop(); return; }   // gia' in corso: ferma
  crumb("audio: tocco su %s", id); vp_stop(); vp.audio = 1; vp_want(jid, id); vp.audio = 1; vp_touched = 0;
}
static int aud_state(const char *jid, const char *id, int *pct, int *secs) {
  if (!vp.audio || !vp.key[0]) return 0; char k[260]; snprintf(k, sizeof k, "%s|%s", jid, id); if (strcmp(k, vp.key)) return 0;
  *pct = vp.pct; *secs = (int)(player_time_ms() / 1000);
  return vp.state == 3 ? 3 : vp.state == 2 ? 2 : 1;
}
// disegna il video (o lo stato della preparazione) nel riquadro; 1 se si vede il filmato
static int vp_draw(float x, float y, float w, float h) {
  vita2d_texture *t = vp.state == 2 ? player_frame() : NULL;
  { static unsigned long long t0; static char lk[260]; if (strcmp(lk, vp.key)) { snprintf(lk, sizeof lk, "%s", vp.key); t0 = 0; } if (vp.state == 2 && !t0) t0 = sceKernelGetProcessTimeWide(); if (vp.state == 2 && t0) { if (t && player_frames() == 1) { { unsigned a_ = 0, b_ = 0; player_size(&a_, &b_); crumb("lettore: primo fotogramma %ux%u", a_, b_); } t0 = 1; } else if (t0 > 1000 && !t && sceKernelGetProcessTimeWide() - t0 > 5000000ULL) { crumb("lettore: nessun fotogramma dopo 5 s"); t0 = 1; } } }
  if (t) { unsigned pw_ = 0, ph_ = 0; player_size(&pw_, &ph_); float tw0 = pw_ ? (float)pw_ : vita2d_texture_get_width(t), th0 = ph_ ? (float)ph_ : vita2d_texture_get_height(t), s = fminf(w / tw0, h / th0); vita2d_draw_texture_scale(t, x + (w - tw0 * s) / 2, y + (h - th0 * s) / 2, s, s); return 1; }
  char b[96]; if (vp.state == 3) snprintf(b, sizeof b, "%s", T("Video non disponibile")); else if (vp.phase == 1) snprintf(b, sizeof b, "%s %d%%", T("Converto per la Vita"), vp.pct); else if (vp.phase == 2) snprintf(b, sizeof b, "%s %d%%", T("Scarico sulla Vita"), vp.pct); else snprintf(b, sizeof b, "%s", T("Scarico da WhatsApp"));
  float bw = 360, bx = x + (w - bw) / 2, by = y + h / 2 + 30;
  vita2d_draw_rectangle(bx - 30, by - 50, bw + 60, 96, RGBA8(0, 0, 0, 170)); text((int)(x + (w - tw(18, b)) / 2), (int)by - 38, C_WHITE, 18, b);
  rrect(bx, by, bw, 12, 6, RGBA8(255, 255, 255, 70)); if (vp.state != 3 && vp.phase) { float f = vp.pct / 100.0f; if (f < 0.04f) f = 0.04f; rrect(bx, by, bw * f, 12, 6, C_GREEN2); }
  return 0;
}
static void draw_xfer_overlay(void) {
  if (vp.audio) vp_open_ready();
  { static unsigned long long ad_t; static int ad_done; if (vp.audio && vp.opened) { if (!ad_t) ad_t = sceKernelGetProcessTimeWide(); if (!ad_done && sceKernelGetProcessTimeWide() - ad_t > 3000000ULL) { char d[160]; player_audio_diag(d, sizeof d); crumb("lettore: %s", d); ad_done = 1; } } else { ad_t = 0; ad_done = 0; } }   // l'audio parte appena il file e' scaricato
  if (vp.audio && (screen != S_CHAT || (vp.state == 2 && player_ended()))) vp_stop();
  if (!vp_touched && !vp.audio) vp_stop(); vp_touched = 0;
  if (!xfer.active) return;
  char title[80], phase[96]; pthread_mutex_lock(&mu); snprintf(title, sizeof title, "%s", xfer.title); snprintf(phase, sizeof phase, "%s", xfer.phase); unsigned long long done = xfer.done, total = xfer.total; pthread_mutex_unlock(&mu);
  vita2d_draw_rectangle(0, 0, SW, SH, C_BG); btn(0, 0, SW, SH, B_UPD_BLOCK, 0);
  textb((SW - tw(30, title)) / 2, 120, C_GREEN2, 30, title);
  if (phase[0]) text((SW - tw(22, phase)) / 2, 190, C_TEXT, 22, phase);
  float frac = total > 0 ? (float)done / (float)total : 0; if (frac > 1) frac = 1; float bw = 640, bx = (SW - bw) / 2, by = 250;
  rrect(bx, by, bw, 26, 13, C_DIV);
  if (total > 0) { float fw = bw * frac; if (fw < 26) fw = 26; rrect(bx, by, fw, 26, 13, C_GREEN2); }
  else { float ph = (float)(frame_no % 90) / 90.0f; float fw = 160, fx = bx + (bw - fw) * (ph < 0.5f ? ph * 2 : 2 - ph * 2); rrect(fx, by, fw, 26, 13, C_GREEN2); }
  char pc[96] = ""; if (total > 0) { if (total > 100) snprintf(pc, sizeof pc, "%d%%   -   %.1f / %.1f MB", (int)(frac * 100), done / 1048576.0, total / 1048576.0); else snprintf(pc, sizeof pc, "%d%%", (int)(frac * 100)); }
  if (pc[0]) text((SW - tw(18, pc)) / 2, by + 44, C_SUB, 18, pc);
  rrect((SW - 260) / 2, 420, 260, 46, 23, C_BAR); text((SW - 260) / 2 + (260 - tw(18, T("Annulla (Cerchio)"))) / 2, 432, C_RED, 18, T("Annulla (Cerchio)")); btn((SW - 260) / 2, 420, 260, 46, B_XFER_CANCEL, 0);
}

// ---------- disegno schermate aggiuntive ----------
static void draw_extra_screens(void) {
  switch (screen) {
    case S_UPDATES: draw_updates(); break;
    case S_STATUSVIEW: draw_statusview(); break;
    case S_COMM: draw_comm(); break;
    case S_CALLS: draw_calls(); break;
    case S_CAMERA: draw_camera(); break;
    case S_LANG: draw_lang(); break;
    case S_SEARCH: draw_search(); break;
    case S_NEWCHAT: draw_newchat(); break;
    case S_GROUP: draw_group(); break;
    case S_CONTACT: draw_contact(); break;
    case S_MEDIA: draw_media(); break;
    default: break;
  }
  draw_viewer();
  draw_popup();
  draw_update_overlay();
  draw_xfer_overlay();
}

// ---------- gestione tocchi ----------
static int extra_back(void) {
  switch (screen) {
    case S_CAMERA: cam_stop(); screen = S_PICKER; return 1;
    case S_LANG: screen = S_PROFILE; misc_scroll = 0; return 1;
    case S_SEARCH: screen = search_from == S_CHAT && cur_chat >= 0 ? S_CHAT : S_LIST; misc_scroll = 0; return 1;
    case S_NEWCHAT: screen = S_LIST; return 1;
    case S_GROUP: screen = S_NEWCHAT; misc_scroll = 0; return 1;
    case S_CONTACT: screen = contact_from; return 1;
    case S_MEDIA: screen = S_CHAT; return 1;
    case S_STATUSVIEW: screen = S_UPDATES; return 1;
    case S_UPDATES: case S_COMM: case S_CALLS: goto_tab(0); return 1;
    default: return 0;
  }
}
static void handle_extra(int id, int arg) {
  switch (id) {
    case B_TAB: goto_tab(arg); break;
    case B_CALL_ROW: { jv *a = qarr("calls"); if (a && arg < a->count) open_by_jid(json_str(a->items[arg], "jid", "")); break; }
    case B_LOGOUT: popup_act(A_LOGOUT); break;
    case B_BRIDGE_ADDR: ime_open(IME_BRIDGE, T("Indirizzo bridge (es. http://192.168.1.59:8787)"), base_url); break;
    case B_PLUGIN: popup_act(plugin_installed() ? A_TOGGLE_NOTIF : A_INSTALL_PLUGIN); plugin_cache = -1; break;
    case B_PLUGIN_RM: popup_act(A_UNINSTALL_PLUGIN); break;
    case B_PLUGIN_DIAG: {   // diagnostica: configurazione, segno di vita, esito dei plugin di prova, ultime righe del registro
      popup_begin(T("Diagnostica plugin notifiche"));
      { const char *cp = tai_config_path(); char l[64]; snprintf(l, sizeof l, T("Config: %s"), cp ? cp : "NON TROVATA"); popup_add(l, A_CLOSE, 0); }
      int al = alive_state(); char l0[64];
      snprintf(l0, sizeof l0, T("Plugin: %s"), al == 1 ? T("attivo (il file di stato cambia)") : al < 0 ? T("verifica in corso...") : T("NON attivo (file di stato fermo)")); popup_add(l0, A_CLOSE, 0);
      for (int n = 0; n < 4; n++) {   // plugin di prova: se un livello non scrive nulla, il caricamento si ferma li'
        char f[64], l[64]; snprintf(f, sizeof f, "ux0:data/whatsapp/probe_s%d.log", n); size_t pl2; unsigned char *pb2 = read_file(f, &pl2);
        if (!pb2) snprintf(l, sizeof l, "Prova %d: assente (non partito)", n);
        else { char *e2 = (char *)pb2 + pl2; while (e2 > (char *)pb2 && (e2[-1] == '\n' || e2[-1] == '\r')) e2--; *e2 = 0; char *ls = (char *)pb2; for (char *q = ls; q < e2; q++) if (*q == '\n') ls = q + 1; snprintf(l, sizeof l, "P%d: %.54s", n, ls); free(pb2); }
        popup_add(l, A_CLOSE, 0);
      }
      size_t pl; unsigned char *pb = read_file("ur0:tai/wa_plugin.log", &pl);
      if (!pb) popup_add("plugin.log assente: plugin principale mai partito", A_CLOSE, 0);
      else { char *tail = (char *)pb + (pl > 400 ? pl - 400 : 0); char *lines[20]; int nl = 0; char *sv; for (char *tk = strtok_r(tail, "\n", &sv); tk && nl < 20; tk = strtok_r(NULL, "\n", &sv)) lines[nl++] = tk;
        for (int i = nl > 3 ? nl - 3 : 0; i < nl; i++) { char l[64]; snprintf(l, sizeof l, "%.58s", lines[i]); popup_add(l, A_CLOSE, 0); } free(pb); }
      popup_add(T("Chiudi"), A_CLOSE, 0); break; }
    case B_NOTIF_TEST: {
      write_file("ur0:tai/wa_notif_test.txt", "1", 1);
#ifndef SAFE_BUILD
      { static SceWChar16 w[0x410 / 2]; memset(w, 0, sizeof w); const char *t = "Prova dall'app"; for (int i = 0; t[i] && i < 200; i++) w[i] = (unsigned char)t[i];   // prova diretta dall'app (processo normale)
        int r0 = sceSysmoduleLoadModule(SCE_SYSMODULE_NOTIFICATION_UTIL); int r = sceNotificationUtilSendNotification(w); crumb("notifica diretta dall'app: carica 0x%x invio 0x%x", r0, r);
        char m[96]; snprintf(m, sizeof m, T("Prova: dall'app esito 0x%x (0 = riuscita)"), (unsigned)r); toast_set(m); }
#else
      toast_set(T("Prova inviata: se il plugin e' attivo vedrai la notifica entro pochi secondi"));
#endif
      break; }
    case B_CAM_OPEN: { int r = cam_start(1); if (r < 0) r = cam_start(0); if (r < 0) { char t[64]; snprintf(t, sizeof t, T("Fotocamera non disponibile (0x%x)"), (unsigned)r); toast_set(t); } else screen = S_CAMERA; break; }
    case B_CAM_SHOT: cam_shoot(); break;
    case B_CAM_FLIP: { int d = cam_dev ^ 1; cam_stop(); if (cam_start(d) < 0) cam_start(d ^ 1); break; }
    case B_UPD_CHECK: { UpdInfo u; upd_snapshot(&u); if (u.state == UPD_AVAILABLE) u.state = UPD_IDLE; upd_check(1); toast_set(T("Controllo aggiornamenti...")); break; }
    case B_UPD_CANCEL: upd_cancel(); break;
    case B_XFER_CANCEL: xfer.cancel = 1; break;
    case B_LANG: screen = S_LANG; misc_scroll = 0; break;
    case B_LANG_ROW: lang_select(arg); break;
    case B_UPD_BLOCK: break;
    case B_COMM_NEW: ime_open(IME_COMM_NAME, T("Nome della community"), ""); break;
    case B_COMM_HEAD: { jv *a = qarr("comm"); if (a && arg < a->count) { comm_sel_idx = arg; snprintf(comm_sel, sizeof comm_sel, "%s", json_str(a->items[arg], "id", "")); char tt[160]; snprintf(tt, sizeof tt, "%s", json_str(a->items[arg], "subject", T("Community")));
        popup_begin(tt); popup_add(T("Crea un gruppo nella community"), A_COMM_GROUP, 0); popup_add(T("Rinomina"), A_COMM_RENAME, 0); popup_add(T("Cambia descrizione"), A_COMM_DESC, 0); popup_add(T("Esci dalla community"), A_COMM_LEAVE, 1); popup_add(T("Chiudi"), A_CLOSE, 0); } break; }
    case B_VIEW_SAVE: if (view_is_video) { start_prep(view_jid, view_key_id, "video", ""); break; } { Job *j = job_new(J_DOWNLOAD, view_jid); if (j) { snprintf(j->id, sizeof j->id, "%s", view_key_id); snprintf(j->kind, sizeof j->kind, "%s", view_is_video ? "vid" : "pic"); job_commit(); } break; }
    case B_VIEW_REPLY: { int ci = chat_index(view_jid); if (ci >= 0) { if (!(screen == S_CHAT && cur_chat == ci)) open_chat(ci); viewer_open = 0; Msg *m = find_msg(view_key_id); snprintf(reply_id, sizeof reply_id, "%s", view_key_id); edit_id[0] = 0; snprintf(reply_name, sizeof reply_name, "%s", m && m->from_me ? T("Tu") : (m && m->name[0] ? m->name : chats[ci].name)); snprintf(reply_text, sizeof reply_text, "%s", view_is_video ? T("Video") : T("Foto")); ime_open(IME_DRAFT, T("Scrivi un messaggio"), draft); } break; }
    case B_VIEW_REACT: popup_begin(T("Reagisci")); { static const unsigned em[6] = { 0x1F44D, 0x2764, 0x1F602, 0x1F62E, 0x1F622, 0x1F64F }; for (int i = 0; i < 6; i++) { char t[16] = ""; append_cp(t, sizeof t, em[i]); popup_add(t, A_REACT_BASE + i, 0); } } break;
    case B_THEME_UI: popup_act(A_TOGGLE_UI); break;
    case B_FAB: build_contacts(); misc_scroll = 0; screen = S_NEWCHAT; break;
    case B_SEARCH_ICON: search_scope[0] = 0; search_q[0] = 0; search_from = S_LIST; misc_scroll = 0; screen = S_SEARCH; qtag[0] = 0; ime_next = IME_SEARCH; break;
    case B_LISTMENU: popup_begin(""); popup_add(T("Nuovo gruppo"), A_NEW_GROUP, 0); popup_add(show_archived ? T("Tutte le chat") : T("Chat archiviate"), A_LIST_ARCHIVED, 0); popup_add(ui_k < 1.2f ? T("Dimensione: normale") : ui_k < 1.8f ? T("Dimensione: grande") : T("Dimensione: molto grande"), A_TOGGLE_SCALE, 0); popup_add(ui_dark ? T("Tema chiaro") : T("Tema scuro"), A_TOGGLE_UI, 0); popup_add(receipts_on ? T("Conferma viste storie: SI") : T("Conferma viste storie: NO"), A_TOGGLE_RECEIPTS, 0); popup_add(T("Profilo"), A_LIST_PROFILE, 0); popup_add(T("Esci dall'app"), A_QUIT, 0); break;
    case B_CHATMENU: if (cur_chat >= 0) popup_chat_menu(); break;
    case B_HEADER_INFO: popup_act(A_VIEW_CONTACT); break;
    case B_POPUP_CLOSE: popup_open = 0; break;
    case B_POPUP_ITEM: if (arg >= 0 && arg < npit) popup_act(pit[arg].act == A_CLOSE ? A_CLOSE : pit[arg].act); break;
    case B_UNBLOCK_BAR: popup_act(A_UNBLOCK); break;
    case B_SEARCH_BOX: ime_open(IME_SEARCH, T("Cerca"), search_q); break;
    case B_SEARCH_ROW: { jv *o = qobj("search"); if (!o) break; if (arg >= 1000) { jv *ms = json_get(o, "messages"); if (ms && arg - 1000 < ms->count) open_by_jid(json_str(ms->items[arg - 1000], "jid", "")); } else { jv *cs = json_get(o, "chats"); if (cs && arg < cs->count) open_by_jid(json_str(cs->items[arg], "jid", "")); } break; }
    case B_NC_GROUP: ngsel = 0; gsubject[0] = 0; misc_scroll = 0; screen = S_GROUP; break;
    case B_NC_CONTACT: ime_open(IME_NUMBER, T("Numero con prefisso (es. 393331234567)"), ""); break;
    case B_NC_ROW: open_chat(arg); break;
    case B_GR_SUBJECT: ime_open(IME_GSUBJECT, T("Nome del gruppo"), gsubject); break;
    case B_GR_ROW: { int k = gsel_has(chats[arg].jid); if (k >= 0) { memmove(gsel[k], gsel[k + 1], (size_t)(ngsel - k - 1) * 160); ngsel--; } else if (ngsel < 40) snprintf(gsel[ngsel++], 160, "%s", chats[arg].jid); break; }
    case B_GR_CREATE: if (gsubject[0] && ngsel > 0) {
        char body[2100], sj[260]; jesc(gsubject, sj, sizeof sj); int k = snprintf(body, sizeof body, "{\"subject\":\"%s\",\"jids\":[", sj);
        for (int i = 0; i < ngsel && k < (int)sizeof body - 200; i++) k += snprintf(body + k, sizeof body - k, "%s\"%s\"", i ? "," : "", gsel[i]);
        snprintf(body + k, sizeof body - k, "]}"); fire("/group/create", body, T("Gruppo creato")); screen = S_LIST;
      } else toast_set(T("Serve un nome e almeno un partecipante"));
      break;
    case B_CT_BLOCK: { int bl = arg; fire_jid("/block", contact_jid, bl ? ",\"block\":false" : ",\"block\":true", bl ? T("Contatto sbloccato") : T("Contatto bloccato")); int ci = chat_index(contact_jid); if (ci >= 0) chats[ci].blocked = !bl; query_jid("contact", "/contact/", contact_jid, ""); break; }
    case B_MEDIA_TAB: media_tab = arg; misc_scroll = 0; media_query(); break;
    case B_MEDIA_ITEM: { jv *a = qarr("list"); if (a && arg < a->count) { snprintf(view_jid, sizeof view_jid, "%s", media_jid); snprintf(view_key_id, sizeof view_key_id, "%s", json_str(a->items[arg], "id", "")); view_is_video = !strcmp(json_str(a->items[arg], "kind", ""), "video"); viewer_open = 1; } break; }
    case B_DOC_ITEM: { jv *a = qarr("list"); if (a && arg < a->count) { Job *j = job_new(J_DOWNLOAD, media_jid); if (j) { snprintf(j->id, sizeof j->id, "%s", json_str(a->items[arg], "id", "")); snprintf(j->text, sizeof j->text, "%s", json_str(a->items[arg], "fname", "documento")); job_commit(); } } break; }
    case B_UPD_ROW: st_mine = 0; st_idx = arg; st_item = 0; screen = S_STATUSVIEW; break;
    case B_UPD_MY: { jv *my = qres2_arr("mystatus"); if (my && my->count) { st_mine = 1; st_idx = 0; st_item = 0; screen = S_STATUSVIEW; } else { popup_begin(T("Nuova storia")); popup_add(T("Testo"), A_STORY_TEXT, 0); popup_add(T("Foto"), A_STORY_PHOTO, 0); popup_add(T("Annulla"), A_CLOSE, 0); } break; }
    case B_STORY_NEW: popup_begin(T("Nuova storia")); popup_add(T("Testo"), A_STORY_TEXT, 0); popup_add(T("Foto"), A_STORY_PHOTO, 0); popup_add(T("Annulla"), A_CLOSE, 0); break;
    case B_FOLLOW_CH: ime_open(IME_CHLINK, T("Link del canale"), ""); break;
    case B_ST_VIEWERS: { jv *its = qres2_arr("mystatus"); if (its && st_item < its->count) { char e[100], pth[200]; url_enc(json_str(its->items[st_item], "id", ""), e, sizeof e); snprintf(pth, sizeof pth, "/viewers/%s", e); query2("viewers", pth); viewers_wait = 1; } break; }
    case B_ST_PREV: st_step(-1); break;
    case B_ST_NEXT: st_step(1); break;
    case B_COMM_ROW: { jv *a = qarr("comm"); if (!a) break; int ci = arg / 100, gi = arg % 100; if (ci < a->count) { jv *g = json_get(a->items[ci], "groups"); if (g && gi < g->count) open_by_jid(json_str(g->items[gi], "id", "")); } break; }
    default: break;
  }
}
static void handle_ime_extra(int ctx, const char *t) {
  char e[400], e2[400], p[900];
  switch (ctx) {
    case IME_SEARCH: snprintf(search_q, sizeof search_q, "%s", t); url_enc(t, e, sizeof e); url_enc(search_scope, e2, sizeof e2); snprintf(p, sizeof p, "/search?q=%s&jid=%s", e, e2); misc_scroll = 0; query("search", p); break;
    case IME_NUMBER: snprintf(nc_number, sizeof nc_number, "%s", t); ime_next = IME_CNAME; break;
    case IME_CNAME: url_enc(nc_number, e, sizeof e); url_enc(t, e2, sizeof e2); snprintf(p, sizeof p, "/checknumber?num=%s&name=%s", e, e2); qtag[0] = 0; query("chk", p); nc_check = 1; toast_set(T("Controllo numero...")); break;
    case IME_BRIDGE: { char u[256]; snprintf(u, sizeof u, "%s", t); for (char *q = u; *q; q++) if (*q == ' ' || *q == '\n') *q = 0;
      if (u[0] && strncmp(u, "http", 4)) { char v[300]; snprintf(v, sizeof v, "http://%s", u); snprintf(u, sizeof u, "%s", v); }
      if (!strchr(u + 7, ':') && u[0]) strncat(u, ":8787", sizeof u - strlen(u) - 1);
      if (u[0]) { pthread_mutex_lock(&mu); snprintf(base_url, sizeof base_url, "%s", u); pthread_mutex_unlock(&mu); save_base_url(u); toast_set(T("Indirizzo salvato")); } break; }
    case IME_STORYTEXT: if (t[0]) { snprintf(story_text, sizeof story_text, "%s", t); popup_begin(T("Colore di sfondo")); const char *cn[6] = { T("Verde"), T("Blu"), T("Viola"), T("Rosso"), T("Arancio"), T("Grigio") }; for (int i = 0; i < 6; i++) popup_add(cn[i], A_STORY_COL_BASE + i, 0); } break;
    case IME_CHLINK: if (t[0]) { char e[700], b[800]; jesc(t, e, sizeof e); snprintf(b, sizeof b, "{\"link\":\"%s\"}", e); fire("/channel/follow", b, T("Canale seguito")); } break;
    case IME_COMM_NAME: if (t[0]) { snprintf(comm_name_tmp, sizeof comm_name_tmp, "%s", t); ime_next = IME_COMM_DESC; } break;
    case IME_COMM_DESC: { char e1[300], e2[2100], b[2500]; jesc(comm_name_tmp, e1, sizeof e1); jesc(t, e2, sizeof e2); snprintf(b, sizeof b, "{\"subject\":\"%s\",\"description\":\"%s\"}", e1, e2); fire("/community/create", b, T("Community creata")); comm_refresh_at = frame_no + 360; break; }
    case IME_COMM_GNAME: if (t[0]) { char e1[300], b[600]; jesc(t, e1, sizeof e1); snprintf(b, sizeof b, "{\"id\":\"%s\",\"subject\":\"%s\"}", comm_sel, e1); fire("/community/creategroup", b, T("Gruppo creato")); comm_refresh_at = frame_no + 360; } break;
    case IME_COMM_RENAME: if (t[0]) { char e1[300], b[600]; jesc(t, e1, sizeof e1); snprintf(b, sizeof b, "{\"id\":\"%s\",\"subject\":\"%s\"}", comm_sel, e1); fire("/community/rename", b, T("Community rinominata")); comm_refresh_at = frame_no + 240; } break;
    case IME_COMM_DESC2: { char e2[2100], b[2400]; jesc(t, e2, sizeof e2); snprintf(b, sizeof b, "{\"id\":\"%s\",\"description\":\"%s\"}", comm_sel, e2); fire("/community/description", b, T("Descrizione aggiornata")); break; }
    case IME_GSUBJECT: snprintf(gsubject, sizeof gsubject, "%s", t); break;
  }
}
static void extra_tick(void) {
  { // aggiornamenti: controllo automatico dopo l'avvio, poi popup quando c'e' una versione nuova
    static int auto_done, prompted_tag_set; static char prompted[48]; UpdInfo u; upd_snapshot(&u);
    if (!auto_done && frame_no > 900 && (screen == S_LIST) && !ime_active && !popup_open && !strcmp(wa_status, "open")) { auto_done = 1; upd_check(0); }
    { static int cl_done; if (!cl_done && frame_no > 240 && (screen == S_LIST || screen == S_PROFILE || screen == S_LOGIN) && !popup_open && !ime_active && !upd_busy()) { cl_done = 1; upd_cleanup_later(); } }   // dopo ~4 s dalla comparsa dell'app: toglie la bolla dell'installatore rimasta dopo l'aggiornamento
    if (u.must_exit) { static unsigned t0; if (!t0) t0 = frame_no; if (frame_no - t0 > 120) quit_req = 1; }
    if (u.state == UPD_AVAILABLE && !popup_open && !ime_active && !upd_busy() && (screen == S_LIST || screen == S_PROFILE) && !(prompted_tag_set && !strcmp(prompted, u.tag))) {
      int failed_before = !strcmp(u.last_fail_tag, u.tag) && u.last_fail_code;
      prompted_tag_set = 1; snprintf(prompted, sizeof prompted, "%s", u.tag);
      if (u.can_install && !(failed_before && u.quiet)) { crumb("aggiornamento automatico a %s", u.tag); upd_install(); }   // obbligatorio: parte da solo, senza chiedere
      else if (u.can_install) { char tt[160]; snprintf(tt, sizeof tt, T("Aggiornamento %s non riuscito prima (codice %08X)"), u.tag, (unsigned)u.last_fail_code); popup_begin(tt); popup_add(T("Riprova"), A_UPD_RETRY, 0); popup_add(T("Chiudi"), A_UPD_LATER, 0); }
      else { char tt[160]; snprintf(tt, sizeof tt, T("Nuova versione %s: installa la versione completa"), u.tag); popup_begin(tt); popup_add(T("Scarica il pacchetto"), A_UPD_NOW, 0); popup_add(T("Piu' tardi"), A_UPD_LATER, 0); }
    }
    if (u.state == UPD_UPTODATE) { if (!u.quiet) { char t[96]; snprintf(t, sizeof t, T("Gia' aggiornato (%s)"), u.current); toast_set(t); } upd_dismiss(); }
    if (u.state == UPD_ERROR && !popup_open && !ime_active) { if (u.quiet) upd_dismiss(); else { popup_begin(u.msg); if (u.can_install && u.tag[0]) popup_add(T("Riprova"), A_UPD_RETRY, 0); popup_add(T("Chiudi"), A_UPD_ERR, 0); upd_dismiss(); } }   // dismiss subito: il popup non si riapre in ciclo
  }
  if (!viewer_open) { pthread_mutex_lock(&mu); for (int i = 0; i < nmed; i++) if (med[i].big || med[i].braw || med[i].bstate) { if (med[i].bstate == 3) continue; tex_retire(med[i].big); med[i].big = NULL; free(med[i].braw); med[i].braw = NULL; med[i].bstate = 0; } pthread_mutex_unlock(&mu); }   // la foto grande pesa: si libera appena si chiude il visualizzatore
  { // dopo una notifica: se l'app viene aperta entro 10 minuti porta direttamente nella chat del messaggio (il bridge se la ricorda)
    static unsigned lastq; static int waiting;
    if (waiting) {
      jv *o = qwait ? NULL : qobj("lastnotif");
      if (o) { waiting = 0; char jid[160]; snprintf(jid, sizeof jid, "%s", json_str(o, "jid", "")); json_free(qres); qres = NULL; qtag[0] = 0; if (jid[0]) { crumb("notifica: apro la chat %s", jid); open_by_jid(jid); } }
      else if (frame_no - lastq > 600) waiting = 0;   // risposta persa: si riprova al prossimo giro
    }
    else if (screen == S_LIST && !qwait && !popup_open && nchats > 0 && (lastq == 0 || frame_no - lastq > 480) && !strcmp(wa_status, "open")) { lastq = frame_no; query("lastnotif", "/lastnotif?take=1"); waiting = 1; }
  }
  { static int dbg_done; static char dbg_key;       // solo per i test: se esiste il file, parte direttamente su una schermata
    if (!dbg_done && screen == S_LIST) { dbg_done = 1; size_t dl; unsigned char *d = read_file("ux0:data/whatsapp/debug_start.txt", &dl);
      if (d) { dbg_key = (char)d[0]; free(d); crumb("debug: schermata di partenza '%c'", dbg_key); if (dbg_key == '1' || dbg_key == 's' || dbg_key == 'm') goto_tab(1); } }
    if (dbg_key == '1') dbg_key = 0;
    else if (dbg_key == 's' && qarr("updates")) { dbg_key = 0; st_mine = 0; st_idx = 0; st_item = 0; screen = S_STATUSVIEW; }
    else if (dbg_key == 'm' && qres2_arr("mystatus")) { dbg_key = 0; st_mine = 1; st_idx = 0; st_item = 0; screen = S_STATUSVIEW; }
    else if (dbg_key == 'r') {                          // prova della cache: apre una chat, la chiude, la riapre
      static unsigned t0; static int step;
      if (step == 0 && nchats > 3) { for (int i = 0; i < nchats; i++) if (!chats[i].channel && !chats[i].group) { open_chat(i); break; } step = 1; t0 = frame_no; crumb("debug: chat aperta (1a volta)"); }
      else if (step == 1 && frame_no - t0 > 240) { close_chat(); step = 2; t0 = frame_no; crumb("debug: chat chiusa"); }
      else if (step == 2 && frame_no - t0 > 60) { for (int i = 0; i < nchats; i++) if (!chats[i].channel && !chats[i].group) { open_chat(i); break; } step = 3; t0 = frame_no; crumb("debug: chat riaperta (2a volta)"); }
      else if (step == 3 && frame_no - t0 > 1) { dbg_key = 0; } }
    else if (dbg_key == 'o') {                          // prova: apre una chat e la lascia aperta mentre l'elenco si riordina
      static int opened_o; if (!opened_o && nchats > 20 && frame_no > 3000) { for (int i = 0; i < nchats; i++) if (!chats[i].channel && !chats[i].group) { open_chat(i); opened_o = 1; crumb("debug: aperta la chat %s (indice %d)", chats[i].jid, i); break; } }
      else if (opened_o && frame_no % 300 == 0 && cur_chat >= 0 && cur_chat < nchats) crumb("debug: chat mostrata ora: %s (indice %d)", chats[cur_chat].jid, cur_chat); }
    else if (dbg_key == 'u') {                          // prova dell'aggiornamento: accetta da solo la versione nuova
      UpdInfo u_; upd_snapshot(&u_); if (u_.state == UPD_AVAILABLE) { crumb("debug: avvio l'aggiornamento a %s", u_.tag); upd_install(); dbg_key = 0; } }
    else if (dbg_key == 'q') { static int started_q; if (!started_q && frame_no > 700 && screen == S_LIST) { started_q = 1; crumb("debug: avvio un trasferimento di prova"); start_prep("test@s.whatsapp.net", "VID123", "video", ""); } }
    else if (dbg_key == 'v') {                          // prova del visualizzatore: apre la prima chat e la prima immagine
      static int opened;
      if (!opened && nchats > 3) { for (int i = 0; i < nchats; i++) if (!chats[i].channel && !chats[i].group) { open_chat(i); break; } opened = 1; }
      else if (opened && screen == S_CHAT && cur_chat >= 0 && !strcmp(msgs_jid, chats[cur_chat].jid)) { for (int i = 0; i < nmsgs; i++) if (msgs[i].kind == K_IMAGE) { dbg_key = 0; viewer_open = 1; view_is_video = 0; snprintf(view_jid, sizeof view_jid, "%s", chats[cur_chat].jid); snprintf(view_key_id, sizeof view_key_id, "%s", msgs[i].id); break; } } }
    else if (dbg_key == 'n') { dbg_key = 0; build_contacts(); misc_scroll = 0; screen = S_NEWCHAT; }
    else if (dbg_key == 'g') { dbg_key = 0; build_contacts(); ngsel = 0; snprintf(gsubject, sizeof gsubject, "Famiglia"); misc_scroll = 0; screen = S_GROUP; }
    else if (dbg_key == 'h' || dbg_key == 'd' || dbg_key == 'y' || dbg_key == 'u' || dbg_key == 'b') {     // prove che richiedono una chat aperta
      static int stage;
      if (stage == 0 && nchats > 3) { for (int i = 0; i < nchats; i++) if (!chats[i].channel && !chats[i].group) { open_chat(i); break; } stage = 1; }
      else if (stage == 1 && screen == S_CHAT && cur_chat >= 0 && !strcmp(msgs_jid, chats[cur_chat].jid) && nmsgs > 0) {
        stage = 2; char k = dbg_key; dbg_key = 0;
        if (k == 'h') popup_act(A_VIEW_CONTACT); else if (k == 'd') popup_act(A_MEDIA); else if (k == 'u') popup_chat_menu();
        else if (k == 'y') { menu_open = 1; snprintf(menu_id, sizeof menu_id, "%s", msgs[nmsgs - 1].id); }
        else if (k == 'b') { snprintf(draft, sizeof draft, "Ciao, questo e' un messaggio di prova abbastanza lungo"); emoji_open = 1; } } }
    else if (dbg_key == 'p') { dbg_key = 0; screen = S_PROFILE; }
    else if (dbg_key == 'l') { dbg_key = 0; screen = S_LANG; misc_scroll = 0; }
    else if (dbg_key == 'a') { dbg_key = 0; screen = S_PROFILE; popup_open_about(); }
    else if (dbg_key == 'c') { for (int i = 0; i < nchats; i++) if (chats[i].channel) { dbg_key = 0; open_chat(i); break; } } }
  if (refresh_my_at && frame_no >= refresh_my_at) { refresh_my_at = 0; query2("mystatus", "/mystatus"); my_q_frame = frame_no; }
  if (viewers_wait) { jv *v = qres2_arr("viewers"); if (v) {
      viewers_wait = 0; popup_begin(T("Visualizzazioni"));
      for (int i = 0; i < v->count && i < 9; i++) { char tm[16], l[64]; fmt_time((long)json_num(v->items[i], "ts", 0), tm, sizeof tm, 1); snprintf(l, sizeof l, "%.40s  %s", json_str(v->items[i], "name", ""), tm); popup_add(l, A_CLOSE, 0); }
      if (!v->count) popup_add(T("Nessuno l'ha ancora vista"), A_CLOSE, 0); popup_add(T("Chiudi"), A_CLOSE, 0); } }
#ifndef SAFE_BUILD
  { static int nag; alive_poll(); if (!nag && alive_state() >= 0 && screen == S_LIST && !popup_open && !ime_active && plugin_upd_checked) {   // dopo ~45 s: se il plugin e' installato ma il suo file di stato non cambia, lo dico
      nag = 1;
      if (plugin_installed() && alive_state() == 0) { crumb("plugin installato ma non attivo (file di stato fermo)"); popup_begin(T("Il plugin delle notifiche non e' attivo")); popup_add(T("Riavvia la Vita ora"), A_REBOOT, 0); popup_add(T("Piu' tardi"), A_CLOSE, 0); } } }
#endif
  crash_pending = 0;   // l'uscita non pulita (es. chiusura dalla Vita) resta solo nel registro: nessuna finestra
  if ((screen == S_LIST || screen == S_LOGIN) && !ime_active && !popup_open && !crash_pending && !plugin_upd_checked) {
    plugin_upd_checked = 1;
#ifndef SAFE_BUILD
    if (plugin_installed() && plugin_differs()) { crumb("aggiorno il plugin notifiche"); if (install_plugin()) { toast_set(T("Plugin notifiche aggiornato")); ask_reboot(); } }
#endif
  }
  if ((screen == S_LIST || screen == S_LOGIN) && !ime_active && !popup_open && !crash_pending && plugin_upd_checked) plugin_prompt_once();
  if (ime_next && !ime_active) {
    int c = ime_next; ime_next = 0;
    if (c == IME_SEARCH) ime_open(IME_SEARCH, T("Cerca"), search_q);
    else if (c == IME_CNAME) ime_open(IME_CNAME, T("Nome (facoltativo)"), "");
    else if (c == IME_COMM_DESC) ime_open(IME_COMM_DESC, T("Descrizione (facoltativa)"), "");
  }
  if (nc_check) { jv *o = qobj("chk"); if (o) { nc_check = 0; if (json_bool(o, "exists", 0)) { snprintf(open_after, sizeof open_after, "%s", json_str(o, "jid", "")); open_after_t = 180; } else toast_set(T("Numero non presente su WhatsApp")); } }
  if (open_after[0]) { int i = chat_index(open_after); if (i >= 0) { open_chat(i); open_after[0] = 0; } else if (--open_after_t <= 0) open_after[0] = 0; }
}
static void extra_init(void) {
  { size_t l; unsigned char *b = read_file("ux0:data/whatsapp/seen.bin", &l); if (b) { nseen = (int)(l / sizeof(unsigned)); if (nseen > 800) nseen = 800; memcpy(seen_h, b, nseen * sizeof(unsigned)); free(b); } }
  { SceUID rf = sceIoOpen("ux0:data/whatsapp/receipts_off.txt", SCE_O_RDONLY, 0); if (rf >= 0) { sceIoClose(rf); receipts_on = 0; } }
  SceUID f = sceIoOpen("ux0:data/whatsapp/theme.txt", SCE_O_RDONLY, 0);
  if (f >= 0) { char d = '0'; sceIoRead(f, &d, 1); sceIoClose(f); if (d >= '0' && d <= '5') theme_idx = d - '0'; }
}
