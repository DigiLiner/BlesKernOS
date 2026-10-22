/* SPDX-License-Identifier: MIT
 * User-specific Jelly screensaver preferences (shared by Display CPL and saver).
 * Separate from SCREENSV.INI, so the screensaver daemon never overwrites it.
 */
#ifndef BLESKERNOS_JELLY_SETTINGS_H
#define BLESKERNOS_JELLY_SETTINGS_H

#define BK_JELLY_CONFIG_PATH "/SYSTEM/USER/CONFIG/JELLY.INI"

typedef struct {
    uint32_t count;       /* 1..10 */
    uint32_t variation;   /* 0=uniform, 1=low, 2=medium, 3=high */
} bk_jelly_settings_t;

static inline void bk_jelly_defaults(bk_jelly_settings_t *s) {
    s->count = 4U;
    s->variation = 2U;
}

static inline uint32_t bk_jelly_clamp(uint32_t v,uint32_t low,uint32_t high) {
    return v<low?low:(v>high?high:v);
}

static inline void bk_jelly_load(bk_jelly_settings_t *s) {
    void *file=NULL;
    uint32_t size=0;
    char data[256];
    if (!s) return;
    bk_jelly_defaults(s);
    if (!bk_user_config_read_all(BK_JELLY_CONFIG_PATH,NULL,&file,&size) || !file) return;
    /* read_all does not guarantee a trailing NUL. Avoid scanning past EOF. */
    if (size >= sizeof(data)) size=sizeof(data)-1U;
    bk_runtime_memcpy(data,file,size);
    data[size]='\0';
    bk_sys_free(file);
    char *line=data;
    while (*line) {
        char *end=line;
        while (*end && *end!='\r' && *end!='\n') end++;
        char saved=*end;*end='\0';
        const char *value=NULL;
        uint32_t *dest=NULL, lo=0,hi=0;
        if (bk_runtime_strncmp(line,"count=",6)==0) {
            value=line+6;dest=&s->count;lo=1;hi=10;
        } else if(bk_runtime_strncmp(line,"variation=",10)==0) {
            value=line+10;dest=&s->variation;lo=0;hi=3;
        }
        if (value && dest && *value>='0' && *value<='9') {
            uint32_t n=0;
            while (*value>='0' && *value<='9') {
                /* Cap input before multiplication to avoid uint overflow. */
                if(n>1000U){n=1000U;break;}
                n=n*10U+(uint32_t)(*value-'0');value++;
            }
            if (!*value) *dest=bk_jelly_clamp(n,lo,hi);
        }
        *end=saved;
        line=end;
        while (*line=='\r' || *line=='\n') line++;
    }
}

static inline bool bk_jelly_save(const bk_jelly_settings_t *s) {
    char text[64];
    if(!s)return false;
    snprintf(text,sizeof(text),"count=%u\r\nvariation=%u\r\n",
        bk_jelly_clamp(s->count,1,10),bk_jelly_clamp(s->variation,0,3));
    return bk_user_config_write_text(BK_JELLY_CONFIG_PATH,text);
}
#endif
