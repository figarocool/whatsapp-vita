#pragma once
int software_player_open(const char *path, int loop);
void software_player_close(void);
int software_player_active(void);
int software_player_draw(float x, float y, float w, float h);
void software_player_render_complete(void);
unsigned software_player_frames(void);
unsigned long long software_player_time_ms(void);
int software_player_ended(void);
int software_player_failed(void);
void software_player_diag(char *out, int n);
