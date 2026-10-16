#!/usr/bin/env python3
"""Arranca BlesK en QEMU con glibc real, usando una copia de disco aislada."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import time

ROOT=Path(__file__).resolve().parents[3]
sys.path.insert(0,str(ROOT/'tools'))
from run_office95_validation import command_for
from kernel_image import KERNEL_MAX_BYTES,copy_boot_image

def run(args):
    subprocess.run([str(a) for a in args],cwd=ROOT,check=True,stdout=subprocess.DEVNULL)

def repack_wineboot_image(source,target,size_mib):
    """Recrea la particion FAT32 de una imagen MBR en un contenedor mayor.

    W3 necesita espacio para los modulos PE y para el WINEPREFIX que crea
    wineboot.  No se redimensiona FAT32 in-place: se construye un filesystem
    nuevo, se copia el arbol visible con mtools y se preservan MBR, codigo de
    boot y la zona reservada de Stage2/kernel.  La imagen es solo un fixture
    desechable del harness; la imagen base nunca se modifica.
    """
    source,target=Path(source),Path(target)
    with source.open('rb') as src:
        mbr=bytearray(src.read(512))
        if mbr[510:512]!=b'\x55\xaa': raise RuntimeError('W3: MBR invalido')
        start,total=struct.unpack_from('<II',mbr,454)
        if not start or not total: raise RuntimeError('W3: particion FAT invalida')
        offset=start*512
        src.seek(offset);old_boot=bytearray(src.read(512))
    if struct.unpack_from('<H',old_boot,11)[0]!=512 or struct.unpack_from('<I',old_boot,36)[0]==0:
        raise RuntimeError('W3: se requiere FAT32/512')
    reserved=struct.unpack_from('<H',old_boot,14)[0]
    if reserved<12: raise RuntimeError('W3: reserva FAT32 inesperada')
    fsinfo_sector=reserved-3
    backup_boot_sector=reserved-2
    backup_fsinfo_sector=reserved-1
    old_mib=total*512/(1024*1024)
    if size_mib<=old_mib:
        raise RuntimeError(f'W3: --wineboot-image-mib debe superar {old_mib:.1f} MiB')
    part_kib=size_mib*1024
    label=bytes(old_boot[71:82]).decode('ascii','ignore').rstrip(' \x00') or 'BLESKERNOS'

    with tempfile.TemporaryDirectory(prefix='bles-w3-repack-') as td:
        td=Path(td);tree=td/'tree';tree.mkdir();part=td/'fat32.img'
        old_fat=f'{source}@@{offset}'
        # Extraer solamente el arbol FAT. La metadata POSIX de W3 se crea
        # despues en el guest; no dependemos de bytes reservados de entradas
        # antiguas para reconstruir esta copia temporal.
        run(['mcopy','-s','-o','-i',old_fat,'::*',tree])
        run(['mkfs.fat','-C','-F','32','-n',label[:11],'-R',str(reserved),
             '-b',str(backup_boot_sector),part,str(part_kib)])

        with part.open('r+b') as fp:
            # mkfs coloca inicialmente FSInfo en sector 1; BlesK reserva
            # 1.. para Stage2/kernel y traslada FSInfo a los tres ultimos
            # sectores reservados, igual que tools/build_fat32_ata.py.
            fp.seek(512);fsinfo=fp.read(512)
            for sector in (fsinfo_sector,backup_fsinfo_sector):
                fp.seek(sector*512);fp.write(fsinfo)
            for sector in (0,backup_boot_sector):
                fp.seek(sector*512);boot=bytearray(fp.read(512))
                struct.pack_into('<H',boot,48,fsinfo_sector)
                struct.pack_into('<H',boot,50,backup_boot_sector)
                struct.pack_into('<I',boot,28,start)
                boot[0:3]=old_boot[0:3]
                boot[90:512]=old_boot[90:512]
                fp.seek(sector*512);fp.write(boot)

            # Preservar Stage2 + kernel crudo del fixture fuente. El runner
            # vuelve a reemplazarlos enseguida con sus binarios de test.
            with source.open('rb') as src:
                src.seek(offset+512)
                raw=src.read((fsinfo_sector-1)*512)
            fp.seek(512);fp.write(raw)

        new_fat=str(part)
        for child in sorted(tree.iterdir(),key=lambda x:x.name.lower()):
            run(['mcopy','-s','-o','-i',new_fat,child,'::/'])

        new_total=part.stat().st_size//512
        struct.pack_into('<I',mbr,458,new_total)
        # La geometria CHS no participa del boot LBA; marcar el final como
        # maximo evita dejar la geometria corta de la imagen de 96 MiB.
        mbr[451:454]=b'\xfe\xff\xff'
        with target.open('wb') as dst:
            dst.write(mbr)
            if start>1: dst.write(bytes((start-1)*512))
            with part.open('rb') as fp: shutil.copyfileobj(fp,dst,1024*1024)

WORKER=r'''
#include "include/pe_loader.h"
#include "include/syscall.h"
#include "include/block.h"
#include "include/fat.h"
#include "include/memory.h"
#include "include/perfmon.h"
#include "include/w3_profile.h"
static void linux_validation_worker(void *unused UNUSED) {
    syscall_set_linux_trace(TEST_TRACE);
    syscall_set_linux_trace_process(TRACE_FILTER_LITERAL);
    syscall_set_linux_trace_module(TRACE_MODULE_LITERAL);
    const char *paths[]={TEST_PATHS};
    for (unsigned i=0;i<sizeof(paths)/sizeof(paths[0]);++i) {
        BENCH_START
        W3P_BEGIN
        int status=-999, result=-1;
        kprintf("[GATE:A] START %s\n",paths[i]);
        int pid=elf_spawn_program_ex(paths[i],gui_get_desktop(),NULL);
        uint32_t start=pit_get_ticks();
        if (pid>=0) {
            while ((result=task_waitpid((uint32_t)pid,&status))==0 &&
                   pit_get_ticks()-start < TEST_TIMEOUT*pit_get_frequency_hz()) task_yield();
            if (result==0) { task_debug_dump_all("gate-timeout"); task_request_exit_process((uint32_t)pid,-998); status=-998; }
        } else kprintf("[GATE:A] LOAD_ERROR %s\n",elf_last_error());
        W3P_END
        kprintf("[GATE:A] RESULT %s status=%d\n",paths[i],status);
        BENCH_END
    }
    kprintf("[GATE:A] END\n");
}
'''

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--image',type=Path,default=ROOT/'build/bleskernos-usb-developer.img')
    p.add_argument('--rootfs',type=Path,default=ROOT/'ports/wine/rootfs')
    p.add_argument('--output',type=Path,required=True)
    p.add_argument('--cases',default=','.join(str(n) for n in range(1,31)))
    p.add_argument('--cpus',type=int,choices=[1,2,4],default=1)
    p.add_argument('--accel',choices=['tcg','kvm'],default='tcg',help='acelerador QEMU; conservar TCG para regresiones emuladas')
    p.add_argument('--cpu',choices=['pentium3','host'],default='pentium3',
                   help='modelo de CPU QEMU; host requiere KVM')
    p.add_argument('--ram-mib',type=int,default=256,help='RAM de QEMU para el gate (default: 256 MiB)')
    p.add_argument('--seconds',type=int,default=360)
    p.add_argument('--case-seconds',type=int,default=15)
    p.add_argument('--trace',type=int,choices=[0,1,2],default=0)
    p.add_argument('--profile-syscalls',action='store_true',help='registra syscalls que tardan al menos 10 ticks')
    p.add_argument('--profile-w3',action='store_true',help='contadores agregados Wine/VFS/FAT/block; sin trazas por syscall')
    p.add_argument('--storage-bench',action='store_true',help='mide lectura cruda, FAT y CPU sin wineboot')
    p.add_argument('--trace-proc',default='',help='filtra [LINUX:SYSCALL] por nombre exacto de tarea Linux')
    p.add_argument('--trace-module',default='',help='arma trace=2 cuando mmap carga este modulo/path y sigue su PID, ej. services.exe')
    p.add_argument('--legacy',action='store_true')
    p.add_argument('--program',action='append',default=[])
    p.add_argument('--wineboot',action='store_true',help='W3: stagea el runtime PE/Unix minimo y ejecuta wineboot --init')
    p.add_argument('--wineboot-full-runtime',action='store_true',help='incluye todas las DLL y datos empaquetados para instalar wine.inf completo')
    p.add_argument('--wineboot-trace-modules',action='store_true',help='diagnostica la carga de DLL durante wineboot')
    p.add_argument('--wine-ntdll',type=Path,help='ntdll.so i386 reconstruido para probar antes de actualizar el paquete')
    p.add_argument('--wineboot-image-mib',type=int,default=256,help='tamano de la particion FAT32 temporal usada por W3 (default: 256 MiB)')
    p.add_argument('--ahci-image',type=Path,help='copia desechable de un segundo disco SATA AHCI')
    p.add_argument('--boot-ahci',action='store_true',help='arranca el disco principal por SATA AHCI')
    p.add_argument('--drive-cache',choices=['default','none','writethrough'],default='default',
                   help='cache del disco QEMU para ensayos de latencia y persistencia')
    p.add_argument('--ahci-reuse',action='store_true',help='verifica los bytes escritos antes de reiniciar')
    p.add_argument('--expected-text',action='append',default=[])
    p.add_argument('--live-marker',default='',help='marca que confirma que el ultimo programa alcanzo estado servidor')
    p.add_argument('--live-seconds',type=int,default=10,help='segundos que el ultimo programa debe seguir vivo tras live-marker')
    args=p.parse_args();out=args.output.resolve();base=args.image.resolve()
    if args.ahci_reuse and not args.ahci_image:p.error('--ahci-reuse requiere --ahci-image')
    if args.boot_ahci and args.ahci_image:p.error('usar --boot-ahci o --ahci-image, no ambos')
    if args.ahci_image and not args.ahci_image.is_file():p.error('falta imagen AHCI')
    if args.wineboot_full_runtime and (not args.wineboot or args.wineboot_image_mib<512):
        p.error('--wineboot-full-runtime requiere --wineboot y --wineboot-image-mib >= 512')
    if args.wineboot_trace_modules and not args.wineboot:p.error('--wineboot-trace-modules requiere --wineboot')
    if args.profile_w3 and (not args.wineboot or args.cpus != 1 or args.profile_syscalls):
        p.error('--profile-w3 requiere --wineboot, --cpus 1 y sin --profile-syscalls')
    if args.cpu=='host' and args.accel!='kvm':p.error('--cpu host requiere --accel kvm')
    if args.storage_bench and (args.wineboot or args.ahci_image):
        p.error('--storage-bench mide el disco principal sin Wine ni disco secundario')
    if args.wine_ntdll and (not args.wineboot or not args.wine_ntdll.is_file()):
        p.error('--wine-ntdll requiere --wineboot y un archivo existente')
    if args.ram_mib < 64 or args.ram_mib > 1536:p.error('--ram-mib valido: 64..1536')
    if not base.is_file():p.error('falta imagen base')
    if out.exists() and any(out.iterdir()):p.error('usar directorio de salida nuevo')
    cases=[int(n) for n in args.cases.split(',')]
    if not cases or any(n<1 or n>30 for n in cases):p.error('casos validos: 1..30')
    out.mkdir(parents=True,exist_ok=True)
    run(['make','-j2','build/kernel.bin','build/system/libs/posix/ld-blesk.so'])
    paths=['/WINEGATE/bin/abi']+[f'/WINEGATE/bin/case{n:02d}' for n in cases]
    paths+=args.program
    if args.storage_bench:paths.append('/WINEGATE/bin/storage-bench')
    if args.ahci_image:paths.append('/WINEGATE/bin/ahci-test')
    if args.wineboot: paths.append('/WINEGATE/bin/wineboot-launch')
    if args.legacy:paths+=['/SYSTEM/TESTS/NATIVE/LINUXPIE.ELF','/SYSTEM/TESTS/NATIVE/INTERPPIE.ELF','/SYSTEM/TESTS/NATIVE/GLIBCHEL.ELF']
    source=(ROOT/'kernel/core/kernel.c').read_text()
    if source.count('    gui_run();')!=1:raise RuntimeError('actualizar punto de entrada del harness')
    bench_start=''
    bench_end=''
    if args.storage_bench:
        bench_start=r'''
        if (i + 1U == sizeof(paths)/sizeof(paths[0])) {
            block_device_t *disk = block_get(fat_get_active_name());
            uint8_t *buffer = (uint8_t *)kmalloc(65536U);
            uint32_t start = pit_get_ticks(), sectors = 0U;
            bool good = disk && buffer;
            perfmon_set_enabled(true);
            for (uint32_t lba = 0U; good && lba < 8192U; lba += 128U) {
                good = block_read(disk, lba, 128U, buffer);
                sectors += 128U;
            }
            uint32_t hz = pit_get_frequency_hz();
            if (!hz) hz = 1U;
            kprintf("[BENCH:RAW] sectors=%u ms=%u ok=%u\n", sectors,
                    (pit_get_ticks()-start)*1000U/hz, good?1U:0U);
            if (buffer) kfree(buffer);
            perfmon_force_report(); perfmon_poll();
        }
        '''
        bench_end=r'''
        if (i + 1U == sizeof(paths)/sizeof(paths[0])) {
            perfmon_force_report(); perfmon_poll();
            perfmon_set_enabled(false);
        }
        '''
        args.expected_text += ['[BENCH:RAW] sectors=8192', '[BENCH:PASS] all data verified']
    worker=(WORKER.replace('BENCH_START',bench_start).replace('BENCH_END',bench_end)
        .replace('W3P_BEGIN', 'if (i + 1U == sizeof(paths)/sizeof(paths[0])) w3p_reset();' if args.profile_w3 else '')
        .replace('W3P_END', 'if (i + 1U == sizeof(paths)/sizeof(paths[0])) w3p_dump();' if args.profile_w3 else '')
        .replace('TRACE_FILTER_LITERAL',json.dumps(args.trace_proc) if args.trace_proc else 'NULL')
        .replace('TRACE_MODULE_LITERAL',json.dumps(args.trace_module) if args.trace_module else 'NULL')
        .replace('TEST_TIMEOUT',str(args.case_seconds)+'U').replace('TEST_TRACE',str(args.trace))
        .replace('TEST_PATHS',','.join(json.dumps(s) for s in paths)))
    source=source.replace('void kernel_main(void) {',worker+'\nvoid kernel_main(void) {')
    source=source.replace('    gui_run();','    task_create_kernel("linux-gate",linux_validation_worker,NULL);\n    gui_run();')
    c=out/'kernel-test.c';o=out/'kernel-test.o';elf=out/'kernel-test.elf';binary=out/'kernel-test.bin'
    c.write_text(source)
    kernel_compile=command_for('kernel/core/kernel.c','build/kernel/core/kernel.o',
        {'kernel/core/kernel.c':c,'build/kernel/core/kernel.o':o})
    if args.profile_w3: kernel_compile.insert(1,'-DW3_PROFILE=1')
    run(kernel_compile)
    syscall=out/'syscall.o'
    command=command_for('kernel/api/syscall.c','build/kernel/api/syscall.o',{'build/kernel/api/syscall.o':syscall})
    command.insert(1,'-DBLESK_LINUX_TRACE=0')
    if args.profile_syscalls: command.insert(1,'-DBLESK_LINUX_PROFILE=1')
    if args.profile_w3: command.insert(1,'-DW3_PROFILE=1')
    run(command)
    linked={'build/kernel/core/kernel.o':o,'build/kernel/api/syscall.o':syscall,'build/kernel.elf':elf}
    if args.profile_w3:
        for source_path in ('kernel/drivers/core/vfs.c',
                            'kernel/drivers/core/block.c',
                            'kernel/drivers/storage/fat.c',
                            'kernel/drivers/storage/ahci.c'):
            object_path='build/'+source_path[:-2]+'.o'
            profiled=out/(Path(source_path).stem+'-w3-profile.o')
            compile_command=command_for(source_path,object_path,
                {object_path:profiled})
            compile_command.insert(1,'-DW3_PROFILE=1')
            run(compile_command)
            linked[object_path]=profiled
        profiler=out/'w3-profile.o'
        compile_command=command_for('kernel/api/syscall.c','build/kernel/api/syscall.o',
            {'kernel/api/syscall.c':ROOT/'ports/wine/tests/w3-profile.c',
             'build/kernel/api/syscall.o':profiler})
        compile_command.insert(1,'-DW3_PROFILE=1')
        run(compile_command)
    if args.profile_syscalls:
        fat_object=out/'fat-profile.o'
        command=command_for('kernel/drivers/storage/fat.c','build/kernel/drivers/storage/fat.o',{'build/kernel/drivers/storage/fat.o':fat_object})
        command.insert(1,'-DBLESK_FAT_PROFILE=1')
        run(command)
        linked['build/kernel/drivers/storage/fat.o']=fat_object
    link_command=command_for('build/kernel/core/kernel.o','build/kernel.elf',linked)
    if args.profile_w3:
        link_command.insert(link_command.index('--whole-archive'),profiler)
    run(link_command)
    run(['objcopy','-O','binary',elf,binary]);kernel=binary.read_bytes()
    if len(kernel)>KERNEL_MAX_BYTES:raise RuntimeError('kernel supera reserva fisica')
    stage=out/'stage2.bin'
    run(['nasm','-f','bin',f'-DKERNEL_SECTORS={(len(kernel)+511)//512}',f'-DKERNEL_CHECKSUM={sum(kernel)&0xffffffff}','boot/stage2.asm','-o',stage])
    image=out/'test.img';copy_boot_image(base,image)
    if args.wineboot:
        expanded=out/'test-w3-expanded.img'
        repack_wineboot_image(image,expanded,args.wineboot_image_mib)
        image.unlink();expanded.replace(image)
    with image.open('r+b') as disk:
        offset=struct.unpack_from('<I',disk.read(512),454)[0]*512
        disk.seek(offset);bpb=disk.read(512)
        if struct.unpack_from('<H',bpb,11)[0]!=512 or struct.unpack_from('<H',bpb,14)[0]*512<9*512+len(kernel):raise RuntimeError('geometria inesperada')
        disk.seek(offset+512);disk.write(stage.read_bytes());disk.seek(offset+9*512);disk.write(kernel)
    fat=str(image)+'@@'+str(offset)

    # Minimal FAT fixtures may omit the normal user's config and /LIB tree.
    # Stage only the directories required by this disposable gate image.
    for guest_dir in ('::/SYSTEM','::/SYSTEM/USER',
                      '::/SYSTEM/USER/CONFIG','::/LIB'):
        exists=subprocess.run(['mdir','-i',fat,guest_dir],cwd=ROOT,
            stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL).returncode==0
        if not exists:run(['mmd','-i',fat,guest_dir])

    # The desktop starts its animated screensaver after five idle minutes.
    # Headless Wine gates have no input and must not spend QEMU CPU on it.
    # Change only this disposable image; keep the source image untouched.
    if args.wineboot:
        saver_config=out/'screensaver-gate.ini'
        saver_config.write_text('enabled=0\ntimeout=300\npath=/SYSTEM/SCREENS/SSLOGO.SCV\n')
        run(['mcopy','-o','-i',fat,saver_config,
             '::/SYSTEM/USER/CONFIG/SCREENSV.INI'])

    # A normal Linux host already provides /tmp before Wine starts.  Stage the
    # directory in the disposable W3 fixture instead of forcing the guest FAT
    # implementation to grow the root directory from inside wineboot-launch.
    # The launcher still applies the Linux-visible 01777 mode in-guest, so the
    # POSIX mode semantics remain part of the gate.  Keep the WINEPREFIX absent:
    # upstream wineboot must still create /WINEGATE/prefix-w3 itself.
    if args.wineboot:
        tmp_exists=subprocess.run(['mdir','-i',fat,'::/TMP'],cwd=ROOT,
            stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL).returncode==0
        if not tmp_exists:run(['mmd','-i',fat,'::/TMP'])

    run(['mcopy','-s','-o','-i',fat,args.rootfs.resolve()/'WINEGATE','::/'])
    if args.storage_bench:
        bench=out/'storage-bench'
        run(['gcc','-m32','-O2','-no-pie',
            '-Wl,--dynamic-linker=/WINEGATE/lib/ld-linux.so.2',
            '-Wl,-rpath,/WINEGATE/lib',
            ROOT/'ports/wine/tests/storage-bench.c','-o',bench])
        run(['mcopy','-o','-i',fat,bench,'::/WINEGATE/BIN/STORAGE-BENCH'])
    if args.ahci_image:
        ahci_test=out/'ahci-test'
        command=['gcc','-m32','-O2','-no-pie',
            '-Wl,--dynamic-linker=/WINEGATE/lib/ld-linux.so.2',
            '-Wl,-rpath,/WINEGATE/lib']
        if args.ahci_reuse:command.append('-DAHCI_REUSE')
        run([*command,ROOT/'ports/wine/tests/ahci-storage.c','-o',ahci_test])
        run(['mcopy','-o','-i',fat,ahci_test,'::/WINEGATE/BIN/AHCI-TEST'])

    # W3 starts where W2 ended: execute Wine's real wineboot PE program.
    # Compile the tiny Linux-side launcher on demand so the source tree, not a
    # prebuilt opaque binary, defines the gate.  The launcher intentionally
    # leaves /WINEGATE/prefix-w3 absent; wineboot must create it itself.
    if args.wineboot:
        registry_gate=out/'wine-reg-gate.exe'
        run(['i686-w64-mingw32-gcc','-Os','-s',
             ROOT/'ports/wine/tests/wine-reg-gate.c',
             '-o',registry_gate,'-ladvapi32'])
        run(['mcopy','-o','-i',fat,registry_gate,
             '::/WINEGATE/BIN/WINE-REG-GATE.EXE'])
        wineboot_launcher=out/'wineboot-launch'
        run(['gcc','-m32','-O2','-no-pie',
             *(['-DWINEBOOT_TRACE_MODULES'] if args.wineboot_trace_modules else []),
             *(['-DWINEBOOT_PROFILE_PHASES'] if args.profile_w3 else []),
             '-Wl,--dynamic-linker=/WINEGATE/lib/ld-linux.so.2',
             '-Wl,-rpath,/WINEGATE/lib',
             ROOT/'ports/wine/tests/wineboot-launch.c','-o',wineboot_launcher])
        run(['mcopy','-o','-i',fat,wineboot_launcher,'::/WINEGATE/BIN/WINEBOOT-LAUNCH'])

    # Wine upstream was installed with --prefix=/opt/wine, and wineserver
    # intentionally resolves its NLS database from that prefix.  Stage the
    # packaged NLS data into the guest rather than patching Wine or faking a
    # special BlesK path.  Keep W2 small: the full Wine tree is ~370 MiB,
    # while NLS is only ~10 MiB and is the data wineserver currently needs.
    wine_pkg=ROOT/'ports/wine/package/wine-11.0-i386/rootfs/opt/wine'
    wine_nls=wine_pkg/'share/wine/nls'
    required_nls=wine_nls/'l_intl.nls'
    wine_inf=wine_pkg/'share/wine/wine.inf'
    if not required_nls.is_file():
        raise RuntimeError(f'falta Wine NLS empaquetado: {required_nls}')
    if not wine_inf.is_file():
        raise RuntimeError(f'falta Wine INF empaquetado: {wine_inf}')
    for guest_dir in ('::/OPT','::/OPT/WINE','::/OPT/WINE/SHARE','::/OPT/WINE/SHARE/WINE'):
        exists=subprocess.run(['mdir','-i',fat,guest_dir],cwd=ROOT,
            stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL).returncode==0
        if not exists:run(['mmd','-i',fat,guest_dir])
    run(['mcopy','-s','-o','-i',fat,wine_nls,'::/OPT/WINE/SHARE/WINE/'])
    # wineboot --init consumes the upstream setup database through the Z:\ drive.
    # NLS alone is not enough: without wine.inf Wine cannot complete prefix setup.
    run(['mcopy','-o','-i',fat,wine_inf,'::/OPT/WINE/SHARE/WINE/WINE.INF'])
    staged_wine={
        str(Path('/opt/wine/share/wine/nls')/f.name):hashlib.sha256(f.read_bytes()).hexdigest()
        for f in sorted(wine_nls.iterdir()) if f.is_file()
    }
    staged_wine['/opt/wine/share/wine/wine.inf']=hashlib.sha256(wine_inf.read_bytes()).hexdigest()

    if args.wineboot:
        # W3 uses a disposable enlarged FAT32 fixture because wineboot must
        # have room both for upstream PE modules and for the prefix it creates.
        # This is not a fake Wine runtime: every staged file comes untouched
        # from the pinned Wine 11.0 installation.  The list is the direct
        # wineboot/service bootstrap set; missing later modules are expected to
        # become the next observable W3 dependency rather than being guessed.
        core_modules=[
            'wineboot.exe','services.exe','rpcss.exe','start.exe','rundll32.exe',
            # services.exe calls SetupDiGetClassDevsW() during its autostart
            # phase. setupapi.dll has a direct runtime import on cfgmgr32.dll;
            # without it services never reaches SVCCTL_STARTED_EVENT on a new
            # prefix, leaving wineboot blocked in start_services_process().
            'advapi32.dll','cfgmgr32.dll','cryptbase.dll','gdi32.dll','imm32.dll','kernel32.dll','kernelbase.dll',
            'newdev.dll','ntdll.dll','rpcrt4.dll','setupapi.dll',
            'shell32.dll','shcore.dll','shlwapi.dll','ucrtbase.dll','msvcrt.dll','user32.dll',
            'userenv.dll','version.dll','wininet.dll','ws2_32.dll',
            'sechost.dll','combase.dll','ole32.dll','oleaut32.dll','win32u.dll',
        ]
        pe_only_modules=['apisetschema.dll']
        special_unix=['ntdll.so','win32u.so','mountmgr.so','ws2_32.so','wine','wine-preloader']
        # win32u.so is loaded lazily by ntdll through MemoryWineUnixFuncs.
        # Unlike the small PE-side win32u.dll.so, the Unix implementation has
        # a direct DT_NEEDED on libm.so.6.  The minimal WINEGATE rootfs only
        # carries libc/ld-linux, so without libm dlopen(win32u.so) fails before
        # win32u can register its USER/GDI syscall table.
        runtime_elf_libs=['libm.so.6']
        for guest_dir in (
            '::/OPT/WINE/BIN','::/OPT/WINE/LIB','::/OPT/WINE/LIB/WINE',
            '::/OPT/WINE/LIB/WINE/I386-WINDOWS','::/OPT/WINE/LIB/WINE/I386-UNIX'):
            exists=subprocess.run(['mdir','-i',fat,guest_dir],cwd=ROOT,
                stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL).returncode==0
            if not exists:run(['mmd','-i',fat,guest_dir])

        for guest_name,source_name in (('WINE','wine'),('WINEBOOT','wine'),('WINESERVER','wineserver')):
            source=wine_pkg/'bin'/source_name
            if not source.is_file(): raise RuntimeError(f'falta Wine W3: {source}')
            run(['mcopy','-o','-i',fat,source,f'::/OPT/WINE/BIN/{guest_name}'])
            staged_wine[f'/opt/wine/bin/{guest_name.lower()}']=hashlib.sha256(source.read_bytes()).hexdigest()

        windows_dir=wine_pkg/'lib/wine/i386-windows'
        unix_dir=wine_pkg/'lib/wine/i386-unix'
        for name in core_modules:
            pe=windows_dir/name
            unix=unix_dir/(name+'.so')
            if not pe.is_file(): raise RuntimeError(f'falta Wine PE W3: {pe}')
            if not unix.is_file(): raise RuntimeError(f'falta Wine Unix W3: {unix}')
            run(['mcopy','-o','-i',fat,pe,f'::/OPT/WINE/LIB/WINE/I386-WINDOWS/{name.upper()}'])
            run(['mcopy','-o','-i',fat,unix,f'::/OPT/WINE/LIB/WINE/I386-UNIX/{(name+".so").upper()}'])
            staged_wine[f'/opt/wine/lib/wine/i386-windows/{name}']=hashlib.sha256(pe.read_bytes()).hexdigest()
            staged_wine[f'/opt/wine/lib/wine/i386-unix/{name}.so']=hashlib.sha256(unix.read_bytes()).hexdigest()
        for name in pe_only_modules:
            pe=windows_dir/name
            if not pe.is_file(): raise RuntimeError(f'falta Wine PE-only W3: {pe}')
            run(['mcopy','-o','-i',fat,pe,f'::/OPT/WINE/LIB/WINE/I386-WINDOWS/{name.upper()}'])
            staged_wine[f'/opt/wine/lib/wine/i386-windows/{name}']=hashlib.sha256(pe.read_bytes()).hexdigest()
        for name in special_unix:
            source=unix_dir/name
            if not source.is_file(): raise RuntimeError(f'falta Wine Unix W3: {source}')
            run(['mcopy','-o','-i',fat,source,f'::/OPT/WINE/LIB/WINE/I386-UNIX/{name.upper()}'])
            staged_wine[f'/opt/wine/lib/wine/i386-unix/{name}']=hashlib.sha256(source.read_bytes()).hexdigest()
        runtime_lib_dir=ROOT/'ports/wine/package/wine-11.0-i386/rootfs/WINEGATE/lib'
        for name in runtime_elf_libs:
            source=runtime_lib_dir/name
            if not source.is_file(): raise RuntimeError(f'falta Wine runtime ELF W3: {source}')
            run(['mcopy','-o','-i',fat,source,f'::/WINEGATE/LIB/{name.upper()}'])
            staged_wine[f'/WINEGATE/lib/{name}']=hashlib.sha256(source.read_bytes()).hexdigest()

    if args.wineboot_full_runtime:
        # PreInstall needs winedevice/mountmgr; DefaultInstall registers and
        # copies many more builtins, drivers, INF files and other data. Use
        # the complete pinned package for the final prefix validation.
        for relative in ('lib/wine','share/wine'):
            source=wine_pkg/relative
            destination='::/OPT/WINE/'+str(Path(relative).parent).upper()+'/'
            run(['mcopy','-s','-o','-i',fat,source,destination])
            for f in sorted(source.rglob('*')):
                if f.is_file():
                    guest=Path('/opt/wine')/f.relative_to(wine_pkg)
                    staged_wine[str(guest)]=hashlib.sha256(f.read_bytes()).hexdigest()
        for source in sorted(runtime_lib_dir.glob('lib*.so*')):
            if source.name == 'libc.so.6' or not source.is_file(): continue
            run(['mcopy','-o','-i',fat,source,f'::/WINEGATE/LIB/{source.name}'])
            staged_wine[f'/WINEGATE/lib/{source.name}']=hashlib.sha256(source.read_bytes()).hexdigest()

    if args.wine_ntdll:
        replacement=args.wine_ntdll.resolve()
        if replacement.read_bytes()[:5]!=b'\x7fELF\x01':
            raise RuntimeError('--wine-ntdll debe ser ELF i386')
        run(['mcopy','-o','-i',fat,replacement,'::/OPT/WINE/LIB/WINE/I386-UNIX/NTDLL.SO'])
        staged_wine['/opt/wine/lib/wine/i386-unix/ntdll.so']=hashlib.sha256(replacement.read_bytes()).hexdigest()
    (out/'staged-wine-data.json').write_text(json.dumps(staged_wine,indent=2)+'\n')

    run(['mcopy','-o','-i',fat,ROOT/'build/system/libs/posix/ld-blesk.so','::/LIB/LD-LINUX.SO.2'])
    manifest=args.rootfs.resolve()/'runtime-manifest.json'
    shutil.copyfile(manifest,out/'runtime-manifest.json')
    (out/'staged-files.json').write_text(json.dumps({str(f.relative_to(args.rootfs.resolve())):hashlib.sha256(f.read_bytes()).hexdigest()
        for f in sorted((args.rootfs.resolve()/'WINEGATE').rglob('*')) if f.is_file()},indent=2)+'\n')
    (out/'sha256.json').write_text(json.dumps({str(f):hashlib.sha256(f.read_bytes()).hexdigest() for f in [base,binary,image]},indent=2)+'\n')
    monitor_socket=Path('/tmp')/f'blesk-qemu-{os.getpid()}.sock'
    command=['qemu-system-i386','-accel',args.accel,'-cpu',args.cpu,'-m',str(args.ram_mib),'-smp',str(args.cpus),
        '-vga','std','-display','none','-serial',f'file:{out / "serial.log"}',
        '-monitor',f'unix:{monitor_socket},server,nowait',
        '-nic','none','-no-reboot','-no-shutdown']
    cache_opt=',cache='+args.drive_cache if args.drive_cache!='default' else ''
    if args.boot_ahci:
        command += ['-drive',f'file={image},format=raw,if=none,id=boot0{cache_opt}',
            '-device','ich9-ahci,id=boot-ahci',
            '-device','ide-hd,drive=boot0,bus=boot-ahci.0']
    else:
        command += ['-drive',f'file={image},format=raw,if=ide{cache_opt}']
    if args.ahci_image:
        ahci_image=out/'ahci.img'
        shutil.copyfile(args.ahci_image.resolve(),ahci_image)
        command += ['-drive',f'file={ahci_image},format=raw,if=none,id=sata0',
            '-device','ich9-ahci,id=ahci0',
            '-device','ide-hd,drive=sata0,bus=ahci0.0']
    (out/'command.json').write_text(json.dumps(command,indent=2)+'\n')
    serial='';start=time.monotonic();live_since=None;live_stable=False
    with (out/'qemu.log').open('w') as log:
        process=subprocess.Popen(command,cwd=ROOT,stdout=log,stderr=log)
        try:
            while process.poll() is None and time.monotonic()-start<args.seconds:
                time.sleep(1)
                serial=(out/'serial.log').read_text(errors='replace') if (out/'serial.log').exists() else ''
                if '[GATE:A] END' in serial:break
                # A foreground server is successful precisely when it does not exit.
                # For W2, let the caller name an upstream readiness marker and require
                # the final test process to remain alive for a stability window after it.
                # This is a harness rule only; Wine and the guest kernel are untouched.
                if args.live_marker and args.live_marker in serial and f'[GATE:A] START {paths[-1]}' in serial:
                    last_result=re.search(r'\[GATE:A\] RESULT '+re.escape(paths[-1])+r' status=(-?\d+)',serial)
                    fatal=any(marker in serial for marker in
                              ('[LINUX:FAULT]','[PANIC]','boot event wait timed out'))
                    if last_result or fatal:
                        live_since=None
                    elif live_since is None:
                        live_since=time.monotonic()
                    elif time.monotonic()-live_since>=args.live_seconds:
                        live_stable=True
                        break
        finally:
            process.terminate()
            try:process.wait(timeout=5)
            except subprocess.TimeoutExpired:process.kill();process.wait()
            monitor_socket.unlink(missing_ok=True)
    observations=[(path,int(status)) for path,status in re.findall(r'\[GATE:A\] RESULT (\S+) status=(-?\d+)',serial)]
    results=dict(observations)
    faults=[line for line in serial.splitlines() if any(marker in line for marker in
        ('[LINUX:FAULT]','[PANIC]','[MM] ERROR: free invalido','[MM] CORRUPCION',
         '[LINUX:IPC] pair table full','fail=vm-resources','fail=task-create',
         'boot event wait timed out'))]
    normal_pass='[GATE:A] END' in serial and not faults and len(observations)==len(paths) and all(status==0 for _,status in observations) and all(results.get(path)==0 for path in paths)
    live_pass=(bool(args.live_marker) and live_stable and not faults and
               len(observations)==len(paths)-1 and
               all(results.get(path)==0 for path in paths[:-1]) and
               paths[-1] not in results)
    passed=(normal_pass or live_pass) and all(marker in serial for marker in args.expected_text)
    (out/'result.json').write_text(json.dumps({'passed':passed,'mode':'live' if live_pass else 'normal','live_marker':args.live_marker or None,'live_stable':live_stable,'results':results,'observations':observations,'fatal_faults':faults,'expected':paths,'elapsed':time.monotonic()-start},indent=2)+'\n')
    print(json.dumps(results,indent=2),flush=True)
    if not passed:raise SystemExit('GATE A no aprobado; consultar serial.log y result.json')
if __name__=='__main__':main()
