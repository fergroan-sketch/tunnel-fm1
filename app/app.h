/* SPDX-License-Identifier: MPL-2.0 */
/* TUNNEL FM-1: a hard, hypnotic Detroit techno machine for the M-VAVE FM-1, on fm1-base. Unofficial: not affiliated
 * with M-VAVE. This header is the app's half of the contract with the platform. */
#pragma once
#include <stdint.h>
#include "plat.h"

#define APP_NAME "TUNNEL FM-1"
#define APP_SPLASH_RGB RGB(120, 230, 255)
#define APP_SPLASH                       /* the boot screen: app_splash (app.c), a tunnel of frames */
void app_splash(void);
#define APP_BLOCK 256u                   /* frames per audio block: 5.8 ms at 44.1 kHz */
#define APP_TOUCH                        /* velocity from the keys (lib/touch.h) */
#define APP_BLE_AT_BOOT (!tun_ble_off())
int tun_ble_off(void);

/* the store: the settings, eight objects of four user presets (U01..U32), the panel calibration (the platform's),
 * Bluetooth's pairing */
enum { OBJ_SETTINGS, OBJ_USER0, OBJ_USER1, OBJ_USER2, OBJ_USER3, OBJ_USER4, OBJ_USER5, OBJ_USER6, OBJ_USER7,
       OBJ_PANEL, OBJ_BLE, OBJ_NOBJ };
#define APP_STORE_MAP {0xFC000u, 0xC0000u, 0xC2000u, 0xC4000u, 0xC6000u, 0xC8000u, 0xCA000u, 0xCC000u, 0xCE000u, \
                       0xDC000u, 0xDE000u}

void app_init(void);
void app_render(int32_t *lr, uint32_t n, uint32_t master_q12);
void app_panic(void);
void app_ui_init(void);
void app_frame(void);
void app_input_only(void);
void app_say(const char *a, const char *b);

#define APP_HOST_EXPECT
int app_expect(const char *what, int *got);
