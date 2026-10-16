#!/usr/bin/env python3
"""Actual command HTTP parsers, error handling, redirects, ping and netstat."""
from pathlib import Path
import subprocess,tempfile
ROOT=Path(__file__).resolve().parents[2]
h=(ROOT/'tests/commands/host_adapter.h').read_text()
s=r'''
static int mode,calls,pings,renewed;
bool bk_net_resolve(const char *host,uint8_t ip[4],uint32_t timeout){ip[0]=127;ip[1]=ip[2]=0;ip[3]=1;return true;}
bool bk_net_ping(const uint8_t ip[4],uint32_t timeout,uint32_t *elapsed){assert(timeout==50);*elapsed=2;return ++pings==1;}
void bk_sys_sleep_ms(uint32_t ms){}
bool bk_net_dhcp(uint32_t timeout){renewed++;return true;}
bool bk_net_get_info(bk_net_info_t *n){memset(n,0,sizeof(*n));strcpy(n->device,"fake0");n->configured=true;n->link_up=true;n->mtu=1500;return true;}
uint32_t bk_net_socket_snapshot(bk_socket_info_t *out,uint32_t cap){assert(cap>=2);memset(out,0,sizeof(*out)*2);out[0].type=BK_NET_SOCKET_TCP;out[0].state=6;out[0].local_port=80;out[1].type=BK_NET_SOCKET_UDP;out[1].local_port=53;return 2;}
bool bk_file_write_all(const char *p,const void *d,uint32_t n){store(p,d,n);return true;}
int32_t bk_net_http_exchange_quiet(const char *url,const void *request,uint32_t length,void *data,uint32_t cap,uint32_t timeout){
 const char *r=request;calls++;assert(strstr(r,"Accept-Encoding: identity\r\n"));assert(strstr(r,"Connection: close\r\n"));assert(length==strlen(r));
 const char *response="HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello";
 if(mode==1){assert(!strncmp(r,"HEAD ",5));response="HTTP/1.1 200 OK\r\nContent-Length: 5000\r\n\r\n";}
 if(mode==2 && calls==1)response="HTTP/1.1 302 Found\r\nLocation: /next\r\nContent-Length: 0\r\n\r\n";
 if(mode==2 && calls==2)assert(!strcmp(url,"http://example.test/next"));
 if(mode==3)response="HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n2\r\nhe\r\n3;extension=x\r\nllo\r\n0\r\n\r\n";
 if(mode==4)response="HTTP/1.1 200 OK\r\nContent-Length: 8\r\n\r\nhello";
 if(mode==5)response="HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n99\r\nshort";
 if(mode==6)response="HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n";
 if(mode==7)response="HTTP/1.1 200 OK\r\nContent-Encoding: gzip\r\nContent-Length: 5\r\n\r\nhello";
 if(mode==8)return cap;
 memcpy(data,response,strlen(response));return strlen(response);
}
static int invoke_net(int count,char **argv){used=0;output[0]=0;calls=0;return command_network(count,argv);}
#define NET(expected,...) do {char *args[]={__VA_ARGS__};int code=invoke_net(sizeof(args)/sizeof(args[0]),args);if(code!=(expected)){fprintf(stderr,"%s: got %d expected %d: %s",args[0],code,expected,output);abort();}}while(0)
int main(void){
 NET(0,"curl","http://example.test/");assert(!strcmp(output,"hello"));
 mode=1;NET(0,"curl","-I","http://example.test/");assert(strstr(output,"Content-Length: 5000"));
 mode=2;NET(0,"curl","-L","http://example.test/");assert(calls==2&&!strcmp(output,"hello"));
 mode=3;NET(0,"wget","http://example.test/","download");contents("download","hello");
 for(mode=4;mode<=8;mode++){store("download","KEEP",4);NET(mode==6?1:2,"wget","http://example.test/","download");contents("download","KEEP");}
 mode=0;NET(2,"curl","http://example.test/\r\nbad");assert(calls==0);
 NET(2,"curl","ftp://example.test/");assert(calls==0);
 NET(0,"ping","-n","2","-w","50","-i","0","127.0.0.1");assert(strstr(output,"Enviados=2 recibidos=1 perdidos=1"));
 NET(2,"ping","-n","4294967296","host");
 NET(0,"ipconfig","/renew");assert(renewed==1&&strstr(output,"fake0"));
 NET(0,"netstat","-t");assert(strstr(output,"TCP")&&strstr(output,"LISTEN")&&!strstr(output,"UDP"));
 puts("PASS: actual native HTTP commands, HEAD, redirects, chunk extensions/decoding, truncation/errors preserve files, URL validation, ping flags/statistics, DHCP and socket filters");
}
'''
with tempfile.TemporaryDirectory(prefix='bk-netcommands-') as d:
 p=Path(d);(p/'test.c').write_text(h+s)
 subprocess.run(['gcc','-O1','-ffunction-sections','-fdata-sections','-fsanitize=address,undefined','-fno-pie','-no-pie','-DTYPES_H','-include','stdint.h','-include','stdbool.h','-include','stddef.h','-I'+str(ROOT/'system/commands'),str(p/'test.c'),str(ROOT/'system/commands/common.c'),'-Wl,--gc-sections','-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],cwd=p,check=True)
