import socket,socketserver
payload=bytes(range(256))*130
received={}
class FTPHandler(socketserver.StreamRequestHandler):
 def handle(self):
  self.wfile.write(b'220-Test server\r\n220 Ready\r\n');listener=None
  try:
   while line:=self.rfile.readline():
    parts=line.decode().rstrip('\r\n').split(' ',1);cmd=parts[0];arg=parts[1] if len(parts)>1 else ''
    if cmd=='USER':reply=b'331 Password\r\n'
    elif cmd=='PASS':reply=b'230 Logged in\r\n' if arg!='bad' else b'530 Denied\r\n'
    elif cmd=='TYPE':reply=b'200 Binary\r\n'
    elif cmd=='PASV':
     listener=socket.socket();listener.bind(('127.0.0.1',0));listener.listen();port=listener.getsockname()[1]
     reply=f'227 Passive (127,0,0,1,{port//256},{port%256})\r\n'.encode()
    elif cmd in ('LIST','RETR','STOR'):
     self.wfile.write(b'150 Opening data\r\n');conn,_=listener.accept();listener.close();listener=None
     with conn:
      if cmd=='STOR':
       chunks=[]
       while chunk:=conn.recv(4096):chunks.append(chunk)
       received[arg]=b''.join(chunks)
      else:conn.sendall(b'-rw file.bin\r\n' if cmd=='LIST' else payload)
     reply=b'226 Done\r\n'
    else:reply=b'500 Unsupported\r\n'
    self.wfile.write(reply)
  except (BrokenPipeError,ConnectionResetError):pass
  finally:
   if listener:listener.close()
