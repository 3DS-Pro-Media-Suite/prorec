#include "ftp_control.h"
#include <stdio.h>
#include "app.h"
#include "ftp.h"
#include "take_writer.h"
#include "toast.h"

#define FTP_PORT 5121
#define FTP_WIFI_WAIT_MS 30000 // after a sleep or Wi-Fi loss, how long the server may wait for the network

static const FtpConfig FTP_CONFIG = {
        .greeting = "Pro Rec recordings",
        .rootPath = RECORDINGS_DIR,
        .port = FTP_PORT,
        .removeFolderContents = true,
        .canDelete = NULL,
        .isBusy = NULL, // the server cannot start while recording (ftp_control_toggle)
};

char g_ftpLabel[FTP_LABEL_SIZE];
static u64 g_ftpOfflineSince; // when the running server lost its network; 0 while it has one

void ftp_control_update_label(void)
{
	const char *state = "off";
	if (ftp_running()) {
		state = ftp_address() ? "on" : "waiting for Wi-Fi";
	}
	snprintf(g_ftpLabel, sizeof g_ftpLabel, "FTP server (" RECORDINGS_DIR "): %s", state);
}

void ftp_control_toggle(void)
{
	if (ftp_running()) {
		ftp_stop();
		toast("FTP server stopped");
	} else if (take_writer_is_active()) {
		toast_warning("Not while recording (the card is busy)");
	} else if (ftp_start(&FTP_CONFIG)) {
		toast("FTP on: ftp://%s", ftp_address());
	} else {
		toast_warning("FTP needs Wi-Fi");
	}
	g_ftpOfflineSince = 0;
	ftp_control_update_label();
}

void ftp_control_watch(u64 now)
{
	if (!ftp_running()) {
		return;
	}
	bool online = ftp_address() != NULL;
	if (online && g_ftpOfflineSince) {
		g_ftpOfflineSince = 0;
		toast("FTP on again: ftp://%s", ftp_address());
		ftp_control_update_label();
	} else if (!online && !g_ftpOfflineSince) {
		g_ftpOfflineSince = now;
		ftp_control_update_label();
	} else if (!online && now - g_ftpOfflineSince > FTP_WIFI_WAIT_MS) {
		ftp_stop();
		g_ftpOfflineSince = 0;
		toast_warning("FTP stopped: no Wi-Fi");
		ftp_control_update_label();
	}
}
