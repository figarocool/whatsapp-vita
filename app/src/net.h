#pragma once
#include <stddef.h>
int net_start(void);
// GET/POST su base_url+path. Ritorna 0 e buffer malloc (da free) in *out, lunghezza in *len.
int http_get(const char *url, char **out, size_t *len);
int http_get_t(const char *url, char **out, size_t *len, long timeout_s);
typedef int (*dl_progress_fn)(unsigned long long done, unsigned long long total);   // ritorna != 0 per annullare
int http_download_file(const char *url, const char *path, long timeout_s, dl_progress_fn cb);   // 0 ok, -1 errore, -2 annullato   // con tempo massimo scelto (video convertiti)
int http_post_json(const char *url, const char *body);
int http_probe(const char *url);
int http_post_buf(const char *url, const void *data, size_t len, const char *ctype);
int http_post_json_resp(const char *url, const char *body, char **out);
