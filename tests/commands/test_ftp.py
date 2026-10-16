#!/usr/bin/env python3
"""Actual passive FTP command against a local, deliberately small FTP server."""
from pathlib import Path
import subprocess,tempfile,socket,socketserver,threading
ROOT=Path(__file__).resolve().parents[2]
from fixtures import FTPHandler,payload,received
server=socketserver.ThreadingTCPServer(('127.0.0.1',0),FTPHandler);server.daemon_threads=True
threading.Thread(target=server.serve_forever,daemon=True).start()
h=(ROOT/'tests/commands/host_adapter.h').read_text()
adapters=r'''
#include <sys/socket.h>
#include <arpa/inet.h>
bool bk_net_resolve(const char *host,uint8_t ip[4],uint32_t ms){return inet_pton(AF_INET,host,ip)==1;}
int bk_net_socket_open(uint8_t type){return socket(AF_INET,SOCK_STREAM,0);}
bool bk_net_socket_connect(int fd,const uint8_t ip[4],uint16_t port,uint32_t ms){struct sockaddr_in a={0};a.sin_family=AF_INET;a.sin_port=htons(port);memcpy(&a.sin_addr,ip,4);return !connect(fd,(void*)&a,sizeof(a));}
int32_t bk_net_socket_send(int fd,const void *p,uint32_t n,uint32_t ms){return send(fd,p,n,0);}
int32_t bk_net_socket_receive(int fd,void *p,uint32_t n,uint32_t ms){return recv(fd,p,n,0);}
void bk_net_socket_close(int fd){close(fd);}
'''
s=(ROOT/'system/commands/ftp.c').read_text().replace('#include "common.h"','').replace('BK_COMMAND_MAIN(run)','')
main='int main(int argc,char **argv){int status=run(argc,argv);fwrite(output,1,used,stdout);return status;}\n'
try:
 with tempfile.TemporaryDirectory(prefix='bk-ftp-') as d:
  p=Path(d);(p/'test.c').write_text(h+adapters+s+main)
  subprocess.run(['gcc','-O1','-ffunction-sections','-fdata-sections','-fsanitize=address,undefined','-fno-pie','-no-pie','-DTYPES_H','-include','stdint.h','-include','stdbool.h','-include','stddef.h','-I'+str(ROOT/'system/commands'),str(p/'test.c'),str(ROOT/'system/commands/common.c'),'-Wl,--gc-sections','-o',str(p/'test')],check=True)
  host=f'127.0.0.1:{server.server_address[1]}'
  def run(*args,status=0):
   r=subprocess.run([str(p/'test'),*args],cwd=p,capture_output=True,timeout=15);assert r.returncode==status,(r.returncode,r.stdout,r.stderr)
   return r.stdout
  assert b'file.bin' in run(host,'list')
  run(host,'get','remote.bin','download');assert (p/'download').read_bytes()==payload
  (p/'upload').write_bytes(payload);run(host,'put','upload','received.bin');assert received['received.bin']==payload
  run('-p','bad',host,'get','remote.bin','download',status=2);assert (p/'download').read_bytes()==payload
  assert not list(p.glob('*.TMP'))
  print('PASS: actual FTP command, multiline greeting, passive listing, binary download/upload, denied login preserves destination, temporary files cleaned')
finally:server.shutdown();server.server_close()
