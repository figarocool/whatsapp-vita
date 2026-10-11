// libvita2d fa riferimento a sceSharedFb* (usato solo dentro la shell); in un'app normale basta restituire errore.
int sceSharedFbClose(int id) { (void)id; return -1; }
int _sceSharedFbOpen(int a, int b) { (void)a; (void)b; return -1; }
int sceSharedFbGetInfo(int id, void *info) { (void)id; (void)info; return -1; }
int sceSharedFbBegin(int id, void *info) { (void)id; (void)info; return -1; }
int sceSharedFbEnd(int id) { (void)id; return -1; }
