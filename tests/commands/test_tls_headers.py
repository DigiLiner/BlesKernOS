#!/usr/bin/env python3
"""Actual TLS HTTP framing preserves connection-close for HEAD requests."""
from pathlib import Path
import subprocess,tempfile
ROOT=Path(__file__).resolve().parents[2]
s=(ROOT/'kernel/drivers/network/tls.c').read_text();helpers=s[s.index('static char ascii_lower('):s.index('static void tls_connection_close(')]
a=s.index('            complete = response_complete((const uint8_t *)data, total,');b=s.index('            if (complete)',a);decision=s[a:b]
code=r'''
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
#define kmemcmp memcmp
'''+helpers+'''
static bool test(const char *request,const void *data,uint32_t total,bool *close) {
    uint32_t request_length=strlen(request);bool must_close=false,complete=false;
'''+decision+'''
    *close=must_close;return complete;
}
'''+r'''
int main(void){
 const char *h="HTTP/1.1 200 OK\r\nContent-Length: 5000\r\nConnection: close\r\n\r\n";
 bool close=false;
 assert(test("HEAD / HTTP/1.1",h,strlen(h),&close)&&close);
 assert(!test("GET / HTTP/1.1",h,strlen(h),&close)&&close);
 assert(!test("HEAD / HTTP/1.1",h,15,&close));
 const char *keep="HTTP/1.1 200 OK\r\nContent-Length: 5000\r\n\r\n";
 assert(test("HEAD / HTTP/1.1",keep,strlen(keep),&close)&&!close);
 puts("PASS: actual TLS HEAD completion, partial headers, GET bodies and Connection: close preserved");
}
'''
# Keep C escapes in the generated fixture: raw Python text needs one backslash.
code=code.replace(chr(92)+chr(92)+'r',chr(92)+'r').replace(chr(92)+chr(92)+'n',chr(92)+'n')
with tempfile.TemporaryDirectory() as d:
 p=Path(d);(p/'test.c').write_text(code)
 subprocess.run(['gcc','-fsanitize=address,undefined','-fno-pie','-no-pie',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
