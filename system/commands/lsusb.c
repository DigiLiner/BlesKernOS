#include "common.h"
static int run(int argc,char **argv) {
    bool verbose=argc==2&&command_is(argv[1],"-v");if(argc>1&&!verbose)return 2;
    uint32_t shown=0;
    for(uint32_t i=0;i<bk_device_usb_count();i++) {
        bk_usb_info_t d;if(!bk_device_usb_info(i,&d))continue;shown++;
        kprintf("USB %u addr=%u ID %04x:%04x clase=%02x interfaces=%u\n",d.id,d.address,d.vendor_id,d.product_id,d.device_class,d.interfaces);
        if(verbose)kprintf("  padre=%u puerto=%u velocidad=%u subclase=%02x\n",d.parent_id,d.port,d.speed,d.device_subclass);
    }
    if(!shown)bk_console_write("Sin dispositivos USB enumerados.\n");
    return 0;
}
BK_COMMAND_MAIN(run)
