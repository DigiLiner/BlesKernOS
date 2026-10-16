/* Test-only virtual printer: validates native IEEE1284 API -> PCL5 pipeline. */
#include "../../kernel/include/driver.h"
#include "../../kernel/include/lpt.h"
#include "../../kernel/include/vfs.h"
#include "../../kernel/include/memory.h"
static uint32_t count(void) { return 1U; }
static bool info(uint32_t index,lpt_port_info_t *out) {
    if(index || !out)return false;
    kmemset(out,0,sizeof(*out));kstrncpy(out->name,"FAKEPRN1",sizeof(out->name)-1U);
    out->present=out->selected=out->acknowledged=out->virtual_port=true;
    return true;
}
static bool identify(uint32_t index,char *out,uint32_t capacity) {
    if(index || capacity<40U)return false;
    kstrncpy(out,"MFG:Test;MDL:Family;CMD:PCL5;",capacity-1U);out[capacity-1U]=0;return true;
}
static int32_t write(uint32_t index,const void *data,uint32_t length,uint32_t timeout) {
    (void)timeout;const uint8_t *p=data;
    if(index || !p || length<2U || p[0]!=0x1bU || p[1]!='E')return -5;
    return vfs_write_all("/TEMP/PRINT/FAMILY.PCL",data,length)?(int32_t)length:-5;
}
static const lpt_virtual_provider_t provider={count,info,write};
static bool init(void) {
    return lpt_register_virtual_provider(&provider)&&lpt_register_identity_provider(&provider,identify);
}
static void stop(void) {lpt_unregister_virtual_provider(&provider);}
const bk_driver_module_t *bleskernos_driver_query(void) {
    static const bk_driver_module_t module={BK_DRIVER_ABI_VERSION,sizeof(bk_driver_module_t),1,0,
        "Test PCL5 family","Test-only IEEE1284 provider",init,stop};
    return &module;
}
