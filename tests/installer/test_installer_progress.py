#!/usr/bin/env python3
"""Compile the actual text-mode Setup UI against an in-memory VGA framebuffer."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/setup/installer.c').read_text()
start = source.index('static uint16_t ui_cell(')
stop = source.index('static void installer_halt_screen(', start)
ui = source[start:stop]

mock = r'''
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define SETUP_VERSION "1.0"
#define VFS_MAX_PATH 256
#define UI_WIDTH 80
#define UI_HEIGHT 25
#define VGA_BLACK 0
#define VGA_LIGHT_GREY 7
#define VGA_WHITE 15
#define VGA_BLUE 1
#define VGA_YELLOW 14
#define VGA_RED 4
#define VGA_LIGHT_GREEN 10
#define VGA_LIGHT_RED 12
#define UI_ATTR(fg,bg) ((uint8_t)(((bg)<<4)|(fg)))
#define UI_NORMAL UI_ATTR(VGA_BLACK,VGA_LIGHT_GREY)
#define UI_DESKTOP UI_ATTR(VGA_WHITE,VGA_BLUE)
#define UI_TITLE UI_ATTR(VGA_WHITE,VGA_BLUE)
#define UI_STATUS UI_ATTR(VGA_BLACK,VGA_LIGHT_GREY)
#define UI_SELECT UI_ATTR(VGA_WHITE,VGA_BLUE)
#define UI_WARNING UI_ATTR(VGA_YELLOW,VGA_RED)
#define UI_OK UI_ATTR(VGA_LIGHT_GREEN,VGA_BLACK)
#define UI_ERROR UI_ATTR(VGA_LIGHT_RED,VGA_BLACK)
#define KEY_BYTE(c) ((uint8_t)(c))
#define KEY_ENTER 13
#define KEY_ESCAPE 27
#define kstrlen strlen
static uint16_t screen[80*25];
#define VGA_TEXT_MEMORY screen
static void vga_set_cursor(int x,int y) { (void)x; (void)y; }
static int kbd_getchar(void) { return KEY_ENTER; }
typedef struct {
    uint32_t files_total, files_done, directories, bytes_total, bytes_done;
    uint32_t current_file_size, current_file_done;
    bool current_file_active;
    uint64_t unpacked_bytes, needed_bytes;
    char current_path[VFS_MAX_PATH], error[VFS_MAX_PATH];
} installer_copy_state_t;
static installer_copy_state_t g_copy;
'''
checks = r'''
static void row(int y, char *dest) {
    for (int i=0; i<80; ++i) dest[i] = (char)(screen[y*80+i]&255);
    dest[80]=0;
}
#define CHECK(c) do {if (!(c)) {fprintf(stderr,"failed line %d: %s\n",__LINE__,#c); return 1;}} while(0)
int main(void) {
    char line[81];
    g_copy.current_file_active = true;
    g_copy.current_file_size = 20U*1024U*1024U;
    g_copy.current_file_done = 5U*1024U*1024U;
    strcpy(g_copy.current_path,"/INSTALL/BOOTSTRAP/SYSTEM/PROGRAMS/SETUP.BEX");
    ui_progress(22,31,"Copiando...");
    row(0,line); CHECK(strstr(line,"BlesKernOS Setup 1.0")!=NULL);
    row(8,line); CHECK(strstr(line,"22 de 31")!=NULL);
    row(13,line); CHECK(strstr(line,"23 de 31")!=NULL);
    row(15,line); CHECK(strstr(line,"SETUP.BEX")!=NULL);
    row(17,line); CHECK(strstr(line,"5120 / 20480 KiB (25%)")!=NULL);
    row(19,line); CHECK(line[15]=='#' && line[26]=='#' && line[27]=='.');
    g_copy.current_file_done=g_copy.current_file_size;
    g_copy.current_file_active=false;
    ui_progress(23,31,"Archivo copiado correctamente.");
    row(13,line); CHECK(strstr(line,"23 de 31")!=NULL);
    row(17,line); CHECK(strstr(line,"100%")!=NULL);
    strcpy(g_copy.current_path,"/INSTALL/BOOTSTRAP/VERY/LONG/DIRECTORY/STRUCTURE/AND/EVEN/MORE/DEEP/DIRECTORIES/NAME/EXTRA/ACTUAL-FILENAME.BKL");
    ui_progress(23,31,"Copiando...");
    row(15,line); CHECK(strstr(line,"ACTUAL-FILENAME.BKL")!=NULL);
    g_copy.current_file_size=0;
    g_copy.current_file_done=0;
    g_copy.current_file_active=true;
    ui_progress(23,31,"Archivo vacio...");
    row(17,line); CHECK(strstr(line,"100%")!=NULL);
    puts("PASS: two progress bars, current file, path clipping, zero-size files, Setup 1.0");
    return 0;
}
'''
with tempfile.TemporaryDirectory() as tmp:
    c = Path(tmp) / 'progress.c'
    exe = Path(tmp) / 'progress'
    c.write_text(mock + '\n' + ui + '\n' + checks)
    subprocess.run(['gcc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                    '-Wno-unused-function', str(c), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
