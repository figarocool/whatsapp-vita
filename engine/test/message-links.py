#!/usr/bin/env python3
"""Compile the actual URL parser, message renderer and browser handler on the host."""
from pathlib import Path
import subprocess, tempfile
root = Path(__file__).resolve().parents[2]
main = (root / 'app/src/main.c').read_text()
functions = main[main.index('static void draw_message_text('):main.index('// ======================================================================\n// IME')]
source = r'''
#include <assert.h>
#include <stdio.h>
#include "message_links.h"
#define K_DELETED 7
#define C_SUB 1
#define C_TEXT 2
#define C_BLUE 3
#define BFONT 18
#define LH 24
#define B_LINK 100
#define T(s) s
#define crumb(...) ((void)0)
typedef struct { int kind; char *text; char **lines; int nlines; } Msg;
static Msg *msgs; static int nmsgs;
static int hits, ids[20], xs[20], ys[20], widths[20], opened, failed;
static char launched[MESSAGE_URL_MAX];
static int tw(int size, const char *s) { (void)size; return strlen(s)*10; }
static void text(int x,int y,int c,int size,const char *s) {(void)x;(void)y;(void)c;(void)size;(void)s;}
static void vita2d_draw_rectangle(int x,int y,int w,int h,int c){(void)x;(void)y;(void)w;(void)h;(void)c;}
static void btn(int x,int y,int w,int h,int id,int arg){assert(id==B_LINK&&h==LH);xs[hits]=x;ys[hits]=y;widths[hits]=w;ids[hits++]=arg;}
static void i18n_system_lang(void) {}
typedef struct {const char *str; unsigned strlen,launchMode,reserved;} SceAppUtilWebBrowserParam;
static int sceAppUtilLaunchWebBrowser(SceAppUtilWebBrowserParam *p){assert(p->strlen==strlen(p->str)&&p->launchMode==0&&!p->reserved);strcpy(launched,p->str);opened++;return failed?-1:0;}
static void toast_set(const char *s){assert(failed&&s[0]);}
'''+functions+r'''
int main(void) {
 MessageLink links[MESSAGE_LINKS_MAX]; char url[MESSAGE_URL_MAX];
 const char *s="Guarda (https://example.org/a_(b)). Poi www.test.it/x?q=1&b=2!";
 assert(message_links(s,links)==2);
 assert(message_link_url(s,0,url)&&!strcmp(url,"https://example.org/a_(b)"));
 assert(message_link_url(s,1,url)&&!strcmp(url,"https://www.test.it/x?q=1&b=2"));
 assert(!message_link_url(s,2,url));
 assert(message_links("javascript:foo ftp://x abcwww.test.it https://",links)==0);
 assert(message_link_url("HTTPS://test.it/path#anchor",0,url)&&!strcmp(url,"HTTPS://test.it/path#anchor"));
 char longurl[3000]; memset(longurl,'a',sizeof longurl);memcpy(longurl,"https://",8);longurl[2999]=0;assert(message_links(longurl,links)==0);
 char *lines[]={"Guarda https://exa","mple.org/a_(b).","Poi www.test.it/x?q=1&b=2!"};
 Msg m={0,"Guarda  https://example.org/a_(b).\nPoi www.test.it/x?q=1&b=2!",lines,3};
 draw_message_text(&m,2,10,20);
 assert(hits==3&&ids[0]==64&&ids[1]==64&&ids[2]==65);
 assert(xs[0]==80&&ys[0]==20&&widths[0]==110);
 assert(xs[1]==10&&ys[1]==44&&widths[1]==140);
 assert(xs[2]==50&&ys[2]==68&&widths[2]==210);
 msgs=&m;nmsgs=1;open_message_link(1);
 assert(opened==1&&!strcmp(launched,"https://www.test.it/x?q=1&b=2"));
 failed=1;open_message_link(0);assert(opened==2);
 open_message_link(32);assert(opened==2);
 m.kind=K_DELETED;hits=0;draw_message_text(&m,0,0,0);open_message_link(0);assert(!hits&&opened==2);
 puts("OK: URL punctuation, normalization, limits, wrapped clickable spans, browser launch and failures");
}
'''
with tempfile.TemporaryDirectory() as tmp:
    path=Path(tmp)/'test.c';path.write_text(source)
    exe=Path(tmp)/'test'
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-I',str(root/'app/src'),str(path),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
assert 'if (id == B_LINK) { id = B_MSG; arg /= MESSAGE_LINKS_MAX; }' in main
assert 'case B_LINK: open_message_link(arg); break;' in main
