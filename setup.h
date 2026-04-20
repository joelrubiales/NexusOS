#ifndef SETUP_H
#define SETUP_H

typedef struct {
    int  language;        /* 0=English, 1=Espanol, 2=Portugues, 3=Francais, 4=Deutsch */
    int  kb_layout;       /* 0=us, 1=es, 2=pt-br, 3=fr, 4=de */
    int  timezone;        /* index into tz list */
    char username[32];
    char password[32];
    char hostname[32];
    int  disk_target;     /* 0=sda, 1=sdb ... (simulated) */
    int  disk_format;     /* 0=ext4, 1=btrfs, 2=xfs */
    int  install_desktop; /* 1=yes, 0=server only */
    int  completed;       /* 1 after successful setup */
} SetupConfig;

/* Runs the full installer wizard. Returns 1 if completed, 0 if cancelled. */
int setup_run(SetupConfig* cfg);

#endif
