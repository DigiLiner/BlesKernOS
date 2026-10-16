#include "common.h"

/* Small passive FTP client. Control replies are buffered and multiline
   greetings are consumed before sending the next command. */
typedef struct {int socket;
char buffer[1024],line[1024];
uint32_t at,used;} ftp_t;
static bool send_all(int socket,const void *buffer,uint32_t length) {
    const uint8_t *p=buffer;
    while(length){int n=bk_net_socket_send(socket,p,length,10000);
    if(n<=0)return false;
    p+=n;
    length-=(uint32_t)n;}return true;
}
static int reply(ftp_t *f) {
    int multiline=0;
    for(uint32_t lines=0;lines<100;lines++) {
        uint32_t n=0;
        for(;;) {
            if(f->at==f->used){int got=bk_net_socket_receive(f->socket,f->buffer,sizeof(f->buffer),10000);
            if(got<=0)return -1;
            f->at=0;
            f->used=got;}
            char c=f->buffer[f->at++];
            if(c=='\n')break;
            if(n+1>=sizeof(f->line))return -1;
            if(c!='\r')f->line[n++]=c;
        }
        f->line[n]=0;
        if(n<4 || f->line[0]<'1'||f->line[0]>'5'||f->line[1]<'0'||f->line[1]>'9'||f->line[2]<'0'||f->line[2]>'9')continue;
        int code=(f->line[0]-'0')*100+(f->line[1]-'0')*10+f->line[2]-'0';
        if(!multiline && f->line[3]=='-'){multiline=code;
        continue;}
        if(f->line[3]==' ' && (!multiline || multiline==code))return code;
    }
    return -1;
}
static int talk(ftp_t *f,const char *command,const char *arg) {
    char line[1024];
    for(const char *p=arg?arg:"";*p;p++)if(*p=='\r'||*p=='\n')return -1;
    int n=snprintf(line,sizeof(line),arg&&*arg?"%s %s\r\n":"%s\r\n",command,arg);
    if(n<0 || (uint32_t)n>=sizeof(line) || !send_all(f->socket,line,n))return -1;
    return reply(f);
}
static int run(int argc,char **argv) {
    const char *user="anonymous",*password="bleskernos@localhost";
    int first=1;
    while(first<argc && argv[first][0]=='-') {
        const char *opt=argv[first++];
        if(first>=argc)return 2;
        if(command_is(opt,"-u"))user=argv[first++];
        else if(command_is(opt,"-p"))password=argv[first++];
        else return 2;
    }
    if(argc-first<2 || argc-first>4)return command_error("ftp","use ftp --help");
    char host[256];
    if(bk_runtime_strlen(argv[first])>=sizeof(host))return 2;
    bk_runtime_strncpy(host,argv[first++],sizeof(host));
    uint32_t port=21;
    for(char *p=host;*p;p++)if(*p==':'){*p++=0;
    bool valid;
    port=command_number(p,&valid);
    if(!valid||!port||port>65535)return 2;
    break;}
    const char *operation=argv[first++];
    bool list=command_is(operation,"list"),get=command_is(operation,"get"),put=command_is(operation,"put");
    if((!list&&!get&&!put) || (list?argc-first>1:argc-first!=2))return 2;
    uint8_t address[4];
    if(!bk_net_resolve(host,address,5000))return command_error("ftp","host no resuelto");
    ftp_t *f=bk_sys_alloc(sizeof(*f));
    if(!f)return 2;
    bk_runtime_memset(f,0,sizeof(*f));
    f->socket=bk_net_socket_open(BK_NET_SOCKET_TCP);
    int data=-1,file=-1,result=2;
    char temporary[BK_PATH_MAX]={0};
    if(f->socket<0 || !bk_net_socket_connect(f->socket,address,(uint16_t)port,10000) || reply(f)!=220)goto done;
    int code=talk(f,"USER",user);
    if(code==331)code=talk(f,"PASS",password);
    if(code!=230 || talk(f,"TYPE","I")!=200 || talk(f,"PASV",NULL)!=227)goto done;
    char *p=f->line;
    while(*p && *p!='(')p++;
    if(!*p)goto done;
    p++;
    uint32_t values[6];
    for(uint32_t i=0;i<6;i++) {
        uint32_t value=0;
        bool digit=false;
        while(*p>='0'&&*p<='9'){digit=true;
        value=value*10+(*p++-'0');
        if(value>255)goto done;}
        if(!digit || (i<5?*p!=',':*p!=')'))goto done;
        p++;
        values[i]=value;
    }
    port=values[4]*256+values[5];
    if(!port)goto done;
    /* Use the control peer, never an arbitrary PASV-provided host. */
    data=bk_net_socket_open(BK_NET_SOCKET_TCP);
    if(data<0 || !bk_net_socket_connect(data,address,(uint16_t)port,10000))goto done;
    if(put){file=bk_file_open(argv[first],BK_FILE_READ_ONLY);
    if(file<0)goto done;}
    if(get) {
        int n=snprintf(temporary,sizeof(temporary),"%s.FTP%u.TMP",argv[first+1],bk_sys_getpid());
        bk_file_entry_t e;
        if(n<0 || (uint32_t)n>=sizeof(temporary) || bk_file_stat(temporary,&e)) {temporary[0]=0;
        goto done;}
        file=bk_file_open(temporary,BK_FILE_WRITE_ONLY|0x10U);
        if(file<0)goto done;
    }
    code=talk(f,list?"LIST":get?"RETR":"STOR",first<argc?argv[first+(put?1:0)]:NULL);
    if(code!=125 && code!=150)goto done;
    char buffer[4096];
    uint32_t total=0;
    while(!bk_proc_exit_requested()) {
        int n=put?bk_file_read(file,buffer,sizeof(buffer)):bk_net_socket_receive(data,buffer,sizeof(buffer),10000);
        if(n<0)goto done;
        if(!n)break;
        if(put){if(!send_all(data,buffer,(uint32_t)n))goto done;}
        else if(get) {
            uint32_t written=0;
            while(written<(uint32_t)n){int count=bk_file_write(file,buffer+written,n-written);
            if(count<=0)goto done;
            written+=count;}
        } else {for(int i=0;i<n;i++)bk_console_putchar(buffer[i]);}
        total+=(uint32_t)n;
    }
    if(bk_proc_exit_requested())goto done;
    bk_net_socket_close(data);
    data=-1;
    code=reply(f);
    if(code!=226 && code!=250)goto done;
    if(file>=0){bool ok=bk_file_close(file);
    file=-1;
    if(!ok)goto done;}
    if(get && !bk_file_rename(temporary,argv[first+1]))goto done;
    temporary[0]=0;
    kprintf("FTP: %u bytes transferidos.\n",total);
    result=0;
done:
    if(file>=0)bk_file_close(file);
    if(data>=0)bk_net_socket_close(data);
    if(f->socket>=0)bk_net_socket_close(f->socket);
    if(temporary[0])bk_file_remove(temporary);
    bk_sys_free(f);
    return result?command_error("ftp","fallo de autenticacion, transferencia o respuesta FTP"):0;
}
BK_COMMAND_MAIN(run)
