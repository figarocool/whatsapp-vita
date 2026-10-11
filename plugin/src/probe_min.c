// Plugin minimo: nessuna libc, nessun import oltre SceIofilemgr. Se la Vita lo carica crea la cartella ur0:tai/wa_ok_min.
#include <psp2/kernel/modulemgr.h>
#include <psp2/io/stat.h>
int module_start(SceSize argc, const void *args) { (void)argc; (void)args; sceIoMkdir("ur0:tai/wa_ok_min", 0777); return SCE_KERNEL_START_SUCCESS; }
int module_stop(SceSize argc, const void *args) { (void)argc; (void)args; return SCE_KERNEL_STOP_SUCCESS; }
void _start() __attribute__((weak, alias("module_start")));
