#include "../include/vision_resources.h"
#include "../include/klock.h"
#include "../include/memory.h"
#include "../include/task.h"
/* The server's mutable links are not the kernel's lifetime registry. A fault
 * halfway through publication/removal must not make the reaper follow them. */
#define VISION_RESOURCES 384U
static struct {void *object;uint32_t owner,rank;bool program,active;} records[VISION_RESOURCES];
static kspinlock_t lock=KSPINLOCK_INITIALIZER;
static uint32_t sequence;
static int find(void *object){for(uint32_t n=0;n<VISION_RESOURCES;n++)if(records[n].object==object)return (int)n;return -1;}
bool bk_vision_resource_register(void *object,bool program) {
 if(mm_allocation_size(object)<(program?sizeof(gui_program_t):sizeof(gui_window_t)))return false;
 uint32_t flags=kspin_lock_irqsave(&lock);int slot=find(object);
 if(slot<0)for(uint32_t n=0;n<VISION_RESOURCES;n++)if(!records[n].object){slot=(int)n;break;}
 if(slot>=0){records[slot].object=object;records[slot].program=program;records[slot].active=true;
  records[slot].owner=task_current_process_id();records[slot].rank=++sequence;}
 kspin_unlock_irqrestore(&lock,flags);return slot>=0;
}
void bk_vision_resource_retire(void *object){uint32_t flags=kspin_lock_irqsave(&lock);int n=find(object);if(n>=0)records[n].active=false;kspin_unlock_irqrestore(&lock,flags);}
void bk_vision_resource_forget(void *object){uint32_t flags=kspin_lock_irqsave(&lock);int n=find(object);if(n>=0)records[n].object=NULL;kspin_unlock_irqrestore(&lock,flags);}
void bk_vision_resource_owner(void *object,uint32_t owner){uint32_t flags=kspin_lock_irqsave(&lock);int n=find(object);if(n>=0)records[n].owner=owner;kspin_unlock_irqrestore(&lock,flags);}
bool vision_resources_checkpoint(gui_desktop_t *desktop) {
 if(!desktop)return false;
 uint32_t flags=kspin_lock_irqsave(&lock),count=0,rank=0;bool ok=true;
 gui_window_t *previous=NULL,*window=desktop->first_window;
 while(window){int n=find(window);
  if(n<0 || records[n].program || !records[n].active || ++count>VISION_RESOURCES || window->prev!=previous){ok=false;break;}
  previous=window;window=window->next;
 }
 if(previous!=desktop->last_window)ok=false;
 count=0;gui_program_t *prior=NULL,*program=desktop->first_program;
 while(ok && program){int n=find(program);
  if(n<0 || !records[n].program || !records[n].active || ++count>VISION_RESOURCES || program->prev!=prior){ok=false;break;}
  prior=program;program=program->next;
 }
 if(prior!=desktop->last_program)ok=false;
 if(ok) {
  for(window=desktop->first_window;window;window=window->next)records[find(window)].rank=++rank;
  for(program=desktop->first_program;program;program=program->next)records[find(program)].rank=++rank;
  sequence=rank;
 }
 kspin_unlock_irqrestore(&lock,flags);return ok;
}
void vision_resources_recover(gui_desktop_t *desktop) {
 if(!desktop)return;
 uint32_t flags=kspin_lock_irqsave(&lock);
 desktop->first_window=NULL;desktop->last_window=NULL;desktop->first_program=NULL;desktop->last_program=NULL;
 desktop->focused_window=NULL;desktop->drag_window=NULL;desktop->resize_window=NULL;desktop->drag_outline_visible=false;
 /* Select the oldest remaining rank, without following any server link. */
 uint32_t previous_rank=0;
 for(uint32_t count=0;count<VISION_RESOURCES;count++) {
  int chosen=-1;uint32_t rank=~0U;
  for(uint32_t n=0;n<VISION_RESOURCES;n++)if(records[n].object && records[n].active && records[n].rank>previous_rank && records[n].rank<rank){chosen=(int)n;rank=records[n].rank;}
  if(chosen<0)break;
  previous_rank=rank;
  if(records[chosen].program){gui_program_t *p=records[chosen].object;p->prev=desktop->last_program;p->next=NULL;
   if(p->prev)p->prev->next=p;else desktop->first_program=p;desktop->last_program=p;
  } else {gui_window_t *w=records[chosen].object;w->owner_pid=records[chosen].owner;w->desktop=desktop;
   w->prev=desktop->last_window;w->next=NULL;w->focused=false;
   if(w->prev)w->prev->next=w;else desktop->first_window=w;desktop->last_window=w;
   if(w->visible && !w->minimized && !w->destroy_state)desktop->focused_window=w;
  }
 }
 if(desktop->focused_window)desktop->focused_window->focused=true;
 kspin_unlock_irqrestore(&lock,flags);
 gui_desktop_invalidate_all(desktop);gui_request_paint();
}
