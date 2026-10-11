#pragma once
typedef enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ } jtype;
typedef struct jv {
  jtype t; double n; char *s;
  struct jv **items; char **keys; int count;
} jv;
jv *json_parse(const char *src);
void json_free(jv *v);
jv *json_get(const jv *o, const char *key);
const char *json_str(const jv *o, const char *key, const char *def);
double json_num(const jv *o, const char *key, double def);
int json_bool(const jv *o, const char *key, int def);
