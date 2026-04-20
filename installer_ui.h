#ifndef NEXUS_INSTALLER_UI_H
#define NEXUS_INSTALLER_UI_H

#include "window.h"

/* Debe coincidir con WM_TITLE_H / WM_CLIENT_INSET en window.c */
#define INSTALLER_WIN_TITLE_H   34
#define INSTALLER_WIN_INSET     4

typedef enum InstallerState {
    WELCOME,
    TIMEZONE,
    DISK_SETUP,
    INSTALLING,
    FINISHED
} InstallerState;

extern InstallerState current_step;

void draw_installer_content(const Window* win);

#endif
