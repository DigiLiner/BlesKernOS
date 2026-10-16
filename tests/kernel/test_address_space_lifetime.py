#!/usr/bin/env python3
"""Exercise production borrowing, retirement, copy references and destruction."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'kernel/mm/paging.c').read_text()
lookup=s[s.index('static paging_process_space_t *paging_find_raw_space('):s.index('/* Count explicit process PTEs')]
borrow=s[s.index('bool paging_process_borrow('):s.index('static bool paging_space_private_pde(')]
swap=s[s.index('void paging_swap_process_ids('):s.index('uint32_t paging_current_process(')]
destroy=s[s.index('void paging_process_destroy('):s.index('bool paging_is_enabled(')]
code=r'''
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <assert.h>
#include <string.h>
#include <stdio.h>
#define PAGING_PROCESS_MAX 8
#define PAGING_PRIVATE_TABLE_MAX 2
#define SMP_MAX_CPUS 2
typedef struct {uint32_t process_id,borrowed_from,users;bool retiring;uint32_t *directory,*table[2];uint16_t pde_index[2],table_count;} paging_process_space_t;
static paging_process_space_t g_process_spaces[8];
static uint32_t g_active_process[2],directory[1024],table[1024];
static int g_paging_lock;static unsigned depth,freed;
#define kmemset memset
static uint32_t kspin_lock_irqsave(int *p){(void)p;assert(!depth);depth++;return 0;}
static void kspin_unlock_irqrestore(int *p,uint32_t f){(void)p;(void)f;assert(depth==1);depth--;}
static void paging_free_private_page(uint32_t *p){if(p){assert(!depth);freed++;}}
''' + lookup + borrow + swap + destroy + r'''
static void seed(void){memset(g_process_spaces,0,sizeof(g_process_spaces));memset(g_active_process,0,sizeof(g_active_process));freed=0;g_process_spaces[0]=(paging_process_space_t){.process_id=1,.directory=directory,.table={table},.table_count=1};}
int main(void){
 seed();assert(paging_process_borrow(1,2));assert(paging_process_borrow(2,3));
 assert(g_process_spaces[2].borrowed_from==1);assert(paging_process_has_borrowers(1));
 assert(!paging_process_retire(1));paging_process_destroy(1);assert(!freed && paging_find_space(3)==&g_process_spaces[0]);
 g_active_process[1]=2;assert(!paging_process_retire(2));paging_process_destroy(2);assert(paging_find_raw_space(2));
 g_active_process[1]=0;assert(paging_process_retire(2));assert(!paging_process_pin(2));paging_process_destroy(2);assert(!paging_find_raw_space(2));
 assert(!paging_process_retire(1));paging_process_destroy(3);assert(!paging_process_has_borrowers(1));
 assert(paging_process_pin(1));assert(!paging_process_retire(1));paging_process_destroy(1);assert(!freed);
 paging_process_unpin(1);g_active_process[1]=1;assert(!paging_process_retire(1));g_active_process[1]=0;
 assert(paging_process_retire(1));assert(!paging_process_pin(1));assert(!paging_process_borrow(1,4));
 paging_process_destroy(1);assert(freed==2 && !paging_find_raw_space(1));
 seed();assert(paging_process_borrow(1,2));paging_swap_process_ids(1,4);
 assert(paging_find_space(2)==&g_process_spaces[0] && g_process_spaces[0].process_id==4);
 assert(paging_process_has_borrowers(4) && !paging_process_retire(4));paging_process_destroy(2);assert(paging_process_retire(4));paging_process_destroy(4);assert(freed==2);
 seed();g_process_spaces[0].users=UINT32_MAX;assert(!paging_process_pin(1));assert(!paging_process_retire(1));
 /* Corrupt cyclic borrow chains fail closed rather than returning a borrower. */
 g_process_spaces[0].borrowed_from=2;g_process_spaces[1].process_id=2;g_process_spaces[1].borrowed_from=1;assert(!paging_find_space(1));
 seed();assert(paging_process_pin(7));paging_process_unpin(7);assert(paging_process_retire(7));
 puts("PASS borrowed CR3 lifetime, active CPU refusal, copy references, retirement, overflow and cycle rejection");
}
'''
with tempfile.TemporaryDirectory(prefix='bk-lifetime-') as d:
 p=Path(d);(p/'test.c').write_text(code)
 subprocess.run(['gcc','-O2','-Wall','-Wextra','-Werror',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
