/* One native help catalog, shared by --help and help COMMAND. */
typedef struct {const char *name,*usage;} command_help_entry_t;
static const command_help_entry_t command_help_entries[]={
    {"help","help [comando] | help -a"},
    {"touch","touch [-c] archivo... (actualiza fechas; -c no crea)"},
    {"copy","copy [-n] [-v] origen destino"},
    {"cp","cp [-n] [-v] origen destino"},
    {"move","move [-n] [-v] origen destino"},
    {"cat","cat [-n] archivo..."},
    {"type","type [-n] archivo..."},
    {"more","more [-n] archivo... (Enter continuar; Q+Enter salir)"},
    {"head","head [-n lineas] archivo"},
    {"tail","tail [-n lineas] archivo"},
    {"wc","wc [-lwc] archivo..."},
    {"grep","grep [-invc] patron archivo... (texto literal; opciones combinables: -in, -v, -c)"},
    {"diff","diff [-q] archivo1 archivo2 (comparacion por linea; 0 iguales, 1 distintos, 2 error)"},
    {"cmp","cmp archivo1 archivo2 (comparacion binaria; primer byte distinto)"},
    {"ls","ls [-a] [-l] [-1] [directorio]"},
    {"dir","dir [-a] [-l] [-1] [directorio]"},
    {"find","find patron [ruta] | find [ruta] [-name texto] [-type f|d] [-i] [-d profundidad]"},
    {"tree","tree [ruta] [-d profundidad] [-type f|d] [-i] [-name texto]"},
    {"du","du [ruta] [-d profundidad] (tamano logico, no bloques asignados)"},
    {"df","df [ruta] (espacio del volumen en KiB)"},
    {"stat","stat ruta..."},
    {"which","which comando... (busca archivos BEX; los builtins tienen prioridad)"},
    {"sync","sync (sincroniza datos pendientes)"},
    {"true","true (devuelve 0)"},
    {"false","false (devuelve 1)"},
    {"rmdir","rmdir directorio... (solo directorios vacios)"},
    {"mkdir","mkdir [-p] directorio..."},
    {"rm","rm [-f] archivo..."},
    {"delete","delete archivo"},
    {"rename","rename origen destino"},
    {"chmod","chmod modo_octal archivo... | chmod archivo (consulta modo)"},
    {"attrib","attrib archivo... | attrib +r|-r archivo... (lectura/escritura mediante modo POSIX)"},
    {"nice","nice PID [low|normal|high] (consulta o cambia prioridad del proceso)"},
    {"mount","mount dispositivo (monta /dispositivo sin cambiar la raiz)"},
    {"unmount","unmount dispositivo (rechaza raiz y volumenes ocupados)"},
    {"diskpart","diskpart list disk|partition|volume\n  diskpart detail DISCO\n  diskpart create DISCO N INICIO_SECTOR CANTIDAD_SECTORES fat32|fat16|linux /YES\n  diskpart delete DISCO N /YES\n  diskpart mount|unmount VOLUMEN\n  N=1..4, inicio>=2048. MBR primario; no GPT ni extendidas.\n  No modifica el disco activo. Crear/borrar modifica la tabla; no mueve datos."},
    {"fdisk","fdisk [dispositivo] (consulta MBR)"},
    {"format","format dispositivo etiqueta /YES (FAT; no permite volumen activo)"},
    {"fsinfo","fsinfo (informacion y comprobacion del volumen activo)"},
    {"checkdisk","checkdisk [ruta] [/fix|-f] (comprueba FAT; /fix modifica el volumen)"},
    {"backup","backup create origen archivo | list archivo | verify archivo | restore archivo destino"},
    {"compress","compress origen destino (RLE BKZ1)"},
    {"extract","extract origen destino (RLE BKZ1)"},
    {"checksum","checksum archivo (FNV1a32, no criptografico)"},
    {"hexedit","hexedit archivo [offset valor_byte]"},
    {"hexdump","hexdump archivo"},
    {"calc","calc numero operador numero (enteros de 32 bits; decimal/0xhex)"},
    {"ps","ps (lista tareas)"},
    {"tasklist","tasklist (lista tareas)"},
    {"kill","kill PID (solicita salida)"},
    {"taskkill","taskkill PID (solicita salida)"},
    {"top","top [-n muestras] [-d intervalo_ms]"},
    {"mem","mem [-b|-k|-m]"},
    {"ping","ping [-n intentos] [-w espera_ms] [-i intervalo_ms] destino"},
    {"ipconfig","ipconfig [/all] [/renew]"},
    {"curl","curl [-I] [-L] [-o archivo] [-t espera_ms] URL"},
    {"wget","wget [-L] [-t espera_ms] URL archivo | wget -o archivo URL"},
    {"netstat","netstat [-a] [-t|-u] (sockets nativos del OS)"},
    {"ftp","ftp [-u usuario] [-p clave] HOST[:puerto] list [ruta] | get remoto local | put local remoto"},
    {"compile","compile fuente.c -o objeto.o [opciones]"},
    {"link","link objeto.o [biblioteca.a...] -o salida.BEX"},
    {"gcc","gcc [opciones] archivo.c -o salida.o|salida.BEX (frontend TinyCC)"},
    {"cc","cc [opciones] archivo.c"},
    {"build","build fuente.c programa.BEX"},
    {"run","run programa [argumentos] (foreground)"},
    {"start","start programa [argumentos] (segundo plano)"},
    {"sh","sh -c \"comando\" | sh --batch archivo (recetas simples, sin pipes)"},
    {"hostname","hostname [-s nombre] (persistente; no modifica DNS ni el hostname DHCP)"},
    {"label","label (consulta etiqueta)"},
    {"uname","uname"},
    {"about","about"},
    {"date","date"},
    {"time","time"},
    {"uptime","uptime"},
    {"sleep","sleep segundos"},
    {"reboot","reboot"},
    {"shutdown","shutdown"},
    {"soundtest","soundtest"},
    {"cpuinfo","cpuinfo"},
    {"pci","pci"},
    {"lspci","lspci"},
    {"usb","usb [-v] (dispositivos USB enumerados)"},
    {"lsusb","lsusb [-v] (dispositivos USB enumerados)"},
    {"bosrecovery","bosrecovery /YES (crea medio fd0)"},
    {"pwd","pwd"},
    {"cd","cd [ruta|~|-]"},
    {"clear","clear"},
    {"echo","echo texto..."},
    {"exit","exit"},
    {"history","history"},
    {"alias","alias [nombre comando | nombre='comando']"},
    {"unalias","unalias nombre"},
    {"set","set [NOMBRE=valor]"},
    {"ver","ver"},
    {"benchmark","benchmark | bench"},
    {"bench","bench"},
    {"perfmon","perfmon [status|on|off|watch|now|reset|interval segundos]"},
    {"bootlog","bootlog [lineas]"},
    {"savecom1","savecom1 [archivo]"},
    {"disks","disks"},
    {"root","root"},
    {"usbdiag","usbdiag"},
    {"dns","dns nombre"},
    {"tcp","tcp destino puerto"},
    {"httphead","httphead URL"},
    {"nettest","nettest [host]"},
    {"net","net [info|dhcp|test [host]|static IP MASCARA GATEWAY DNS]"},
    {"ifconfig","ifconfig (informacion de red)"},
};
static const char *command_external_tools=" ld as ar ranlib nasm ndisasm make fatpack addr2line cxxfilt elfedit objcopy objdump readelf nm size strip strings ";
bool command_usage(const char *name) {
    for(uint32_t i=0;i<sizeof(command_help_entries)/sizeof(command_help_entries[0]);i++)
        if(command_is(name,command_help_entries[i].name)) {kprintf("%s\n",command_help_entries[i].usage);return true;}
    return false;
}
int command_help(int argc,char **argv) {
    if(argc==2 && !command_is(argv[1],"-a")) {
        if(command_usage(argv[1]))return 0;
        char token[80],path[BK_PATH_MAX],line[128];
        int n=snprintf(token,sizeof(token)," %s ",argv[1]);
        if(n>0 && (uint32_t)n<sizeof(token) && ft_match(command_external_tools,token,false)) {
            snprintf(path,sizeof(path),"/SYSTEM/COMMANDS/%s.BEX",argv[1]);
            snprintf(line,sizeof(line),"%s --help",argv[1]);
            return bk_app_run_sync(path,line);
        }
        return command_error("help","comando desconocido");
    }
    if(argc>2)return command_error("help","uso: help [comando] | help -a");
    bk_console_write("BlesKernOS Terminal - comandos nativos\nUse help COMANDO o COMANDO --help.\n");
    for(uint32_t i=0;i<sizeof(command_help_entries)/sizeof(command_help_entries[0]);i++) {
        if(argc==2)kprintf("%s\n",command_help_entries[i].usage);
        else kprintf("%-12s%s",command_help_entries[i].name,(i%5)==4?"\n":" ");
    }
    kprintf("\nHerramientas externas:%s\n",command_external_tools);return 0;
}
