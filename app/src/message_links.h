#ifndef WA_MESSAGE_LINKS_H
#define WA_MESSAGE_LINKS_H
#include <ctype.h>
#include <stddef.h>
#include <string.h>
#include <strings.h>
#define MESSAGE_LINKS_MAX 32
#define MESSAGE_URL_MAX 2048
typedef struct { size_t start, end; } MessageLink;
static int message_links(const char *s, MessageLink *out) {
  int n = 0;
  for (size_t i = 0; s[i] && n < MESSAGE_LINKS_MAX;) {
    int prefix = !strncasecmp(s + i, "https://", 8) ? 8 : !strncasecmp(s + i, "http://", 7) ? 7 : !strncasecmp(s + i, "www.", 4) ? 4 : 0;
    if (!prefix || (i && (isalnum((unsigned char)s[i-1]) || s[i-1] == '_' || s[i-1] == '/'))) { i++; continue; }
    size_t end = i + prefix;
    while (s[end] && !isspace((unsigned char)s[end]) && !strchr("<>\"'", s[end])) end++;
    size_t trimmed = end;
    while (trimmed > i + prefix && strchr(".,;:!?", s[trimmed-1])) trimmed--;
    // Keep balanced parentheses in paths, discard surrounding prose brackets.
    while (trimmed > i + prefix && strchr(")]}", s[trimmed-1])) {
      char close = s[trimmed-1], open = close == ')' ? '(' : close == ']' ? '[' : '{';
      int balance = 0;
      for (size_t k = i; k < trimmed; k++) { if (s[k] == open) balance++; if (s[k] == close) balance--; }
      if (balance >= 0) break;
      trimmed--;
    }
    if (trimmed > i + prefix && trimmed - i + (prefix == 4 ? 8 : 0) < MESSAGE_URL_MAX)
      out[n++] = (MessageLink){i, trimmed};
    i = end;
  }
  return n;
}
// Align wrapped lines with the original text, including collapsed spaces and split words.
static int message_link_byte(const char *source, size_t *cursor, unsigned char ch, const MessageLink *links, int n) {
  if (isspace(ch)) return -1;
  while (source[*cursor] && isspace((unsigned char)source[*cursor])) (*cursor)++;
  if ((unsigned char)source[*cursor] != ch) return -1;
  size_t pos = (*cursor)++;
  for (int i = 0; i < n; i++) if (pos >= links[i].start && pos < links[i].end) return i;
  return -1;
}
static int message_link_url(const char *source, int index, char *url) {
  MessageLink links[MESSAGE_LINKS_MAX]; int n = message_links(source, links);
  if (index < 0 || index >= n) return 0;
  MessageLink l = links[index]; size_t offset = !strncasecmp(source + l.start, "www.", 4) ? 8 : 0;
  if (offset) memcpy(url, "https://", offset);
  memcpy(url + offset, source + l.start, l.end - l.start);
  url[offset + l.end - l.start] = 0;
  return 1;
}
#endif
