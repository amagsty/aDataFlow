#ifndef _SDCARD_H
#define _SDCARD_H

#include <Arduino.h>
#include <FS.h>
#include <SD_MMC.h>
#include "main.h"
#include "pins.h"
#include "buttons.h"

// retry next random file/folder name until this number is reached
#define CREATE_MAX_RETRY 10

void sdcard_init(void);
extern bool is_rec_status_changing;
void rec(String &str, const char *dir);
void sdcard_toggle_rec(void);

#endif