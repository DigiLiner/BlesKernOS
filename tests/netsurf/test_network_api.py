#!/usr/bin/env python3
"""Check quiet HTTP dispatch and preservation of the legacy modal wrapper."""
from pathlib import Path
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parents[2]
source = (ROOT / 'kernel/api/public_api.c').read_text()
source = source[source.index('int32_t bk_net_http_exchange_quiet('):source.index('int32_t bk_net_tls_last_error(')]
harness = r'''
#include <stdint.h>
#include <string.h>
#include <assert.h>
#define kstrncmp strncmp
static int secure, alerts, calls;
static int32_t response=-1;
static int32_t network_https_exchange(const char *u,const void *r,uint32_t n,void *d,uint32_t c,uint32_t t){
    assert(u && r && n==3 && d && c==32 && t==2000);secure=1;calls++;return response;
}
static int32_t network_http_exchange(const char *u,const void *r,uint32_t n,void *d,uint32_t c,uint32_t t){
    int32_t result=network_https_exchange(u,r,n,d,c,t);secure=0;return result;
}
static int bk_gui_network_error(const char *action,int32_t error){assert(action && error==-1);alerts++;return 0;}
'''
checks = r'''
int main(void){
    char data[32];
    assert(bk_net_http_exchange_quiet("https://example.test/","GET",3,data,32,2000)==-1);
    assert(secure && calls==1 && alerts==0);
    response=7;
    assert(bk_net_http_exchange_quiet("http://example.test/","GET",3,data,32,2000)==7);
    assert(!secure && calls==2 && alerts==0);
    assert(bk_net_http_exchange_quiet(0,"GET",3,data,32,2000)==-1 && calls==2);
    response=-1;
    assert(bk_net_http_exchange("https://example.test/","GET",3,data,32,2000)==-1);
    assert(alerts==1);
    return 0;
}
'''
with tempfile.TemporaryDirectory() as d:
    path=Path(d); (path/'check.c').write_text(harness+source+checks)
    subprocess.run(['gcc','-Wall','-Wextra','-fsanitize=address,undefined','-fno-pie','-no-pie',str(path/'check.c'),'-o',str(path/'check')],check=True)
    subprocess.run([str(path/'check')],check=True)
print('PASS: quiet HTTP API, protocol dispatch, and legacy error UI')
