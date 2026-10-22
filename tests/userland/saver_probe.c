/* Compile one real saver per probe, including its private render functions. */
#define bleskernos_program_main saver_original_entry
#ifndef SAVER_KIND
#define SAVER_KIND 1
#endif
#ifndef SAVER_SOURCE
#if SAVER_KIND == 1
#define SAVER_SOURCE "../../system/screensavers/ss_logo.c"
#elif SAVER_KIND == 2
#define SAVER_SOURCE "../../system/screensavers/ss_balls.c"
#else
#define SAVER_SOURCE "../../system/screensavers/ss_pipes.c"
#endif
#endif
#include SAVER_SOURCE
#undef bleskernos_program_main

static uint32_t probe_started, probe_frames, probe_render_ms, probe_reported;
static void probe_paint(gui_program_t *program, gui_desktop_t *desktop, gui_surface_t *surface) {
    uint32_t start = bk_sys_uptime_ms();
#if SAVER_KIND == 1
    ss_logo_paint(program,desktop,surface);
#elif SAVER_KIND == 2
    ss_balls_paint(program,desktop,surface);
#else
    ss_pipes_paint(program,desktop,surface);
#endif
    probe_render_ms += bk_sys_uptime_ms()-start;
    probe_frames++;
    uint32_t now = bk_sys_uptime_ms();
    if ((uint32_t)(now-probe_reported)>=1000U) {
        probe_reported=now;
        kprintf("[SAVERPROBE] live_frames=%u live_ms=%u paint_ms=%u\n",probe_frames,now-probe_started,probe_render_ms);
    }
}

void bleskernos_program_main(gui_desktop_t *desktop) {
    gui_surface_t surface = {.width=800,.height=600,.pitch=800,.clip={0,0,800,600}};
    surface.pixels=bk_sys_alloc_zero(800U*600U*4U);
    if (!surface.pixels) {kprintf("[SAVERPROBE] FAIL allocation\n");return;}

#if SAVER_KIND == 2 && !defined(SAVER_BASELINE)
    ss_balls_state_t *fast=bk_sys_alloc_zero(sizeof(*fast));
    ss_balls_state_t *slow=bk_sys_alloc_zero(sizeof(*slow));
    if(!fast || !slow){kprintf("[SAVERPROBE] FAIL timing allocation\n");return;}
    fast->seed=12345;fast->render_w=400;fast->render_h=300;
    ss_balls_configure_world(fast);ss_balls_reset_scene(fast,0);
    *slow=*fast;
    for(uint32_t now=33;now<=990;now+=33)ss_balls_update(fast,now);
    for(uint32_t now=99;now<=990;now+=99)ss_balls_update(slow,now);
    bool equal=fast->ball_count==slow->ball_count && fast->seed==slow->seed;
    for(unsigned i=0;equal && i<fast->ball_count;i++){
        ss_ball_t *a=&fast->balls[i],*b=&slow->balls[i];
        equal=a->x==b->x && a->y==b->y && a->z==b->z &&
              a->vx==b->vx && a->vy==b->vy && a->vz==b->vz && a->age==b->age;
    }
    bk_sys_free(fast);bk_sys_free(slow);
    if(!equal){kprintf("[SAVERPROBE] FAIL frame-independent physics\n");return;}
    kprintf("[SAVERPROBE] PASS identical physics at 33ms and 99ms cadence\n");
#endif
    uint32_t start,render_ms,blit_ms=0;
#if SAVER_KIND == 1
    ss_logo_state_t st={.active=true,.x=200,.y=200,.dx=2,.dy=2,.seed=12345};
    gui_program_t program={.state=&st};
    st.last_motion_ms=bk_sys_uptime_ms();
#ifndef SAVER_BASELINE
    ss_logo_prepare(&st);
#endif
    start=bk_sys_uptime_ms();
    for(unsigned i=0;i<3;i++)ss_logo_paint(&program,desktop,&surface);
    render_ms=bk_sys_uptime_ms()-start;
#ifndef SAVER_BASELINE
    if(st.logo_pixels)bk_sys_free(st.logo_pixels);
#endif
#elif SAVER_KIND == 2
    ss_balls_state_t st={.seed=12345};
    ss_balls_configure_resolution(&st,desktop);ss_balls_configure_world(&st);
    if(!ss_balls_init_gl(&st)){kprintf("[SAVERPROBE] FAIL Mesa\n");return;}
    for(unsigned i=0;i<64;i++){
        ss_balls_spawn(&st);ss_ball_t *b=&st.balls[i];
        b->x=st.box_left+0.65f+(i%8)*1.1f;b->y=SS_BALLS_FLOOR+0.5f+(i/8)*0.9f;
        b->z=0;b->vx=b->vy=b->vz=0;b->age=100;
    }
    start=bk_sys_uptime_ms();
    for(unsigned i=0;i<3;i++){st.last_spawn_ms=bk_sys_uptime_ms();ss_balls_render_frame(&st);}
    render_ms=bk_sys_uptime_ms()-start;start=bk_sys_uptime_ms();
    for(unsigned i=0;i<10;i++)ss_balls_blit(&surface,&st);
    blit_ms=bk_sys_uptime_ms()-start;ss_mesa_surface_destroy(&st.mesa);
#else
    ss_pipes_state_t st={.seed=12345};ss_pipes_configure_resolution(&st,desktop);
    if(!ss_pipes_init_gl(&st)){kprintf("[SAVERPROBE] FAIL Mesa\n");return;}
    while(st.segment_count<64)ss_pipes_grow(&st);
    start=bk_sys_uptime_ms();
    for(unsigned i=0;i<3;i++){st.last_grow_ms=bk_sys_uptime_ms();ss_pipes_render_frame(&st);}
    render_ms=bk_sys_uptime_ms()-start;start=bk_sys_uptime_ms();
    for(unsigned i=0;i<10;i++)ss_pipes_blit(&surface,&st);
    blit_ms=bk_sys_uptime_ms()-start;ss_mesa_surface_destroy(&st.mesa);
#endif
    bk_sys_free(surface.pixels);
    kprintf("[SAVERPROBE] BENCH kind=%u frames=3 render_ms=%u blit_frames=10 blit_ms=%u\n",SAVER_KIND,render_ms,blit_ms);
#if SAVER_KIND == 1
    ss_logo_open_from_desktop(desktop);ss_logo_state_t *live=g_ss_logo;
#elif SAVER_KIND == 2
    ss_balls_open_from_desktop(desktop);ss_balls_state_t *live=g_ss_balls;
#else
    ss_pipes_open_from_desktop(desktop);ss_pipes_state_t *live=g_ss_pipes;
#endif
    if(!live){kprintf("[SAVERPROBE] FAIL live start\n");return;}
    live->program->paint=probe_paint;
    probe_started=probe_reported=bk_sys_uptime_ms();
    uint32_t last=probe_started;
    kprintf("[SAVERPROBE] LIVE\n");
    while(
#if SAVER_KIND == 1
      ss_logo_is_active()
#elif SAVER_KIND == 2
      ss_balls_is_active()
#else
      ss_pipes_is_active()
#endif
      && !bk_proc_exit_requested()) {
        if(ss_frame_due(bk_sys_uptime_ms(),&last)){
            bk_gui_desktop_invalidate_all(desktop);bk_gui_request_paint();
        }
        bk_sys_sleep_ms(5U);
    }
    kprintf("[SAVERPROBE] CLOSED frames=%u elapsed_ms=%u\n",probe_frames,bk_sys_uptime_ms()-probe_started);
}
