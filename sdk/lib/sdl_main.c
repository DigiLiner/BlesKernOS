extern int main(int argc, char **argv);
extern void SDL_Quit(void);
extern int bk_proc_launch_arg_copy(char *buffer, unsigned int capacity);

static int sdl_parse_arguments(char *command,char **argv,int capacity){
    char*source=command,*destination=command;int argc=0;
    while(*source&&argc+1<capacity){char quote=0;while(*source==' '||*source=='\t')source++;if(!*source)break;argv[argc++]=destination;while(*source){char value=*source++;if(!quote&&(value==' '||value=='\t'))break;if(!quote&&(value=='\"'||value=='\'')){quote=value;continue;}if(quote&&value==quote){quote=0;continue;}if(value=='\\'&&*source)value=*source++;*destination++=value;}*destination++='\0';}
    argv[argc]=(char*)0;return argc;
}
void bleskernos_program_main(void *unused) {
    char command[2048];
    char *arguments[64];
    int count;
    int result;
    (void)unused;
    command[0]='\0';(void)bk_proc_launch_arg_copy(command,sizeof(command));
    count=sdl_parse_arguments(command,arguments,64);
    if(!count){arguments[0]=(char*)"sdl-app";arguments[1]=(char*)0;count=1;}
    result = main(count, arguments);
    SDL_Quit();
    extern void bk_exit(int status);
    bk_exit(result);
}
