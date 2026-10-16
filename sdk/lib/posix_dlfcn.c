#include <dlfcn.h>
#include <elf.h>
#include <fcntl.h>
#include <link.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define DL_MAGIC 0x424B444CU
#define DL_MAX_MODULES 8U
#define DL_MAX_FILE (16U*1024U*1024U)
typedef struct {
    unsigned int magic,low,bias,size,refs;
    unsigned char *image;
    char *name;
    Elf32_Phdr *phdr;
    unsigned short phnum;
    Elf32_Sym *symtab;
    const char *strtab;
    unsigned int strsz,symcount;
    void (**fini)(void);
    unsigned int fini_count;
    void *dependencies[8];
    unsigned int dependency_count;
} dl_module_t;
static dl_module_t *module_list[DL_MAX_MODULES];
static char *last_error;
static char e_handle[]="invalid dynamic module handle";
static char e_elf[]="not a supported ELF32 i386 ET_DYN module";
static char e_io[]="cannot read dynamic module";
static char e_mem[]="not enough memory for dynamic module";
static char e_needed[]="DT_NEEDED dependency cannot be loaded";
static char e_reloc[]="unsupported or unresolved ELF relocation";
static char e_symbol[]="symbol not found";
static char e_slots[]="too many dynamic modules";

#define EXPORT(n) if(!strcmp(symbol,#n))return(void*)(n)
static void *static_symbol(const char *symbol){
    if(!symbol)return 0;
    EXPORT(malloc);EXPORT(calloc);EXPORT(realloc);EXPORT(reallocarray);EXPORT(free);
    EXPORT(memcpy);EXPORT(memmove);EXPORT(memset);EXPORT(memcmp);EXPORT(memchr);EXPORT(memmem);
    EXPORT(strlen);EXPORT(strnlen);EXPORT(strcmp);EXPORT(strncmp);EXPORT(strcpy);EXPORT(strncpy);
    EXPORT(strdup);EXPORT(strndup);EXPORT(strchr);EXPORT(strrchr);EXPORT(strstr);
    EXPORT(open);EXPORT(read);EXPORT(write);EXPORT(close);EXPORT(lseek);EXPORT(dup);EXPORT(dup2);
    EXPORT(fopen);EXPORT(fclose);EXPORT(fread);EXPORT(fwrite);EXPORT(fflush);
    EXPORT(printf);EXPORT(fprintf);EXPORT(snprintf);EXPORT(sscanf);
    EXPORT(pthread_create);EXPORT(pthread_join);EXPORT(pthread_detach);EXPORT(pthread_self);
    EXPORT(pthread_mutex_lock);EXPORT(pthread_mutex_unlock);return 0;
}
#undef EXPORT
static int range_ok(unsigned int off,unsigned int n,unsigned int cap){return off<=cap&&n<=cap-off;}
static void *address(dl_module_t*m,unsigned int va,unsigned int n){unsigned int off;if(!m||va<m->low)return 0;off=va-m->low;return range_ok(off,n,m->size)?m->image+off:0;}
static void *module_symbol(dl_module_t*m,const char*name){unsigned int i;if(!m||!m->symtab||!m->strtab||!name)return 0;for(i=1;i<m->symcount;i++){Elf32_Sym*s=&m->symtab[i];if(s->st_name>=m->strsz||s->st_shndx==SHN_UNDEF)continue;if(!strcmp(m->strtab+s->st_name,name))return address(m,s->st_value,s->st_size?s->st_size:1U);}return 0;}
static void *global_symbol(const char*name,dl_module_t*skip){unsigned int i;void*v=static_symbol(name);if(v)return v;for(i=0;i<DL_MAX_MODULES;i++)if(module_list[i]&&module_list[i]!=skip&&(v=module_symbol(module_list[i],name)))return v;return 0;}
static void *dependency_open(const char *parent,const char *name){char path[260];const char*slash;unsigned int n;if(!name||!name[0])return 0;if(strchr(name,'/'))return dlopen(name,RTLD_NOW);slash=parent?strrchr(parent,'/'):0;if(slash){n=(unsigned int)(slash-parent)+1U;if(n+strlen(name)<sizeof(path)){memcpy(path,parent,n);strcpy(path+n,name);{void*h=dlopen(path,RTLD_NOW);if(h)return h;}}}if(strlen(name)+20U<sizeof(path)){strcpy(path,"/SYSTEM/LIBS/POSIX/");strcat(path,name);{void*h=dlopen(path,RTLD_NOW);if(h)return h;}}if(strlen(name)+14U<sizeof(path)){strcpy(path,"/SYSTEM/LIBS/");strcat(path,name);return dlopen(path,RTLD_NOW);}return 0;}
static int relocate(dl_module_t*m,Elf32_Rel*r,unsigned int bytes){unsigned int i,count=bytes/sizeof(*r);if(bytes&&(!r||(unsigned char*)r<m->image||!range_ok((unsigned int)((unsigned char*)r-m->image),bytes,m->size)))return 0;for(i=0;i<count;i++){unsigned int si=ELF32_R_SYM(r[i].r_info),type=ELF32_R_TYPE(r[i].r_info),sv=0;unsigned int*p=(unsigned int*)address(m,r[i].r_offset,4U);Elf32_Sym*s;if(!p)return 0;if(type==R_386_NONE)continue;if(type==R_386_RELATIVE){*p+=m->bias;continue;}if(!m->symtab||si>=m->symcount)return 0;s=&m->symtab[si];if(s->st_shndx!=SHN_UNDEF)sv=m->bias+s->st_value;else if(s->st_name<m->strsz)sv=(unsigned int)global_symbol(m->strtab+s->st_name,m);if(!sv&&ELF32_ST_BIND(s->st_info)!=STB_WEAK)return 0;if(type==R_386_32)*p+=sv;else if(type==R_386_PC32)*p+=sv-(unsigned int)p;else if(type==R_386_GLOB_DAT||type==R_386_JMP_SLOT)*p=sv;else return 0;}return 1;}

void *dlopen(const char*path,int mode){
    struct stat st;unsigned char*file=0;dl_module_t*m=0;Elf32_Ehdr*h;Elf32_Phdr*ph;Elf32_Dyn*dyn=0;Elf32_Rel*rel=0,*jmp=0;void(**init)(void)=0;unsigned int needed[8],needed_count=0,dync=0,lo=~0U,hi=0,relsz=0,jmpsz=0,initc=0,slot,i;int fd;(void)mode;last_error=0;
    if(!path||!path[0])return(void*)1;
    for(slot=0;slot<DL_MAX_MODULES;slot++)if(module_list[slot]&&module_list[slot]->name&&!strcmp(module_list[slot]->name,path)){module_list[slot]->refs++;return module_list[slot];}
    for(slot=0;slot<DL_MAX_MODULES&&module_list[slot];slot++) {}
    if(slot==DL_MAX_MODULES){last_error=e_slots;return 0;}
    fd=open(path,O_RDONLY);if(fd<0||fstat(fd,&st)<0||st.st_size<(int)sizeof(*h)||st.st_size>(int)DL_MAX_FILE){if(fd>=0)close(fd);last_error=e_io;return 0;}
    file=malloc((unsigned int)st.st_size);if(!file){close(fd);last_error=e_mem;return 0;}if(read(fd,file,(unsigned int)st.st_size)!=st.st_size){close(fd);free(file);last_error=e_io;return 0;}close(fd);h=(Elf32_Ehdr*)(void*)file;
    if(memcmp(h->e_ident,"\177ELF",4)||h->e_ident[EI_CLASS]!=ELFCLASS32||h->e_ident[EI_DATA]!=ELFDATA2LSB||h->e_type!=ET_DYN||h->e_machine!=EM_386||h->e_phentsize!=sizeof(*ph)||!range_ok(h->e_phoff,h->e_phnum*sizeof(*ph),(unsigned int)st.st_size)){free(file);last_error=e_elf;return 0;}ph=(Elf32_Phdr*)(void*)(file+h->e_phoff);
    for(i=0;i<h->e_phnum;i++)if(ph[i].p_type==PT_LOAD){unsigned int end=ph[i].p_vaddr+ph[i].p_memsz;if(end<ph[i].p_vaddr||ph[i].p_filesz>ph[i].p_memsz||!range_ok(ph[i].p_offset,ph[i].p_filesz,(unsigned int)st.st_size)){free(file);last_error=e_elf;return 0;}if(ph[i].p_vaddr<lo)lo=ph[i].p_vaddr;if(end>hi)hi=end;}
    lo&=~0xfffU;hi=(hi+0xfffU)&~0xfffU;
    if(lo==~0U||hi<=lo){free(file);last_error=e_elf;return 0;}
    m=calloc(1,sizeof(*m));if(!m||(m->image=calloc(1,hi-lo))==0){free(m);free(file);last_error=e_mem;return 0;}m->magic=DL_MAGIC;m->refs=1U;m->low=lo;m->size=hi-lo;m->bias=(unsigned int)m->image-lo;m->name=strdup(path);m->phnum=h->e_phnum;
    for(i=0;i<h->e_phnum;i++)if(ph[i].p_type==PT_LOAD)memcpy(m->image+ph[i].p_vaddr-lo,file+ph[i].p_offset,ph[i].p_filesz);
    /* La copia de phdr vive con el modulo; el buffer del archivo se libera. */
    m->phdr=malloc(h->e_phnum*sizeof(*ph));if(m->phdr)memcpy(m->phdr,ph,h->e_phnum*sizeof(*ph));
    for(i=0;i<h->e_phnum;i++)if(ph[i].p_type==PT_DYNAMIC){dyn=(Elf32_Dyn*)address(m,ph[i].p_vaddr,ph[i].p_memsz);dync=ph[i].p_memsz/sizeof(*dyn);}
    if(!dyn){last_error=e_elf;goto fail;}
    for(i=0;i<dync&&dyn[i].d_tag!=DT_NULL;i++){unsigned int v=dyn[i].d_un.d_ptr;switch(dyn[i].d_tag){case DT_NEEDED:if(needed_count>=8U){last_error=e_slots;goto fail;}needed[needed_count++]=dyn[i].d_un.d_val;break;case DT_STRTAB:m->strtab=(char*)address(m,v,1);break;case DT_SYMTAB:m->symtab=(Elf32_Sym*)address(m,v,sizeof(Elf32_Sym));break;case DT_STRSZ:m->strsz=dyn[i].d_un.d_val;break;case DT_HASH:{unsigned int*x=(unsigned int*)address(m,v,8);if(x)m->symcount=x[1];break;}case DT_REL:rel=(Elf32_Rel*)address(m,v,sizeof(*rel));break;case DT_RELSZ:relsz=dyn[i].d_un.d_val;break;case DT_JMPREL:jmp=(Elf32_Rel*)address(m,v,sizeof(*jmp));break;case DT_PLTRELSZ:jmpsz=dyn[i].d_un.d_val;break;case DT_INIT_ARRAY:init=(void(**)(void))address(m,v,sizeof(void*));break;case DT_INIT_ARRAYSZ:initc=dyn[i].d_un.d_val/sizeof(void*);break;case DT_FINI_ARRAY:m->fini=(void(**)(void))address(m,v,sizeof(void*));break;case DT_FINI_ARRAYSZ:m->fini_count=dyn[i].d_un.d_val/sizeof(void*);break;default:break;}}
    if(m->strtab&&!m->strsz)m->strsz=m->size-(unsigned int)((const unsigned char*)m->strtab-m->image);
    if(!m->symtab||!m->strtab||!m->symcount||m->symcount>m->size/sizeof(Elf32_Sym)){last_error=e_reloc;goto fail;}module_list[slot]=m;for(i=0;i<needed_count;i++){const char*n;if(needed[i]>=m->strsz){last_error=e_needed;goto fail;}n=m->strtab+needed[i];m->dependencies[m->dependency_count]=dependency_open(path,n);if(!m->dependencies[m->dependency_count]){last_error=e_needed;goto fail;}m->dependency_count++;}if(!relocate(m,rel,relsz)||!relocate(m,jmp,jmpsz)){last_error=e_reloc;goto fail;}free(file);for(i=0;init&&i<initc;i++)if(init[i])init[i]();return m;
fail:if(module_list[slot]==m)module_list[slot]=0;while(m&&m->dependency_count)dlclose(m->dependencies[--m->dependency_count]);if(m){free(m->phdr);free(m->name);free(m->image);free(m);}free(file);return 0;
}
void*dlsym(void*handle,const char*symbol){void*v;last_error=0;if(!symbol){last_error=e_symbol;return 0;}if(handle==(void*)1||handle==RTLD_DEFAULT)v=global_symbol(symbol,0);else if(handle&&((dl_module_t*)handle)->magic==DL_MAGIC)v=module_symbol(handle,symbol);else{last_error=e_handle;return 0;}if(!v)last_error=e_symbol;return v;}
int dlclose(void*handle){dl_module_t*m=handle;unsigned int i;if(handle==(void*)1||handle==RTLD_DEFAULT)return 0;if(!m||m->magic!=DL_MAGIC){last_error=e_handle;return-1;}if(m->refs>1U){m->refs--;return 0;}for(i=m->fini_count;m->fini&&i;i--)if(m->fini[i-1])m->fini[i-1]();for(i=0;i<DL_MAX_MODULES;i++)if(module_list[i]==m)module_list[i]=0;m->magic=0;while(m->dependency_count)dlclose(m->dependencies[--m->dependency_count]);free(m->phdr);free(m->name);free(m->image);free(m);return 0;}
char*dlerror(void){char*r=last_error;last_error=0;return r;}
int dl_iterate_phdr(int(*cb)(struct dl_phdr_info*,unsigned int,void*),void*data){unsigned int i;if(!cb)return 0;for(i=0;i<DL_MAX_MODULES;i++)if(module_list[i]){struct dl_phdr_info x;int r;x.dlpi_addr=module_list[i]->bias;x.dlpi_name=module_list[i]->name;x.dlpi_phdr=module_list[i]->phdr;x.dlpi_phnum=module_list[i]->phnum;r=cb(&x,sizeof(x),data);if(r)return r;}return 0;}
