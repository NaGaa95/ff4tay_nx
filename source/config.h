/* config.h -- global configuration and config file handling
 *
 * Copyright (C) 2021 fgsfds, Andy Nguyen
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef __CONFIG_H__
#define __CONFIG_H__

// newlib heap cap; the engine's allocations (textures, decoded assets) all
// come from here via the malloc import, the rest of the address space backs
// the .so load zone (see __libnx_initheap in main.c)
#define MEMORY_MB 512

// FF4: The After Years (com.square_enix.android_googleplay.FF4AY_GP, v1.0.13)
// ships the Cuore engine as libff4a.so. Unlike CTW there is no C++ runtime
// donor: libff4a statically links libc++/libc++abi (no std::/_Znwm/__cxa_throw
// imports), so we load a single module.
#define SO_NAME "libff4a.so"
#define OBB_NAME "main.obb"
#define CONFIG_NAME "config.txt"
#define LOG_NAME "debug.log"

// per-line SD card writes cost a lot of performance; set to 1 only when
// investigating a crash or misbehavior
#define DEBUG_LOG 0

// actual screen size in use right now
extern int screen_width;
extern int screen_height;

// language enum matches the order of the array in MainActivity.loadFile, which
// selects the "<lang>.lproj/" data prefix:
//   0 ru  1 th  2 ja  3 en  4 fr  5 de  6 it  7 es  8 zh_CN  9 zh_TW  10 ko  11 pt
#define LANG_EN 3

typedef struct {
  int screen_width;
  int screen_height;
  int widescreen; // use a 16:9 game viewport instead of the legacy aspect
  int language;
} Config;

extern Config config;

int read_config(const char *file);
int write_config(const char *file);

#endif
