#include "common.h"

static void disks(void) {
    kprintf("DISCO       TIPO      SECTORES    SECTOR  MODO\n");
    for(uint32_t i=0;i<bk_device_block_count();i++) {
        bk_block_info_t d;
        if(!bk_device_block_info(i,&d))continue;
        kprintf("%-11s %-9s %10u %6u  %s\n",d.name,d.type_name,d.sector_count,d.sector_size,d.read_only?"RO":"RW");
    }
}
static void partitions(const char *disk) {
    kprintf("DISCO       PART TIPO               INICIO    SECTORES  ARRANQUE\n");
    for(uint32_t i=0;i<bk_device_partition_count();i++) {
        bk_partition_info_t p;
        if(!bk_device_partition_info(i,&p))continue;
        if(disk && !command_is(disk,p.device_name))continue;
        kprintf("%-11s %u    %-18s %9u %10u  %s\n",p.device_name,p.table_index+1,p.type_name,p.first_sector,p.sector_count,p.bootable?"si":"no");
    }
}
static int run(int argc,char **argv) {
    if(argc==1){command_usage("diskpart");
    disks();
    return 0;}
    if(argc==3 && command_is(argv[1],"list")) {
        if(command_is(argv[2],"disk")){disks();
        return 0;}
        if(command_is(argv[2],"partition")){partitions(NULL);
        return 0;}
        if(command_is(argv[2],"volume")) {
            bk_volume_info_t v;
            if(!bk_device_volume_info(&v))return 2;
            kprintf("Raiz activa: %s, %s, etiqueta %s, %u MiB libres\n",v.device_name,v.filesystem,v.volume_label,(uint32_t)(v.free_bytes/1048576));
            kprintf("Volumenes secundarios accesibles como /NOMBRE; use df /NOMBRE.\n");
            return 0;
        }
    }
    if(argc==3 && command_is(argv[1],"detail")) {
        for(uint32_t i=0;i<bk_device_block_count();i++) {
            bk_block_info_t d;
            if(!bk_device_block_info(i,&d)||!command_is(d.name,argv[2]))continue;
            kprintf("Disco %s: %s, %u sectores de %u bytes, %s\n",d.name,d.type_name,d.sector_count,d.sector_size,d.read_only?"solo lectura":"lectura/escritura");
            partitions(d.name);
            return 0;
        }
        return command_error("diskpart","disco no encontrado");
    }
    bool create=command_is(argv[1],"create"),remove=command_is(argv[1],"delete");
    if(create || remove) {
        if((create && argc!=8) || (remove && argc!=5) || !command_is(argv[argc-1],"/YES"))
            return command_error("diskpart","parametros invalidos o falta /YES; use --help");
        bool valid;
        uint32_t index=command_number(argv[3],&valid),first=0,sectors=0,type=0;
        if(!valid || index<1 || index>4)return command_error("diskpart","particion: 1..4");
        if(create) {
            first=command_number(argv[4],&valid);
            if(!valid)return 2;
            sectors=command_number(argv[5],&valid);
            if(!valid)return 2;
            if(command_is(argv[6],"fat32"))type=0x0c;
            else if(command_is(argv[6],"fat16"))type=0x0e;
            else if(command_is(argv[6],"linux"))type=0x83;
            else return command_error("diskpart","tipo: fat32, fat16 o linux");
        }
        int result=bk_device_partition_edit(argv[2],index,first,sectors,type);
        if(result<0) {
            const char *why=result==-2?"disco protegido, activo u ocupado":result==-3?"solo MBR primario valido o disco vacio; GPT/extendidas/volumen directo no editables":result==-4?"error de E/S al actualizar MBR":result==-5?"sin slots de dispositivos":"rango invalido, solapamiento o slot incorrecto";
            return command_error("diskpart",why);
        }
        kprintf("MBR actualizado y verificado. %s\n",result?"Reinicie para actualizar los dispositivos.":"Vista de particion actualizada.");
        if(create)kprintf("La particion no esta formateada. Use format %sp%u ETIQUETA /YES.\n",argv[2],index);
        return 0;
    }
    if(argc==3 && (command_is(argv[1],"mount")||command_is(argv[1],"unmount"))) {
        bool ok=command_is(argv[1],"mount")?bk_device_attach_volume(argv[2]):bk_device_unmount_volume(argv[2]);
        return ok?0:command_error("diskpart","volumen inaccesible, activo u ocupado");
    }
    return command_error("diskpart","parametros invalidos; use --help");
}
BK_COMMAND_MAIN(run)
