/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef GLMARK2_BLES_H
#define GLMARK2_BLES_H
#include "native-state.h"
#include "gl-state.h"
#include "gl-visual-config.h"
#include "gl-headers.h"
#include "bleskernos_gallium.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <sys/time.h>
#include <unistd.h>
#include "model.h"
#include "texture.h"
static bool bkg_screensaver;
static inline void bkg_configure_screensaver(int& argc,char **argv) {
    for(int i=1;i<argc;i++)if(!std::strcmp(argv[i],"--bkg-screensaver")) {
        bkg_screensaver=true;
        setenv("BKG_JELLY_ACTIVE","1",1);
        setenv("BKG_FULLSCREEN","1",1);
        /* Use the same native presentation queue as standalone glmark2.
         * The old single-frame saver setting took a different, unvalidated
         * startup path. Rendering remains capped at 30 FPS below. */
        setenv("BKG_PRESENT_AHEAD","3",1);
        for(int n=i;n<argc;n++)argv[n]=argv[n+1];
        argc--;i--;
    }
    if(!bkg_screensaver)return;
    for(int i=1;i<argc;i++) {
        const char *env=nullptr,*v=nullptr;
        int low=0,high=0;
        if(!std::strncmp(argv[i],"--bkg-jellies=",14)) {
            env="BKG_JELLY_COUNT";v=argv[i]+14;low=1;high=10;
        } else if(!std::strncmp(argv[i],"--bkg-variety=",14)) {
            env="BKG_JELLY_VARIETY";v=argv[i]+14;low=0;high=3;
        }
        if(!env)continue;
        int n=std::atoi(v);
        if(n<low)n=low;
        if(n>high)n=high;
        char number[12];std::snprintf(number,sizeof(number),"%d",n);
        setenv(env,number,1);
        for(int j=i;j<argc;j++)argv[j]=argv[j+1];
        argc--;i--;
    }
}
static inline unsigned long long bkg_time_us() {
    timeval t;gettimeofday(&t,nullptr);return (unsigned long long)t.tv_sec*1000000+t.tv_usec;
}
static inline void bkg_configure_trace(int& argc,char **argv) {
    for(int i=1;i<argc;i++)if(!std::strcmp(argv[i],"--bkg-trace")) {
        setenv("BLES_WINE_GL_TRACE","1",1);
        for(int n=i;n<argc;n++)argv[n]=argv[n+1];
        argc--;i--;
    }
}
/* The OS launch API supplies one argument string. Expand it locally without
 * a shell; normal POSIX argv is preserved when called with multiple args. */
static inline void bkg_expand_arguments(int& argc,char **&argv) {
    if(argc!=2 || !std::strchr(argv[1],' '))return;
    static std::vector<std::string> words;
    static std::vector<char *> pointers;
    words.clear();pointers.clear();std::string word;char quote=0;bool escaped=false;
    for(const char *p=argv[1];;p++) {
        char ch=*p;
        if(escaped && ch){word+=ch;escaped=false;continue;}
        if(ch=='\\' && quote!='\''){escaped=true;continue;}
        if(quote && ch==quote){quote=0;continue;}
        if(!quote && (ch=='\'' || ch=='"')){quote=ch;continue;}
        if(!ch || (!quote && (ch==' ' || ch=='\t'))) {
            if(!word.empty()){words.push_back(word);word.clear();}
            if(!ch)break;
        } else word+=ch;
    }
    pointers.push_back(argv[0]);
    for(auto& w:words)pointers.push_back(&w[0]);
    pointers.push_back(nullptr);argc=pointers.size()-1;argv=pointers.data();
}
class NativeStateBles : public NativeState {
public:
    WindowProperties properties;
    bkg_context *context=nullptr;
    bool init_display() override {return true;}
    void *display() override {return this;}
    bool create_window(WindowProperties const& p) override {properties=p;return true;}
    void *window(WindowProperties& p) override {p=properties;return this;}
    void visible(bool) override {}
    bool should_quit() override {return bkg_should_quit(context);}
    void flip() override {}
};
class GLStateBles : public GLState {
    bkg_context *context=nullptr;
    NativeStateBles *native=nullptr;
    GLVisualConfig visual;
    bool initialized=false;
    static GLADapiproc load(void *,const char *name) {
        return reinterpret_cast<GLADapiproc>(bkg_get_proc(name));
    }
public:
    ~GLStateBles() override {if(context)bkg_destroy(context);}
    bool init_display(void *p,GLVisualConfig& v) override {native=static_cast<NativeStateBles *>(p);visual=v;return true;}
    bool init_surface(void *) override {return true;}
    bool reset() override {if(context)bkg_destroy(context);context=nullptr;initialized=false;if(native)native->context=nullptr;return true;}
    bool gotNativeConfig(intptr_t& id) override {id=1;return true;}
    void getVisualConfig(GLVisualConfig& v) override {v=visual;v.red=v.green=v.blue=v.alpha=8;v.buffer=32;v.depth=24;v.stencil=8;v.samples=0;}
    bool valid() override {
        if(!context) {
            /* Benchmark throughput is explicit; interactive clients retain
             * the runtime's default of one queued capture. */
            setenv("BKG_PRESENT_AHEAD","3",0);
            context=bkg_create(native->properties.width,native->properties.height);
        }
        native->context=context;
        if(!context || !bkg_current(context))return false;
        if(!initialized)initialized=gladLoadGLUserPtr(load,this) && init_gl_extensions();
        return initialized;
    }
    bool init_gl_extensions() override {
        GLExtensions::MapBuffer=glMapBuffer;GLExtensions::UnmapBuffer=glUnmapBuffer;
        GLExtensions::GenFramebuffers=glGenFramebuffersEXT;
        GLExtensions::DeleteFramebuffers=glDeleteFramebuffersEXT;
        GLExtensions::BindFramebuffer=glBindFramebufferEXT;
        GLExtensions::FramebufferTexture2D=glFramebufferTexture2DEXT;
        GLExtensions::FramebufferRenderbuffer=glFramebufferRenderbufferEXT;
        GLExtensions::CheckFramebufferStatus=glCheckFramebufferStatusEXT;
        GLExtensions::GenRenderbuffers=glGenRenderbuffersEXT;
        GLExtensions::DeleteRenderbuffers=glDeleteRenderbuffersEXT;
        GLExtensions::BindRenderbuffer=glBindRenderbufferEXT;
        GLExtensions::RenderbufferStorage=glRenderbufferStorageEXT;
        GLExtensions::GenerateMipmap=glGenerateMipmapEXT;
        return true;
    }
    void swap() override {
        static unsigned long long last,draw,swap_time,since,frames;
        bool trace=std::getenv("BLES_WINE_GL_TRACE");
        unsigned long long before=trace?bkg_time_us():0;
        if(!bkg_present(context)) {
            std::fprintf(stderr,"[GLMARK2:NATIVE] FAIL GPU presentation\n");std::exit(2);
        }
        if(trace) {
            unsigned long long after=bkg_time_us();
            if(last)draw+=before-last;
            swap_time+=after-before;last=after;frames++;
            if(!since)since=after;
            if(after-since>=1000000) {
                std::fprintf(stderr,"[GLMARK2:TIMING] frames=%llu draw_us=%llu swap_us=%llu\n",frames,draw/frames,swap_time/frames);
                since=after;draw=swap_time=frames=0;
            }
        }
    }
};
/* Use the original scene and shaders. The saver has no benchmark scoring,
 * runs until dismissed, and caps work at 30 FPS instead of stressing the GPU. */
static inline int bkg_run_screensaver(Canvas& canvas) {
    Model::find_models();Texture::find_textures();
    Benchmark benchmark("jellyfish:duration=1e30");
    Scene& scene=benchmark.setup_scene();
    if(!scene.running()) {benchmark.teardown_scene();return 3;}
    std::fprintf(stderr,"[JELLY:SAVER] Gallium jellyfish ready, count=%s variety=%s 30 FPS cap\n",
        std::getenv("BKG_JELLY_COUNT")?std::getenv("BKG_JELLY_COUNT"):"4",
        std::getenv("BKG_JELLY_VARIETY")?std::getenv("BKG_JELLY_VARIETY"):"2");
    while(!canvas.should_quit() && scene.running()) {
        unsigned long long started=bkg_time_us();
        canvas.clear();scene.draw();scene.update();canvas.update();
        unsigned long long elapsed=bkg_time_us()-started;
        if(elapsed<33333)usleep(33333-elapsed);
    }
    benchmark.teardown_scene();return 0;
}
#endif
