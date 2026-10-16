#ifndef BK_BROWSER_PREFERENCES_H
#define BK_BROWSER_PREFERENCES_H
#define BK_INTERNET_CONFIG_PATH "/SYSTEM/USER/CONFIG/INTERNET.INI"
#define BK_DEFAULT_BROWSER_PATH "/SYSTEM/PROGRAMS/NETSURF.BEX"
/* Used by the desktop launcher and Internet Properties. No cached preference:
 * a change applies on the next launch. */
static inline void bk_browser_default_path(char *path, uint32_t capacity) {
    void *raw=NULL;uint32_t size=0,pos=0;
    if(!path || !capacity) return;
    bk_runtime_strncpy(path,BK_DEFAULT_BROWSER_PATH,capacity-1);path[capacity-1]=0;
    if(!bk_file_read_all(BK_INTERNET_CONFIG_PATH,&raw,&size) || !raw) return;
    while(pos<size) {
        uint32_t begin=pos;while(pos<size && ((char *)raw)[pos]!='\n') pos++;
        uint32_t end=pos;if(pos<size) pos++;
        if(end-begin>8 && !bk_runtime_strncmp((char *)raw+begin,"browser=",8)) {
            uint32_t n=end-begin-8;while(n && ((char *)raw)[begin+8+n-1]=='\r') n--;
            if(n && n<capacity && ((char *)raw)[begin+8]=='/') {
                bk_runtime_memcpy(path,(char *)raw+begin+8,n);path[n]=0;
            }
        }
    }
    bk_sys_free(raw);
}
#endif
