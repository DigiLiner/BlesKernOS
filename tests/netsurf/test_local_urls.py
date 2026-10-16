#!/usr/bin/env python3
"""Check real URL resolution and local-file fetches with host file adapters."""
from pathlib import Path
import subprocess
import tempfile
ROOT=Path(__file__).resolve().parents[2]
s=(ROOT/'programs/netsurf/platform.c').read_text()
helpers=s[s.index('static uint32_t nsbk_strlen('):s.index('static void nsbk_set_error(')]
urls=s[s.index('void nsbk_url_normalize('):s.index('static bool nsbk_url_parts(')]
fetch=s[s.index('static int nsbk_uri_hex('):s.index('static bool nsbk_fetch_follow(')]
struct=s[s.index('typedef struct {\n    int status;'):s.index('} nsbk_fetch_result_t;')+len('} nsbk_fetch_result_t;')]
prefix=r'''
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#define TYPES_H
#include "platform.h"
static const char *test_path;
bool bk_file_read_all(const char *path,void **out,uint32_t *length){
 assert(!strcmp(path,test_path));*length=6;*out=malloc(6);memcpy(*out,"body{}",6);return true;
}
void bk_sys_free(void *p){free(p);}
'''
checks=r'''
int main(void){
 char url[1024];
 nsbk_url_normalize(url,sizeof(url)," /SYSTEM/USER/index.html ");assert(!strcmp(url,"file:///SYSTEM/USER/index.html"));
 nsbk_url_normalize(url,sizeof(url),"example.com");assert(!strcmp(url,"http://example.com"));
 nsbk_url_resolve(url,sizeof(url),"file:///SYSTEM/USER/site/index.html","../main.css");assert(!strcmp(url,"file:///SYSTEM/USER/main.css"));
 nsbk_url_resolve(url,sizeof(url),"file:///SYSTEM/USER/site/index.html","/SYSTEM/base.css");assert(!strcmp(url,"file:///SYSTEM/base.css"));
 nsbk_url_resolve(url,sizeof(url),"https://example.com/site/index.html","../main.css");assert(!strcmp(url,"https://example.com/main.css"));
 nsbk_url_resolve(url,sizeof(url),"https://example.com/site/","file:///SYSTEM/USER/index.html");assert(!strcmp(url,"file:///SYSTEM/USER/index.html"));
 nsbk_url_resolve(url,sizeof(url),"file:///SYSTEM/USER/site/index.html","#hola");assert(!strcmp(url,"file:///SYSTEM/USER/site/index.html#hola"));
 for(unsigned capacity=1;capacity<12;capacity++){
  char guarded[14];memset(guarded,0x7f,sizeof(guarded));nsbk_url_normalize(guarded+1,capacity,"/abc");assert(guarded[0]==0x7f && guarded[capacity+1]==0x7f);
 }
 char data[10];nsbk_fetch_result_t result;
 test_path="/SYSTEM/USER/test file.css";
 assert(nsbk_fetch_file(data,sizeof(data),"file:///SYSTEM/USER/test%20file.css","GET",&result));
 assert(!strcmp(data,"body{}") && result.body_offset==0 && result.body_length==6 && result.status==200 && !strcmp(result.content_type,"text/css"));
 assert(!nsbk_fetch_file(data,6,"file:///SYSTEM/USER/test%20file.css","GET",&result));
 assert(!nsbk_fetch_file(data,sizeof(data),"file://remote/test.css","GET",&result));
 assert(!nsbk_fetch_file(data,sizeof(data),"file:///bad%00.css","GET",&result));
 assert(!nsbk_fetch_file(data,sizeof(data),"file:///bad%","GET",&result));
 assert(!nsbk_fetch_file(data,sizeof(data),"file:///test.css","POST",&result));
 puts("PASS: file/HTTP relative URLs, UTF-8 byte preservation, percent paths, bounds, MIME and invalid files");
}
'''
with tempfile.TemporaryDirectory(prefix='bk-browser-files-') as directory:
 p=Path(directory);(p/'test.c').write_text(prefix+struct+helpers+urls+fetch+checks)
 subprocess.run(['gcc','-std=c11','-O2','-Wall','-Wextra','-fsanitize=address,undefined','-fno-pie','-no-pie','-I'+str(ROOT/'programs/netsurf'),'-idirafter',str(ROOT/'sdk/include'),str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
