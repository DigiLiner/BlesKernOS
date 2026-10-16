from pathlib import Path
import ast,subprocess
p=Path('tests/desktop/test_iso_read_cache.py');tree=ast.parse(p.read_text());shim=next(ast.literal_eval(n.value) for n in tree.body if isinstance(n,ast.Assign) and any(isinstance(t,ast.Name) and t.id=='shim' for t in n.targets))
shim=shim.replace('static unsigned char disc[1024][2048];','static unsigned char disc[1024][2048];\nstatic FILE *real_disc;')
a=shim.index(' assert(d==&device && n && lba+n<=1024);');shim=shim[:a]+''' assert(d==&device && n);if(real_disc){assert(!fseek(real_disc,(long)lba*2048,SEEK_SET));return fread(out,2048,n,real_disc)==n;}
'''+shim[a:]
s=Path('kernel/drivers/storage/iso9660.c').read_text();s='\n'.join(l for l in s.splitlines() if not l.startswith('#include'));s=s[:s.index('bool iso9660_builtin_init(')]
main=r'''
int main(int argc,char **argv){assert(argc==2);real_disc=fopen(argv[1],"rb");assert(real_disc);fseek(real_disc,0,SEEK_END);device.sector_count=ftell(real_disc)/2048;
 assert(iso_impl_mount_default() && g_iso_joliet);iso9660_entry_t entry;
 assert(iso_impl_resolve("/setup/thrash/softdrawz.dll",&entry));assert(entry.size==176128);
 assert(iso_impl_resolve("/data/enviro/adboards_d.t3d",&entry));
 assert(iso_impl_resolve("/data/popups/verdana12bi.ffn",&entry));
 puts("PASS actual media/installers/FIFA2000.iso: Joliet renderer, stadium and font names resolve through production driver");fclose(real_disc);free(g_iso_cache);return 0;}
'''
c=Path('/tmp/bk-fifa2000/real-iso.c');c.write_text(shim+s+main);subprocess.run(['cc','-O2','-fsanitize=address,undefined',str(c),'-o','/tmp/bk-fifa2000/real-iso'],check=True);subprocess.run(['/tmp/bk-fifa2000/real-iso','media/installers/FIFA2000.iso'],check=True)
