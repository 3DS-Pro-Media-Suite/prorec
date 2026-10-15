// The FTP server from the app's side: switching it on and off, its menu label, and giving up on it when Wi-Fi
// stays away after a sleep.
#pragma once
#include <3ds.h>

#define FTP_LABEL_SIZE 64

extern char g_ftpLabel[FTP_LABEL_SIZE]; // the server's row in the setup menu: whether it runs

void ftp_control_update_label(void);
void ftp_control_toggle(void);
// The server waits for Wi-Fi by itself after a sleep; the app gives up on it after a while.
void ftp_control_watch(u64 now);
