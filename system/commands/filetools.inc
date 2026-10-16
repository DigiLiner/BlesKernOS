/* Streaming native utilities. No file data is loaded wholesale for copying,
   counting, searching, paging or selecting lines. */
#define FT_CHUNK 2048U
#define FT_LINE 8192U
typedef struct {int fd;
uint32_t at,used;
char data[FT_CHUNK];
bool error;} ft_reader_t;
static ft_reader_t *ft_open(const char *path) {
    int fd=bk_file_open(path,BK_FILE_READ_ONLY);
    if(fd<0)return NULL;
    ft_reader_t *r=bk_sys_alloc(sizeof(*r));
    if(!r){bk_file_close(fd);
    return NULL;}
    bk_runtime_memset(r,0,sizeof(*r));
    r->fd=fd;
    return r;
}
static bool ft_close(ft_reader_t *r) {
    if(!r)return false;
    bool ok=bk_file_close(r->fd) && !r->error;
    bk_sys_free(r);
    return ok;
}
static int ft_byte(ft_reader_t *r) {
    if(r->at==r->used) {
        int n=bk_file_read(r->fd,r->data,sizeof(r->data));
        if(n<=0){if(n<0)r->error=true;
        return -1;}
        r->used=(uint32_t)n;
        r->at=0;
    }
    return (uint8_t)r->data[r->at++];
}
static int ft_line(ft_reader_t *r,char *out,uint32_t cap) {
    uint32_t n=0;
    int c;
    while((c=ft_byte(r))>=0) {
        if(n+1>=cap){r->error=true;
        return -2;}
        out[n++]=(char)c;
        if(c=='\n')break;
    }
    out[n]=0;
    return n?(int)n:(r->error?-2:-1);
}
static void ft_output(const char *data,uint32_t n) {
    char out[FT_CHUNK+1];
    while(n) {
        uint32_t take=n>FT_CHUNK?FT_CHUNK:n;
        bk_runtime_memcpy(out,data,take);
        out[take]=0;
        /* Console strings cannot represent NUL; preserve all other bytes. */
        uint32_t start=0;
        for(uint32_t i=0;i<take;i++)if(!out[i]) {
            bk_console_write(out+start);
            bk_console_putchar(0);
            start=i+1;
        }
        bk_console_write(out+start);
        data+=take;
        n-=take;
    }
}
bool command_join(char *out,uint32_t cap,const char *base,const char *name) {
    uint32_t a=bk_runtime_strlen(base),b=bk_runtime_strlen(name);
    bool slash=a && base[a-1]!='/';
    if(a+b+(slash?1:0)+1>cap)return false;
    bk_runtime_memcpy(out,base,a);
    if(slash)out[a++]='/';
    bk_runtime_memcpy(out+a,name,b+1);
    return true;
}
static bool ft_cancel(void) {return bk_proc_exit_requested();}
static bool ft_write(int fd,const void *p,uint32_t size) {
    const uint8_t *s=p;
    while(size) {int n=bk_file_write(fd,s,size);
    if(n<=0)return false;
    s+=n;
    size-=(uint32_t)n;}
    return true;
}
static int ft_copy(int argc,char **argv) {
    bool moving=command_is(argv[0],"move"),no_clobber=false,verbose=false;
    int first=1;
    for(;first<argc && argv[first][0]=='-';first++) {
        if(command_is(argv[first],"--")){first++;
        break;}
        if(command_is(argv[first],"-n"))no_clobber=true;
        else if(command_is(argv[first],"-v"))verbose=true;
        else return command_error(argv[0],"opcion desconocida; use --help");
    }
    if(argc-first!=2)return command_error(argv[0],"uso: copy|cp|move [-n] [-v] origen destino");
    const char *src=argv[first],*dst=argv[first+1];
    char target[BK_PATH_MAX];
    bk_file_entry_t e;
    if(!bk_file_stat(src,&e) || e.type!=BK_FILE_NODE_FILE)return command_error(argv[0],"origen no es un archivo");
    if(bk_file_stat(dst,&e) && e.type==BK_FILE_NODE_DIRECTORY) {
        const char *base=src;
        for(const char *p=src;*p;p++)if(*p=='/' || *p=='\\')base=p+1;
        if(!command_join(target,sizeof(target),dst,base))return command_error(argv[0],"ruta demasiado larga");
        dst=target;
    }
    /* Kernel identity handles ./, case aliases and equivalent volume paths. */
    if(bk_file_transfer_check(src,dst)<0)return command_error(argv[0],"origen y destino son el mismo archivo");
    if(no_clobber && bk_file_stat(dst,&e))return command_error(argv[0],"el destino ya existe (-n)");
    if(moving && bk_file_rename(src,dst)){if(verbose)kprintf("%s -> %s\n",src,dst);
    return 0;}
    /* Stage beside the destination; never truncate it on a failed source read. */
    char temp[BK_PATH_MAX];
    uint32_t len=bk_runtime_strlen(dst);
    if(len+26>=sizeof(temp))return command_error(argv[0],"ruta demasiado larga");
    bool available=false;
    for(uint32_t i=0;i<100;i++) {
        int written=snprintf(temp,sizeof(temp),"%s.BK%u.%u.TMP",dst,bk_sys_getpid(),i);
        if(written<0 || (uint32_t)written>=sizeof(temp))return command_error(argv[0],"ruta demasiado larga");
        if(!bk_file_stat(temp,&e)){available=true;
        break;}
    }
    if(!available)return command_error(argv[0],"no hay nombre temporal disponible");
    int in=bk_file_open(src,BK_FILE_READ_ONLY),out=-1;
    bool ok=in>=0;
    if(ok)out=bk_file_open(temp,BK_FILE_WRITE_ONLY|0x10U);
    if(out<0)ok=false;
    char *buffer=bk_sys_alloc(16384);
    if(!buffer)ok=false;
    while(ok && !ft_cancel()) {
        int n=bk_file_read(in,buffer,16384);
        if(n<0){ok=false;
        break;}if(!n)break;
        if(!ft_write(out,buffer,(uint32_t)n))ok=false;
    }
    if(ft_cancel())ok=false;
    if(buffer)bk_sys_free(buffer);
    if(in>=0 && !bk_file_close(in))ok=false;
    if(out>=0 && !bk_file_close(out))ok=false;
    if(ok)ok=bk_file_rename(temp,dst);
    if(!ok){bk_file_remove(temp);
    return command_error(argv[0],"copia incompleta; destino conservado");}
    if(moving && !bk_file_remove(src))return command_error(argv[0],"copiado, pero no se pudo eliminar el origen");
    if(verbose)kprintf("%s -> %s\n",src,dst);
    return 0;
}
static int ft_display(int argc,char **argv) {
    bool paged=command_is(argv[0],"more"),number=false;
    int first=1;
    if(first<argc && command_is(argv[first],"-n")){number=true;
    first++;}
    if(first==argc)return command_error(argv[0],"falta archivo");
    char *line=bk_sys_alloc(FT_LINE);
    if(!line)return 2;
    int status=0;
    uint32_t row=0,index=1;
    for(int i=first;i<argc;i++) {
        ft_reader_t *r=ft_open(argv[i]);
        if(!r){status=command_error(argv[0],argv[i]);
        continue;}
        int n=0;
        if(!number && !paged) {
            char block[FT_CHUNK];
            while(!ft_cancel() && (n=bk_file_read(r->fd,block,sizeof(block)))>0)ft_output(block,(uint32_t)n);
            if(n<0)r->error=true;
            if(!ft_close(r))status=2;
            continue;
        }
        while(!ft_cancel() && (n=ft_line(r,line,FT_LINE))>=0) {
            if(number)kprintf("%u\t",index++);
            ft_output(line,(uint32_t)n);
            if(paged && ++row==20) {
                bk_console_write("\n-- Mas: Enter continuar; Q+Enter salir --");
                char key=0,c=0;
                do {
                    if(bk_console_read(&c,1)!=1){ft_close(r);
                    bk_sys_free(line);
                    return 2;}
                    if(c!='\r' && c!='\n' && !key)key=c;
                } while(c!='\n');
                bk_console_write("\n");
                row=0;
                if(key=='q' || key=='Q' || key==BK_KEY_ESCAPE){ft_close(r);
                bk_sys_free(line);
                return 0;}
            }
        }
        if(!ft_close(r))status=command_error(argv[0],"error de lectura o linea mayor a 8190 bytes");
    }
    bk_sys_free(line);
    return status;
}
static int ft_select_lines(int argc,char **argv) {
    bool tail=command_is(argv[0],"tail");
    uint32_t count=10;
    int first=1;
    bool valid;
    if(argc>2 && command_is(argv[1],"-n")) {
        count=command_number(argv[2],&valid);
        if(!valid)return 2;
        first=3;
    }
    if(argc-first!=1)return command_error(argv[0],"uso: head|tail [-n lineas] archivo");
    ft_reader_t *r=ft_open(argv[first]);
    if(!r)return command_error(argv[0],"no se pudo abrir");
    uint32_t start=0;
    if(tail && count) {
        int32_t size=bk_file_size(r->fd);
        if(size<0){ft_close(r);
        return 2;}
        /* Scan backwards in blocks, then stream the selected suffix. */
        uint32_t at=(uint32_t)size,lines=0;
        char block[FT_CHUNK];
        bool last=true,found=false;
        while(at && !found && !ft_cancel()) {
            uint32_t n=at>sizeof(block)?sizeof(block):at;
            at-=n;
            if(bk_file_seek(r->fd,(int32_t)at,0)<0 || bk_file_read(r->fd,block,n)!=(int)n){ft_close(r);
            return 2;}
            for(uint32_t j=n;j>0;j--) {
                char c=block[j-1];
                if(last){last=false;
                if(c=='\n')continue;}
                if(c=='\n' && ++lines==count){start=at+j;
                found=true;
                break;}
            }
        }
        if(bk_file_seek(r->fd,(int32_t)start,0)<0){ft_close(r);
        return 2;}
    }
    char data[FT_CHUNK];
    uint32_t used=0,lines=0;
    int c;
    while(count && !ft_cancel() && (c=ft_byte(r))>=0) {
        data[used++]=(char)c;
        bool done=!tail && c=='\n' && ++lines==count;
        if(used==sizeof(data) || done){ft_output(data,used);
        used=0;}
        if(done)break;
    }
    if(used)ft_output(data,used);
    return ft_close(r)?0:2;
}
static bool ft_match(const char *s,const char *needle,bool insensitive) {
    if(!*needle)return true;
    for(;*s;s++) {
        const char *a=s,*b=needle;
        while(*a && *b) {
            unsigned char x=*a,y=*b;
            if(insensitive){if(x>='A'&&x<='Z')x+=32;
            if(y>='A'&&y<='Z')y+=32;}
            if(x!=y)break;
            a++;
            b++;
        }
        if(!*b)return true;
    }
    return false;
}
static int ft_grep_wc(int argc,char **argv) {
    bool grep=command_is(argv[0],"grep"),insensitive=false,number=false,invert=false,counts=false;
    bool bytes=false,words=false,lines=false;
    int first=1;
    for(;first<argc && argv[first][0]=='-';first++) {
        if(command_is(argv[first],"--")){first++;
        break;}
        for(const char *p=argv[first]+1;*p;p++) {
            if(grep && *p=='i')insensitive=true;
            else if(grep && *p=='n')number=true;
            else if(grep && *p=='v')invert=true;
            else if(grep && *p=='c')counts=true;
            else if(!grep && *p=='c')bytes=true;
            else if(!grep && *p=='w')words=true;
            else if(!grep && *p=='l')lines=true;
            else return command_error(argv[0],"opcion desconocida");
        }
    }
    const char *pattern=NULL;
    if(grep && first<argc)pattern=argv[first++];
    if(first>=argc)return command_error(argv[0],"faltan argumentos; use --help");
    char *line=grep?bk_sys_alloc(FT_LINE):NULL;
    if(grep && !line)return 2;
    int result=grep?1:0;
    for(int i=first;i<argc;i++) {
        ft_reader_t *r=ft_open(argv[i]);
        if(!r){result=2;
        continue;}
        uint32_t lc=0,wc=0,bc=0,matches=0;
        if(grep) {
            int n;
            while(!ft_cancel() && (n=ft_line(r,line,FT_LINE))>=0) {
                lc++;
                if(ft_match(line,pattern,insensitive)==!invert) {
                    matches++;
                    if(!counts){if(argc-first>1)kprintf("%s:",argv[i]);
                    if(number)kprintf("%u:",lc);
                    ft_output(line,n);}
                }
            }
            if(counts)kprintf("%s:%u\n",argv[i],matches);
            if(matches && result!=2)result=0;
        } else {
            bool in_word=false;
            int c;
            while(!ft_cancel() && (c=ft_byte(r))>=0) {
                bc++;
                if(c=='\n')lc++;
                bool white=c==' '||c=='\t'||c=='\r'||c=='\n'||c=='\f'||c=='\v';
                if(!white && !in_word)wc++;
                in_word=!white;
            }
            bool all=!bytes&&!words&&!lines;
            if(lines||all)kprintf("%u ",lc);
            if(words||all)kprintf("%u ",wc);
            if(bytes||all)kprintf("%u ",bc);
            kprintf("%s\n",argv[i]);
        }
        if(!ft_close(r))result=2;
    }
    if(line)bk_sys_free(line);
    return result;
}
static int ft_compare(int argc,char **argv) {
    bool brief=command_is(argv[0],"cmp");
    int first=1;
    if(argc>1 && command_is(argv[1],"-q")){brief=true;
    first++;}
    if(argc-first!=2)return command_error(argv[0],"uso: diff [-q] archivo1 archivo2");
    ft_reader_t *a=ft_open(argv[first]),*b=ft_open(argv[first+1]);
    if(!a || !b){if(a)ft_close(a);
    if(b)ft_close(b);
    return command_error(argv[0],"no se pudieron abrir los archivos");}
    int status=0;
    if(brief) {
        uint32_t byte=0;
        int x,y;
        do {x=ft_byte(a);
        y=ft_byte(b);
        byte++;
        if(x!=y){kprintf("Difieren en byte %u\n",byte);
        status=1;
        break;}}while(x>=0 && !ft_cancel());
    } else {
        char *left=bk_sys_alloc(FT_LINE),*right=bk_sys_alloc(FT_LINE);
        uint32_t row=0;
        if(!left || !right)status=2;
        while(!status || status==1) {
            if(!left||!right||ft_cancel())break;
            int x=ft_line(a,left,FT_LINE),y=ft_line(b,right,FT_LINE);
            row++;
            if(x==-2 || y==-2){status=2;
            break;}if(x<0 && y<0)break;
            if(x!=y || (x>0 && bk_runtime_memcmp(left,right,(uint32_t)x))) {
                status=1;
                kprintf("@@ linea %u @@\n",row);
                if(x>=0){bk_console_write("- ");
                ft_output(left,x);
                if(!x||left[x-1]!='\n')bk_console_putchar('\n');}
                if(y>=0){bk_console_write("+ ");
                ft_output(right,y);
                if(!y||right[y-1]!='\n')bk_console_putchar('\n');}
            }
        }
        if(left)bk_sys_free(left);
        if(right)bk_sys_free(right);
    }
    bool ac=ft_close(a),bc=ft_close(b);
    return ac&&bc?status:2;
}
static int ft_list(int argc,char **argv) {
    bool long_form=false,all=false,only_names=false;
    const char *path=".";
    bool path_set=false;
    for(int i=1;i<argc;i++) {
        if(command_is(argv[i],"-l"))long_form=true;
        else if(command_is(argv[i],"-a"))all=true;
        else if(command_is(argv[i],"-1"))only_names=true;
        else if(!path_set && argv[i][0]!='-'){path=argv[i];
        path_set=true;}
        else return command_error(argv[0],"uso: ls [-a] [-l] [-1] [directorio]");
    }
    bk_file_entry_t *entries=bk_sys_alloc(sizeof(*entries)*COMMAND_DIR_MAX);
    uint32_t n=0;
    if(!entries)return 2;
    if(!bk_file_list_dir(path,entries,COMMAND_DIR_MAX,&n)){bk_sys_free(entries);
    return command_error(argv[0],"directorio inaccesible");}
    for(uint32_t i=0;i<n;i++) {
        if(!all && (entries[i].name[0]=='.' || (entries[i].attributes&2)))continue;
        if(long_form)kprintf("%s %10u %s\n",entries[i].type==BK_FILE_NODE_DIRECTORY?"d":"-",entries[i].size,entries[i].name);
        else kprintf("%s%s\n",entries[i].name,!only_names&&entries[i].type==BK_FILE_NODE_DIRECTORY?"/":"");
    }
    bk_sys_free(entries);
    if(n==COMMAND_DIR_MAX){bk_console_write("AVISO: limite de 128 entradas de la API; listado posiblemente incompleto.\n");
    return 1;}
    return 0;
}
static bool ft_name_match(const char *s,const char *pattern,bool insensitive) {
    bool wildcard=false;for(const char *p=pattern;*p;p++)if(*p=='*'||*p=='?')wildcard=true;
    if(!wildcard)return ft_match(s,pattern,insensitive);
    const char *star=NULL,*retry=NULL;
    while(*s) {
        char a=*s,b=*pattern;
        if(insensitive){if(a>='A'&&a<='Z')a+=32;if(b>='A'&&b<='Z')b+=32;}
        if(*pattern=='?' || (a==b && *pattern)){s++;pattern++;}
        else if(*pattern=='*'){star=pattern++;retry=s;}
        else if(star){pattern=star+1;s=++retry;}
        else return false;
    }
    while(*pattern=='*')pattern++;
    return !*pattern;
}
typedef struct {uint32_t files,dirs;
uint64_t bytes;
bool failed;
uint32_t depth,max_depth;
const char *pattern;
char kind;
bool insensitive,du,tree;} ft_walk_t;
static void ft_walk(const char *path,ft_walk_t *w) {
    if(ft_cancel()){w->failed=true;
    return;}
    bk_file_entry_t entry;
    if(!bk_file_stat(path,&entry)){kprintf("Inaccesible: %s\n",path);
    w->failed=true;
    return;}
    bool dir=entry.type==BK_FILE_NODE_DIRECTORY;
    if(dir)w->dirs++;
    else {w->files++;
    w->bytes+=entry.size;}
    if(!w->du && (!w->kind || (w->kind=='d')==dir) &&
       (!w->pattern || ft_name_match(entry.name,w->pattern,w->insensitive))) {
        if(w->tree)for(uint32_t i=0;i<w->depth;i++)bk_console_write("  ");
        kprintf("%s%s\n",w->tree?entry.name:path,dir?"/":"");
    }
    if(!dir || w->depth>=w->max_depth)return;
    bk_file_entry_t *list=bk_sys_alloc(sizeof(*list)*COMMAND_DIR_MAX);
    uint32_t n=0;
    if(!list || !bk_file_list_dir(path,list,COMMAND_DIR_MAX,&n)){if(list)bk_sys_free(list);
    w->failed=true;
    return;}
    if(n==COMMAND_DIR_MAX){kprintf("AVISO: recorrido posiblemente incompleto: %s\n",path);
    w->failed=true;}
    for(uint32_t i=0;i<n;i++) {
        if(command_is(list[i].name,".")||command_is(list[i].name,".."))continue;
        char child[BK_PATH_MAX];
        if(!command_join(child,sizeof(child),path,list[i].name)){w->failed=true;
        continue;}
        w->depth++;
        ft_walk(child,w);
        w->depth--;
    }
    bk_sys_free(list);
}
static int ft_traverse(int argc,char **argv) {
    ft_walk_t w={0};
    w.max_depth=12;
    w.du=command_is(argv[0],"du");
    w.tree=command_is(argv[0],"tree");
    const char *path=".";
    bool path_set=false;
    /* Preserve find PATTERN [PATH], also accept find PATH -name PATTERN. */
    if(command_is(argv[0],"find") && argc>1 && argv[1][0]!='-' && (argc<3 || argv[2][0]!='-')) {
        w.pattern=argv[1];
        if(argc>2)path=argv[2];
        if(argc>3)return 2;
    } else for(int i=1;i<argc;i++) {
        if(command_is(argv[i],"-i"))w.insensitive=true;
        else if(command_is(argv[i],"-name") && i+1<argc)w.pattern=argv[++i];
        else if(command_is(argv[i],"-type") && i+1<argc) {w.kind=argv[++i][0];
        if((w.kind!='f'&&w.kind!='d')||argv[i][1])return 2;}
        else if(command_is(argv[i],"-d") && i+1<argc) {bool ok;
        w.max_depth=command_number(argv[++i],&ok);
        if(!ok||w.max_depth>32)return 2;}
        else if(!path_set && argv[i][0]!='-'){path=argv[i];
        path_set=true;}
        else return command_error(argv[0],"parametros invalidos; use --help");
    }
    ft_walk(path,&w);
    if(w.du)kprintf("%u KiB  %s (%u archivos, %u directorios)\n",(uint32_t)((w.bytes+1023)/1024),path,w.files,w.dirs);
    return w.failed?2:0;
}
int command_filetool(int argc,char **argv) {
    const char *name=argv[0];
    if(command_is(name,"copy")||command_is(name,"cp")||command_is(name,"move"))return ft_copy(argc,argv);
    if(command_is(name,"cat")||command_is(name,"type")||command_is(name,"more"))return ft_display(argc,argv);
    if(command_is(name,"head")||command_is(name,"tail"))return ft_select_lines(argc,argv);
    if(command_is(name,"grep")||command_is(name,"wc"))return ft_grep_wc(argc,argv);
    if(command_is(name,"diff")||command_is(name,"cmp"))return ft_compare(argc,argv);
    if(command_is(name,"ls")||command_is(name,"dir"))return ft_list(argc,argv);
    if(command_is(name,"find")||command_is(name,"tree")||command_is(name,"du"))return ft_traverse(argc,argv);
    if(command_is(name,"sync"))return argc==1&&bk_file_sync()?0:2;
    if(command_is(name,"true"))return 0;
    if(command_is(name,"false"))return 1;
    if(command_is(name,"touch")) {
        bool create=true;
        int first=1;
        if(argc>1&&command_is(argv[1],"-c")){create=false;
        first++;}
        if(first==argc)return command_error(name,"uso: touch [-c] archivo...");
        int status=0;
        for(int i=first;i<argc;i++)if(!bk_file_touch(argv[i],create))status=command_error(name,argv[i]);
        return status;
    }
    if(command_is(name,"rmdir")) {
        if(argc<2)return 2;
        int result=0;
        for(int i=1;i<argc;i++) {bk_file_entry_t e;
            if(!bk_file_stat(argv[i],&e)||e.type!=BK_FILE_NODE_DIRECTORY||!bk_file_remove(argv[i]))result=command_error(name,argv[i]);}
        return result;
    }
    if(command_is(name,"stat")) {
        if(argc<2)return 2;
        int status=0;
        for(int i=1;i<argc;i++){bk_file_entry_t e;
        uint16_t mode=0;
            if(!bk_file_stat(argv[i],&e)){status=command_error(name,argv[i]);
            continue;}
            bk_file_get_mode(argv[i],&mode);
            char octal[5]={(char)('0'+((mode>>9)&7)),(char)('0'+((mode>>6)&7)),(char)('0'+((mode>>3)&7)),(char)('0'+(mode&7)),0};
            kprintf("%s: %s, %u bytes, modo %s, atributos 0x%x\n",argv[i],e.type==BK_FILE_NODE_DIRECTORY?"directorio":"archivo",e.size,octal,e.attributes);
        }return status;
    }
    if(command_is(name,"df")) {
        const char *path=argc==2?argv[1]:"/";
        uint64_t total,free_bytes;
        if(argc>2||!bk_file_space_path(path,&total,&free_bytes))return command_error(name,"no se pudo consultar el volumen");
        kprintf("Ruta  Total KiB  Usado KiB  Libre KiB\n%s  %u  %u  %u\n",path,(uint32_t)(total/1024),(uint32_t)((total-free_bytes)/1024),(uint32_t)(free_bytes/1024));
        return 0;
    }
    if(command_is(name,"which")) {
        if(argc<2)return 2;
        int status=0;
        const char *dirs[]={"/SYSTEM/COMMANDS","/SYSTEM/PROGRAMS"};
        for(int i=1;i<argc;i++) {
            bool found=false;
            char path[BK_PATH_MAX],base[BK_PATH_MAX];
            bk_file_entry_t e;
            static const char *builtins[]={"cd","pwd","clear","echo","history","alias","unalias","set","ver","exit","savecom1","perfmon","benchmark","bench","ifconfig","dns","tcp","httphead","nettest","net","bootlog","root","disks","usbdiag","pci"};
            for(uint32_t k=0;k<sizeof(builtins)/sizeof(builtins[0]);k++)if(command_is(argv[i],builtins[k])) {kprintf("%s: interno del shell\n",argv[i]);
            found=true;
            break;}
            if(found)continue;
            for(uint32_t j=0;j<2;j++) {
                uint32_t len=bk_runtime_strlen(argv[i]);
                if(len+5>=sizeof(base))continue;
                bk_runtime_strncpy(base,argv[i],sizeof(base));
                bk_runtime_strcat(base,".BEX");
                if(command_join(path,sizeof(path),dirs[j],base)&&bk_file_stat(path,&e)&&e.type==BK_FILE_NODE_FILE){kprintf("%s\n",path);
                found=true;
                break;}
            }
            if(!found)status=1;
        }return status;
    }
    if(command_is(name,"chmod") || command_is(name,"attrib")) {
        if(argc<2)return 2;
        bool attr=command_is(name,"attrib"),query=attr?argv[1][0]!='+'&&argv[1][0]!='-':argc==2;
        uint32_t mode=0;
        bool readonly=false;
        if(!query) {
            if(attr){if(!command_is(argv[1],"+r")&&!command_is(argv[1],"-r"))return 2;
            readonly=argv[1][0]=='+';}
            else {for(const char *p=argv[1];*p;p++){if(*p<'0'||*p>'7'||mode>0777)return 2;
            mode=mode*8+(*p-'0');}if(!argv[1][0]||mode>07777)return 2;}
        }
        int first=query?1:2;
        if(first>=argc)return 2;
        int status=0;
        for(int i=first;i<argc;i++) {
            uint16_t current;
            if(!bk_file_get_mode(argv[i],&current)){status=command_error(name,argv[i]);
            continue;}
            if(query) {
                char octal[5]={(char)('0'+((current>>9)&7)),(char)('0'+((current>>6)&7)),(char)('0'+((current>>3)&7)),(char)('0'+(current&7)),0};
                kprintf("%s: modo %s %s\n",argv[i],octal,(current&0222)?"RW":"R");
            }
            else if(!bk_file_set_mode(argv[i],attr?(readonly?(current&~0222U):(current|0200U)):(uint16_t)mode))status=command_error(name,argv[i]);
        }return status;
    }
    if(command_is(name,"nice")) {
        if(argc<2||argc>3)return 2;
        bool ok;
        uint32_t pid=command_number(argv[1],&ok),level=0;
        if(!ok||!pid)return 2;
        if(argc==3){level=command_is(argv[2],"low")?1:command_is(argv[2],"normal")?2:command_is(argv[2],"high")?3:0;
        if(!level)return 2;}
        uint32_t result=bk_proc_priority(pid,level);
        if(!result)return command_error(name,"proceso inexistente o protegido");
        kprintf("Proceso %u: prioridad %s\n",pid,result==1?"low":result==3?"high":"normal");
        return 0;
    }
    if(command_is(name,"mount") || command_is(name,"unmount")) {
        if(argc!=2)return 2;
        bool ok=command_is(name,"mount")?bk_device_attach_volume(argv[1]):bk_device_unmount_volume(argv[1]);
        if(!ok)return command_error(name,"volumen inaccesible, activo u ocupado");
        kprintf("%s: %s\n",name,argv[1]);
        return 0;
    }
    return command_error(name,"comando desconocido");
}
