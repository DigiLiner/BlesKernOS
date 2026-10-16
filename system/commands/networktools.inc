static void nt_ip(const uint8_t a[4]) {kprintf("%u.%u.%u.%u",a[0],a[1],a[2],a[3]);}
static bool nt_resolve(const char *host,uint8_t address[4],uint32_t timeout) {
    return host && bk_net_resolve(host,address,timeout);
}
static int nt_ping(int argc,char **argv) {
    uint32_t count=4,timeout=3000,interval=1000;
    const char *host=NULL;
    for(int i=1;i<argc;i++) {
        if(argv[i][0]=='-' && i+1<argc) {
            const char *option=argv[i];
            bool ok;
            uint32_t n=command_number(argv[++i],&ok);
            if(!ok)return 2;
            if(command_is(option,"-n") && n && n<=1000)count=n;
            else if(command_is(option,"-w") && n && n<=60000)timeout=n;
            else if(command_is(option,"-i") && n<=60000)interval=n;
            else return command_error("ping","opcion/rango invalido");
        } else if(!host)host=argv[i];
        else return 2;
    }
    uint8_t ip[4];
    if(!host || !nt_resolve(host,ip,timeout))return command_error("ping","destino no resuelto");
    uint32_t received=0,sent=0,min=0xffffffffU,max=0;
    uint64_t sum=0;
    for(uint32_t i=0;i<count && !ft_cancel();i++) {
        uint32_t elapsed;
        sent++;
        if(bk_net_ping(ip,timeout,&elapsed)) {
            received++;
            sum+=elapsed;
            if(elapsed<min)min=elapsed;
            if(elapsed>max)max=elapsed;
            nt_ip(ip);
            kprintf(" intento=%u tiempo=%u ms\n",sent,elapsed);
        } else kprintf("Intento %u: sin respuesta\n",sent);
        if(i+1<count)bk_sys_sleep_ms(interval);
    }
    kprintf("Enviados=%u recibidos=%u perdidos=%u\n",sent,received,sent-received);
    if(received)kprintf("RTT min/prom/max=%u/%u/%u ms\n",min,(uint32_t)(sum/received),max);
    return received?0:1;
}
static int nt_info(int argc,char **argv) {
    bool renew=false;
    for(int i=1;i<argc;i++) {
        if(command_is(argv[i],"/renew")||command_is(argv[i],"--renew"))renew=true;
        else if(!command_is(argv[i],"/all")&&!command_is(argv[i],"--all"))return 2;
    }
    if(renew && !bk_net_dhcp(10000))return command_error("ipconfig","DHCP fallo");
    bk_net_info_t n;
    if(!bk_net_get_info(&n))return 2;
    kprintf("NIC %s, enlace %s, MTU %u, IPv4 %s, TLS %s\n",n.device,n.link_up?"activo":"inactivo",n.mtu,n.configured?"configurada":"sin configurar",n.tls_ready?"si":"no");
    kprintf("MAC %02x:%02x:%02x:%02x:%02x:%02x\n",n.mac[0],n.mac[1],n.mac[2],n.mac[3],n.mac[4],n.mac[5]);
    bk_console_write("IP ");
    nt_ip(n.address);
    bk_console_write(" mascara ");
    nt_ip(n.netmask);
    bk_console_write(" gateway ");
    nt_ip(n.gateway);
    bk_console_write(" DNS ");
    nt_ip(n.dns);
    kprintf("\nRX=%u TX=%u descartados=%u\n",n.rx_packets,n.tx_packets,n.rx_dropped);
    return 0;
}
static int nt_stat(int argc,char **argv) {
    uint32_t filter=0;
    for(int i=1;i<argc;i++) {
        if(command_is(argv[i],"-t"))filter=BK_NET_SOCKET_TCP;
        else if(command_is(argv[i],"-u"))filter=BK_NET_SOCKET_UDP;
        else if(!command_is(argv[i],"-a"))return 2;
    }
    bk_socket_info_t sockets[16];
    uint32_t n=bk_net_socket_snapshot(sockets,16);
    const char *states[]={"CLOSED","ALLOCATED","SYN_SENT","ESTABLISHED","CLOSE_WAIT","FIN_WAIT","LISTEN","SYN_RECEIVED"};
    bk_console_write("PROTO LOCAL REMOTO ESTADO RX_PENDIENTE\n");
    for(uint32_t i=0;i<n;i++) {
        bk_socket_info_t *s=&sockets[i];
        if(filter && s->type!=filter)continue;
        kprintf("%s  *:%u  ",s->type==BK_NET_SOCKET_TCP?"TCP":"UDP",s->local_port);
        nt_ip(s->remote_ip);
        kprintf(":%u  %s  %u\n",s->remote_port,s->type==BK_NET_SOCKET_UDP?"DATAGRAM":s->state<8?states[s->state]:"?",s->pending);
    }
    if(!n)bk_console_write("Sin sockets nativos activos.\n");
    return 0;
}
#define NT_CAP (4U*1024U*1024U)
typedef struct {char host[256],target[1024],authority[280];
bool secure;} nt_url_t;
static bool nt_url(const char *url,nt_url_t *out) {
    const char *p=url;
    if(!p)return false;
    out->secure=bk_runtime_strncmp(p,"https://",8)==0;
    if(out->secure)p+=8;
    else if(!bk_runtime_strncmp(p,"http://",7))p+=7;
    else return false;
    uint32_t n=0;
    while(p[n] && p[n]!='/' && p[n]!='?' && p[n]!='#') {
        if((uint8_t)p[n]<=32 || p[n]=='@' || n+1>=sizeof(out->authority))return false;
        n++;
    }
    if(!n)return false;
    bk_runtime_memcpy(out->authority,p,n);
    out->authority[n]=0;
    uint32_t h=0;
    while(h<n && p[h]!=':')h++;
    if(h>=sizeof(out->host))return false;
    bk_runtime_memcpy(out->host,p,h);
    out->host[h]=0;
    p+=n;
    if(!*p || *p=='#'){out->target[0]='/';
    out->target[1]=0;
    return true;}
    uint32_t used=0;
    if(*p=='?')out->target[used++]='/';
    while(*p && *p!='#') {if((uint8_t)*p<=32 || used+1>=sizeof(out->target))return false;
    out->target[used++]=*p++;}
    out->target[used]=0;
    return true;
}
static char nt_lower(char c){return c>='A'&&c<='Z'?(char)(c+32):c;}
static bool nt_equal(const char *s,uint32_t n,const char *name) {
    if(n!=bk_runtime_strlen(name))return false;
    for(uint32_t i=0;i<n;i++)if(nt_lower(s[i])!=nt_lower(name[i]))return false;
    return true;
}
static uint32_t nt_header_end(const char *s,uint32_t n) {
    for(uint32_t i=0;i+3<n;i++)if(!bk_runtime_memcmp(s+i,"\r\n\r\n",4))return i+4;
    return 0;
}
static bool nt_header(const char *raw,uint32_t end,const char *key,char *out,uint32_t cap) {
    uint32_t at=0;
    while(at<end && raw[at]!='\n')at++;
    if(at<end)at++;
    while(at<end) {
        uint32_t start=at;
        while(at<end && raw[at]!='\n')at++;
        uint32_t last=at;
        if(at<end)at++;
        if(last>start && raw[last-1]=='\r')last--;
        uint32_t colon=start;
        while(colon<last && raw[colon]!=':')colon++;
        if(!nt_equal(raw+start,colon-start,key))continue;
        colon++;
        while(colon<last && (raw[colon]==' '||raw[colon]=='\t'))colon++;
        uint32_t n=last-colon;
        if(n>=cap)return false;
        bk_runtime_memcpy(out,raw+colon,n);
        out[n]=0;
        return true;
    }
    return false;
}
/* Decode chunked bodies in place and require a complete terminating chunk. */
static int32_t nt_unchunk(char *body,uint32_t n) {
    uint32_t at=0,out=0;
    while(at<n) {
        uint32_t size=0;
        bool digit=false;
        while(at<n && body[at]!='\r') {
            char c=body[at++];
            if(c==';'){while(at<n && body[at]!='\r')at++;
            break;}
            uint32_t d=c>='0'&&c<='9'?(uint32_t)(c-'0'):c>='a'&&c<='f'?(uint32_t)(c-'a'+10):c>='A'&&c<='F'?(uint32_t)(c-'A'+10):99;
            if(d>15 || size>(0xffffffffU-d)/16)return -1;
            size=size*16+d;
            digit=true;
        }
        if(!digit || at+2>n || body[at]!='\r' || body[at+1]!='\n')return -1;
        at+=2;
        if(!size) {
            if(at+2<=n && body[at]=='\r' && body[at+1]=='\n')return (int32_t)out;
            if(nt_header_end(body+at,n-at))return (int32_t)out;
            return -1;
        }
        if(size>n-at || n-at-size<2)return -1;
        for(uint32_t i=0;i<size;i++)body[out++]=body[at++];
        if(body[at]!='\r'||body[at+1]!='\n')return -1;
        at+=2;
    }
    return -1;
}
static int nt_http(int argc,char **argv) {
    bool save=command_is(argv[0],"wget"),headers=false,follow=false;
    uint32_t timeout=15000;
    const char *url=NULL,*output=NULL;
    for(int i=1;i<argc;i++) {
        if(command_is(argv[i],"-I"))headers=true;
        else if(command_is(argv[i],"-L"))follow=true;
        else if(command_is(argv[i],"-o") && i+1<argc)output=argv[++i];
        else if(command_is(argv[i],"-t") && i+1<argc) {bool ok;
        timeout=command_number(argv[++i],&ok);
        if(!ok||timeout<1||timeout>60000)return 2;}
        else if(!url && argv[i][0]!='-')url=argv[i];
        else if(save && !output && argv[i][0]!='-')output=argv[i];
        else return 2;
    }
    if(!url || (save && !output))return command_error(argv[0],"faltan URL o archivo; use --help");
    char current[1400];
    if(bk_runtime_strlen(url)>=sizeof(current))return 2;
    bk_runtime_strncpy(current,url,sizeof(current));
    char *raw=bk_sys_alloc(NT_CAP);
    if(!raw)return 2;
    int result=2;
    for(uint32_t redirect=0;redirect<=5;redirect++) {
        nt_url_t parsed;
        if(!nt_url(current,&parsed))break;
        char request[1700];
        int len=snprintf(request,sizeof(request),"%s %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: BlesKernOS/0.9\r\nAccept-Encoding: identity\r\nConnection: close\r\n\r\n",headers?"HEAD":"GET",parsed.target,parsed.authority);
        if(len<0 || (uint32_t)len>=sizeof(request))break;
        int32_t n=bk_net_http_exchange_quiet(current,request,(uint32_t)len,raw,NT_CAP-1,timeout);
        if(n<=0 || (uint32_t)n>=NT_CAP-1)break;
        raw[n]=0;
        uint32_t end=nt_header_end(raw,(uint32_t)n);
        if(!end || end<12 || bk_runtime_strncmp(raw,"HTTP/1.",7) ||
           raw[9]<'1'||raw[9]>'5'||raw[10]<'0'||raw[10]>'9'||raw[11]<'0'||raw[11]>'9')break;
        uint32_t status=(uint32_t)(raw[9]-'0')*100+(uint32_t)(raw[10]-'0')*10+(uint32_t)(raw[11]-'0');
        char value[1400];
        if(follow && (status==301||status==302||status==303||status==307||status==308) && nt_header(raw,end,"Location",value,sizeof(value))) {
            if(redirect==5)break;
            int needed=0;
            if(!bk_runtime_strncmp(value,"https://",8)||!bk_runtime_strncmp(value,"http://",7))bk_runtime_strncpy(current,value,sizeof(current));
            else if(value[0]=='/' && value[1]=='/')needed=snprintf(current,sizeof(current),"%s:%s",parsed.secure?"https":"http",value);
            else if(value[0]=='/') {
                needed=snprintf(current,sizeof(current),"%s://%s%s",parsed.secure?"https":"http",parsed.authority,value);
            } else {
                char *slash=parsed.target;
                for(char *p=parsed.target;*p && *p!='?';p++)if(*p=='/')slash=p;
                slash[1]=0;
                needed=snprintf(current,sizeof(current),"%s://%s%s%s",parsed.secure?"https":"http",parsed.authority,parsed.target,value);
            }
            if(needed<0 || (uint32_t)needed>=sizeof(current))break;
            continue;
        }
        uint32_t bytes=(uint32_t)n-end;
        if(!headers && nt_header(raw,end,"Content-Length",value,sizeof(value))) {bool ok;
        uint32_t expected=command_number(value,&ok);
        if(!ok||bytes!=expected)break;}
        if(!headers && nt_header(raw,end,"Transfer-Encoding",value,sizeof(value))) {
            if(!nt_equal(value,bk_runtime_strlen(value),"chunked"))break;
            int32_t decoded=nt_unchunk(raw+end,bytes);
            if(decoded<0)break;
            bytes=(uint32_t)decoded;
        }
        if(!headers && nt_header(raw,end,"Content-Encoding",value,sizeof(value)) && !nt_equal(value,bk_runtime_strlen(value),"identity"))break;
        if(status>=400){kprintf("HTTP %u\n",status);
        result=1;
        break;}
        if(output) {
            /* Completed, validated response only; no partial file on HTTP errors. */
            if(!bk_file_write_all(output,headers?raw:raw+end,headers?end:bytes))break;
            kprintf("%u bytes guardados en %s (HTTP %u)\n",headers?end:bytes,output,status);
        } else ft_output(headers?raw:raw+end,headers?end:bytes);
        result=0;
        break;
    }
    bk_sys_free(raw);
    if(result==2)command_error(argv[0],"URL, transporte o respuesta incompleta/no soportada (limite 4 MiB)");
    return result;
}
int command_network(int argc,char **argv) {
    if(command_is(argv[0],"ping"))return nt_ping(argc,argv);
    if(command_is(argv[0],"ipconfig"))return nt_info(argc,argv);
    if(command_is(argv[0],"netstat"))return nt_stat(argc,argv);
    return nt_http(argc,argv);
}
