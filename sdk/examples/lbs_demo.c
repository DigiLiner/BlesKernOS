/* Runs in Ring 3. Every service under test is discovered from its own LBS. */
#include <bleskernos_lbs_services.h>
#include <bleskernos_heap.h>
#include <bleskernos_input_lbs.h>
#include <bleskernos_ngs.h>
#include <bleskernos_ngs3d.h>
#include <bleskernos_media_lbs.h>
#include <bleskernos_archive_lbs.h>
#include <bleskernos_hyperzip_lbs.h>
#include <bleskernos_sound_lbs.h>
#include <bleskernos_print_lbs.h>
#include <dlfcn.h>
#include <link.h>
#include <stdio.h>
#include <string.h>
#include <GL/gl.h>
static void *modules[12];
static const char *names[] = {"SYSTEM", "VISION", "SOUND", "NETWORK", "NGS",    "NGS3D",
                              "MEDIA",  "INPUT",  "FONT",  "PRINT",   "CRYPTO", "SECURITY"};
static void check(int ok, const char *message) {
    char line[256];
    snprintf(line, sizeof(line), "[LBSTEST] %s: %s\n", ok ? "PASS" : "FAIL", message);
    bk_sys_log(line);
    if (!ok) {
        char *error = dlerror();
        if (error)
            bk_sys_log(error);
        bk_exit(1);
    }
}
static void *symbol(unsigned int m, const char *name) {
    void *p = dlsym(modules[m], name);
    check(p != 0, name);
    return p;
}
#define LOAD(type, var, m, name) __typeof__((type)0) var = (type)symbol(m, name)
static volatile unsigned int keys, mouse, key_state_verified;
static bk_input_state_t *tracked_input;
static __typeof__(&bk_input_state_event) feed_input;
static __typeof__(&bk_input_state_key) key_down;
static __typeof__(&bk_font_draw_text) font_draw;
static __typeof__(&bk_gui_surface_fill_rect) ngs_fill_native;
static void paint(bk_gui_window_t *w, bk_gui_surface_t *s, void *u) {
    (void)w;
    (void)u;
    ngs_fill_native(s, (bk_gui_rect_t){0, 0, 400, 250}, 0x00ffffff);
    check(font_draw(s, 0, 14, 8, 8, "LBS Ring 3: Vision + FONT", 25, 0x00000000,
                    (bk_gui_rect_t){0, 0, 400, 250}),
          "FONT paint");
}
static bool input(bk_gui_window_t *w, const bk_gui_event_t *e, void *u) {
    (void)w;
    (void)u;
    if (tracked_input)
        feed_input(tracked_input, e);
    if (e->type == BK_GUI_EVENT_KEY) {
        keys++;
        if (tracked_input && key_down(tracked_input, e->key))
            key_state_verified++;
    }
    if (e->type == BK_GUI_EVENT_MOUSE_MOVE || e->type == BK_GUI_EVENT_MOUSE_DOWN)
        mouse++;
    return false;
}
static int count_module(struct dl_phdr_info *info, unsigned int size, void *out) {
    (void)info;
    (void)size;
    (*(unsigned int *)out)++;
    return 0;
}
static int log_module(struct dl_phdr_info *info, unsigned int size, void *unused) {
    char line[320];
    (void)size;
    (void)unused;
    snprintf(line, sizeof(line), "[LBSTEST] module_bias=%x path=%s\n", info->dlpi_addr,
             info->dlpi_name);
    bk_sys_log(line);
    return 0;
}
static uint32_t own_memory(void) {
    bk_process_info_t infos[128];
    uint32_t i, n = bk_proc_snapshot(infos, 128), pid = bk_sys_getpid();
    for (i = 0; i < n; i++)
        if (infos[i].pid == pid)
            return infos[i].memory_bytes;
    return 0;
}
static void isolation_child(void) {
    void *h = dlopen("/SYSTEM/TESTS/NATIVE/FIXDEP.LBS", RTLD_NOW);
    int (*get)(void) = h ? dlsym(h, "lbs_fixture_get") : 0;
    void (*set)(int) = h ? dlsym(h, "lbs_fixture_set") : 0;
    check(get && set && get() == 7, "child private library data");
    set(99);
    bk_sys_sleep_ms(100);
    check(get() == 99, "child private write retained");
    check(!dlclose(h), "child release");
    bk_exit(0);
}
static void loader_lifecycle(void) {
    void *h, *dep;
    int counter = 0, status;
    int32_t child[2];
    unsigned int i;
    h = dlopen("/SYSTEM/TESTS/NATIVE/FIXROOT.LBS", RTLD_NOW);
    check(h != 0, "guest DT_NEEDED fixture");
    int (*answer)(void) = dlsym(h, "lbs_fixture_answer");
    check(answer && answer() == 42, "guest relocation and constructor");
    void (*set_counter)(int *) = dlsym(h, "lbs_fixture_counter");
    check(set_counter != 0, "dependency symbol scope");
    set_counter(&counter);
    check(!dlclose(h) && counter == 1, "guest destructor and dependency unload");
    check(!dlopen("/SYSTEM/TESTS/NATIVE/CYCLEA.LBS", RTLD_NOW), "guest cycle rejected");
    check(!dlopen("/SYSTEM/TESTS/NATIVE/MISSINGROOT.LBS", RTLD_NOW),
          "guest missing transitive dependency rejected");
    dep = dlopen("/SYSTEM/TESTS/NATIVE/FIXDEP.LBS", RTLD_NOW);
    check(dep != 0, "parent isolation fixture");
    int (*get)(void) = dlsym(dep, "lbs_fixture_get");
    void (*set)(int) = dlsym(dep, "lbs_fixture_set");
    check(get && set, "parent fixture functions");
    set(41);
    for (i = 0; i < 2; i++) {
        child[i] = bk_spawn("/SYSTEM/TESTS/NATIVE/LBSTEST.BEX", "--isolation");
        check(child[i] > 0, "spawn simultaneous LBS process");
    }
    for (i = 0; i < 2; i++) {
        check(bk_waitpid(child[i], &status) >= 0 && status == 0, "child exited successfully");
    }
    check(get() == 41, "parent data isolated across processes");
    check(!dlclose(dep), "parent fixture release");
}
void bleskernos_program_main(void *unused) {
    unsigned int i, n, before, after;
    char path[64], line[240], arg[80];
    uint32_t started = bk_uptime_ms();
    (void)unused;
    if (bk_proc_launch_arg_copy(arg, sizeof(arg)) && !strcmp(arg, "--isolation"))
        isolation_child();
    loader_lifecycle();
    {
        void *archive = bk_lbs_open("ARCHIVE.LBS", 1);
        check(archive != 0, "ARCHIVE load");
        __typeof__(&bk_archive_pack) pack=dlsym(archive,"bk_archive_pack");
        __typeof__(&bk_archive_unpack) unpack=dlsym(archive,"bk_archive_unpack");
        check(pack && unpack, "ARCHIVE exports");
        void *encoded=0,*decoded=0;uint32_t en=0,dn=0;
        check(!pack("AAAAABBBBBBC",12,&encoded,&en) && !unpack(encoded,en,&decoded,&dn) &&
              dn==12 && !memcmp(decoded,"AAAAABBBBBBC",12), "ARCHIVE real round trip");
        bk_sys_free(decoded);
        check(unpack(encoded,en-1,&decoded,&dn)==BK_LBS_INVALID && !decoded && !dn,
              "ARCHIVE rejects truncation without output");
        bk_sys_free(encoded);
        __typeof__(&hzip_archive_init) zip_init=dlsym(archive,"hzip_archive_init");
        __typeof__(&hzip_archive_open) zip_open=dlsym(archive,"hzip_archive_open");
        __typeof__(&hzip_archive_close) zip_close=dlsym(archive,"hzip_archive_close");
        __typeof__(&hzip_read_entry) zip_read=dlsym(archive,"hzip_read_entry");
        check(zip_init && zip_open && zip_close && zip_read,"ARCHIVE ZIP exports");
        hzip_archive_t zip;zip_init(&zip);
        check(!zip_open(&zip,"/SYSTEM/TESTS/NATIVE/SHARED.ZIP") && zip.entry_count==2,
              "ARCHIVE opens ZIP through native descriptor domain");
        check(dlclose(archive)<0,"ARCHIVE ZIP resources prevent unload");
        for(unsigned entry=0;entry<2;entry++) {
            check(!zip_read(&zip,entry,&decoded,&dn) && dn==12 &&
                  !memcmp(decoded,"shared-code!",12),"ARCHIVE stored/deflated ZIP and CRC");
            bk_sys_free(decoded);
        }
        zip_close(&zip);
        check(!dlclose(archive), "ARCHIVE release");
        void *command = bk_lbs_open("COMMAND.LBS",1);
        check(command != 0, "COMMAND load");
        uint32_t (*number)(const char *,bool *)=dlsym(command,"command_number");
        bool valid=false;
        check(number && number("12345",&valid)==12345 && valid, "COMMAND shared parser");
        check(!dlclose(command), "COMMAND release");
    }
    for (i = 0; i < 12; i++) {
        snprintf(path, sizeof(path), "%s.LBS", names[i]);
        modules[i] = bk_lbs_open(path, 1);
        check(modules[i] != 0, path);
        {
            const bk_lbs_info_t *(*info)(void) = dlsym(modules[i], "bk_lbs_info");
            check(info && info()->abi_major == 1 && !strcmp(info()->name, names[i]),
                  "module ABI and identity");
        }
    }
    {
        bk_dl_statistics_t stats = {sizeof(stats), 0, 0, 0, 0};
        check(!bk_dl_statistics(&stats), "loader memory statistics");
        snprintf(line, sizeof(line), "[LBSTEST] virtual_images_bytes=%u process_bytes=%u\n",
                 stats.image_bytes, own_memory());
        bk_sys_log(line);
    }
    {
        bk_dl_mapping_statistics_t mapped = {sizeof(mapped), 0, 0, 0, 0};
        check(!bk_dl_mapping_statistics(&mapped), "native VM mapping statistics");
        check(mapped.file_readonly_bytes > 0 && mapped.private_bytes < mapped.virtual_bytes,
              "LBS read-only file mappings enabled");
        snprintf(line, sizeof(line), "[LBSTEST] file_readonly_bytes=%u private_mapping_bytes=%u\n",
                 mapped.file_readonly_bytes, mapped.private_bytes);
        bk_sys_log(line);
    }
    snprintf(line, sizeof(line), "[LBSTEST] load_12_ms=%u\n", bk_uptime_ms() - started);
    bk_sys_log(line);
    {
        void *alias = dlopen("/SYSTEM/LIBS/NATIVE/SYSTEM.LBS", RTLD_NOW);
        check(alias == modules[0], "former native directory aliases SYSTEM module");
        check(alias && !dlclose(alias), "former directory alias reference released");
    }
    check(!bk_lbs_open("SYSTEM.LBS", 2), "ABI mismatch rejected");
    char *abi_error = dlerror();
    check(abi_error && strstr(abi_error, "ABI"), "ABI mismatch diagnostic");
    check(!dlopen("/SYSTEM/LIBS/NATIVE/MISSING.LBS", RTLD_NOW), "missing module rejected");
    check(!dlopen("/SYSTEM/LIBS/NATIVE/CORRUPT.LBS", RTLD_NOW), "corrupt module rejected");
    check(!dlsym(modules[0], "missing_symbol"), "missing symbol rejected");
    check(dlclose((void *)0x12345) < 0, "invalid handle rejected");
    n = 0;
    dl_iterate_phdr(count_module, &n);
    check(n == 12, "dependency deduplication");
    {
        LOAD(uint32_t (*)(void), getpid, 0, "bk_sys_getpid");
        check(getpid() == (uint32_t)bk_getpid(), "SYSTEM process");
        LOAD(void *(*)(size_t), alloc, 0, "bk_sys_alloc");
        LOAD(void (*)(void *), release, 0, "bk_sys_free");
        void *p = alloc(4096);
        check(p != 0, "SYSTEM allocation");
        memset(p, 0xa5, 4096);
        release(p);
    }
    {
        LOAD(int (*)(const void *, uint32_t, uint8_t *), hash, 10, "bk_crypto_sha256");
        static const uint8_t expected[32] = {0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea,
                                             0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
                                             0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c,
                                             0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad};
        uint8_t digest[32];
        check(!hash("abc", 3, digest) && !memcmp(digest, expected, 32),
              "BearSSL SHA256 known answer");
        LOAD(__typeof__(&bk_crypto_sha224), hash224, 10, "bk_crypto_sha224");
        static const uint8_t expected224[28] = {
            0x23, 0x09, 0x7d, 0x22, 0x34, 0x05, 0xd8, 0x22, 0x86, 0x42, 0xa4, 0x77, 0xbd, 0xa2,
            0x55, 0xb3, 0x2a, 0xad, 0xbc, 0xe4, 0xbd, 0xa0, 0xb3, 0xf7, 0xe3, 0x6c, 0x9d, 0xa7};
        check(!hash224("abc", 3, digest) && !memcmp(digest, expected224, 28),
              "BearSSL SHA224 known answer");
        LOAD(__typeof__(&bk_crypto_hmac_sha256), hmac, 10, "bk_crypto_hmac_sha256");
        static const uint8_t expected_hmac[32] = {0xb0, 0x34, 0x4c, 0x61, 0xd8, 0xdb, 0x38, 0x53,
                                                  0x5c, 0xa8, 0xaf, 0xce, 0xaf, 0x0b, 0xf1, 0x2b,
                                                  0x88, 0x1d, 0xc2, 0x00, 0xc9, 0x83, 0x3d, 0xa7,
                                                  0x26, 0xe9, 0x37, 0x6c, 0x2e, 0x32, 0xcf, 0xf7};
        uint8_t key[20];
        memset(key, 0x0b, sizeof(key));
        check(!hmac(key, sizeof(key), "Hi There", 8, digest) && !memcmp(digest, expected_hmac, 32),
              "BearSSL HMAC-SHA256 known answer");
        LOAD(int (*)(void *, uint32_t), random, 10, "bk_crypto_random");
        LOAD(int (*)(void), random_available, 10, "bk_crypto_random_available");
        snprintf(line, sizeof(line), "[LBSTEST] hardware_rng=%u\n", random_available());
        bk_sys_log(line);
        check(random(digest, 32) == (random_available() ? 0 : BK_LBS_UNSUPPORTED),
              "hardware CSPRNG or explicit unsupported");
    }
    {
        LOAD(int (*)(bk_security_context_t *), context, 11, "bk_security_context");
        bk_security_context_t c = {sizeof(c), 0, 0, 0};
        check(!context(&c) && c.pid == (uint32_t)bk_getpid() && c.flags == BK_SECURITY_RING3,
              "SECURITY process context");
        LOAD(int (*)(uint32_t, const char *), auth, 11, "bk_security_authorize");
        check(auth(0, "/") == BK_LBS_UNSUPPORTED, "no fictitious authorization");
    }
    {
        LOAD(int (*)(uint8_t *, uint32_t, const uint8_t *const *, const uint16_t *, uint32_t), mix,
             2, "bk_sound_mix_u8");
        const uint8_t a[] = {255, 0, 128}, b[] = {255, 0, 128};
        const uint8_t *channels[] = {a, b};
        uint16_t gains[] = {256, 256};
        uint8_t out[3];
        check(!mix(out, 3, channels, gains, 2) && out[0] == 255 && out[1] == 0 && out[2] == 128,
              "SOUND saturating mixer");
        LOAD(bool (*)(void), available, 2, "bk_sound_pcm_available");
        if (available()) {
            LOAD(__typeof__(&bk_sound_play_pcm_u8), play_pcm, 2, "bk_sound_play_pcm_u8");
            LOAD(void (*)(void), stop_pcm, 2, "bk_sound_stop");
            uint8_t samples[4000];
            for (i = 0; i < sizeof(samples); i++)
                samples[i] = (i / 9) & 1 ? 160 : 96;
            stop_pcm();
            check(play_pcm(samples, sizeof(samples), 22050, 80),
                  "SOUND PCM driver accepted buffer");
            bk_sys_sleep_ms(550);
            stop_pcm();
        } else
            bk_sys_log("[LBSTEST] SKIP: no PCM device");
    }
    {
        LOAD(bool (*)(bk_net_info_t *), netinfo, 3, "bk_net_get_info");
        bk_net_info_t info;
        check(netinfo(&info), "NETWORK interface snapshot");
        LOAD(int (*)(uint8_t), socket_open, 3, "bk_net_socket_open");
        LOAD(void (*)(int), socket_close, 3, "bk_net_socket_close");
        int socket = socket_open(1);
        check(socket >= 0, "NETWORK TCP resource");
        socket_close(socket);
        LOAD(__typeof__(&bk_net_socket_bind), bind_socket, 3, "bk_net_socket_bind");
        LOAD(__typeof__(&bk_net_socket_listen), listen_socket, 3, "bk_net_socket_listen");
        LOAD(__typeof__(&bk_net_socket_connect), connect_socket, 3, "bk_net_socket_connect");
        LOAD(__typeof__(&bk_net_socket_accept), accept_socket, 3, "bk_net_socket_accept");
        LOAD(__typeof__(&bk_net_socket_send), send_socket, 3, "bk_net_socket_send");
        LOAD(__typeof__(&bk_net_socket_receive), receive_socket, 3, "bk_net_socket_receive");
        LOAD(__typeof__(&bk_net_socket_sendto), send_datagram, 3, "bk_net_socket_sendto");
        LOAD(__typeof__(&bk_net_socket_receivefrom), receive_datagram, 3,
             "bk_net_socket_receivefrom");
        LOAD(__typeof__(&bk_net_resolve), resolve, 3, "bk_net_resolve");
        uint8_t loopback[4] = {127, 0, 0, 1}, peer[4];
        uint16_t port;
        char payload[8] = {0};
        check(resolve("127.0.0.1", peer, 1000) && !memcmp(peer, loopback, 4),
              "NETWORK native resolver");
        int server = socket_open(BK_NET_SOCKET_TCP), client = socket_open(BK_NET_SOCKET_TCP);
        check(server >= 0 && client >= 0 && bind_socket(server, loopback, 44321) &&
                  listen_socket(server, 2),
              "NETWORK TCP server");
        check(connect_socket(client, loopback, 44321, 1000), "NETWORK TCP client loopback");
        int accepted = accept_socket(server, peer, &port, 1000);
        check(accepted >= 0, "NETWORK TCP accept");
        check(send_socket(client, "LBS", 3, 1000) == 3 &&
                  receive_socket(accepted, payload, sizeof(payload), 1000) == 3 &&
                  !memcmp(payload, "LBS", 3),
              "NETWORK TCP real transfer");
        check(receive_socket(accepted, payload, sizeof(payload), 0) <= 0,
              "NETWORK nonblocking empty receive");
        socket_close(accepted);
        socket_close(client);
        socket_close(server);
        server = socket_open(BK_NET_SOCKET_UDP);
        client = socket_open(BK_NET_SOCKET_UDP);
        check(server >= 0 && client >= 0 && bind_socket(server, loopback, 44322),
              "NETWORK UDP server");
        /* The existing stack has TCP loopback; UDP is exercised over the
           real RTL8139/SLIRP path to a host-local echo fixture. */
        void *port_text = 0;
        uint32_t port_size = 0, echo_port = 0;
        uint8_t host[4] = {10, 0, 2, 2};
        check(bk_file_read_all("/SYSTEM/TESTS/NATIVE/UDPPORT.TXT", &port_text, &port_size),
              "NETWORK UDP echo fixture");
        for (uint32_t digit = 0; digit < port_size; digit++)
            echo_port = echo_port * 10 + ((const char *)port_text)[digit] - '0';
        bk_sys_free(port_text);
        check(echo_port > 0 && echo_port <= 65535 &&
                  send_datagram(client, host, echo_port, "UDP", 3, 5000) == 3 &&
                  receive_datagram(client, peer, &port, payload, sizeof(payload), 5000) == 3 &&
                  !memcmp(payload, "UDP", 3),
              "NETWORK UDP real datagram");
        socket_close(client);
        socket_close(server);
    }
    {
        LOAD(const char *(*)(void), formats, 6, "bk_media_formats");
        check(strstr(formats(), "MPEG1") != 0, "MEDIA existing formats");
        LOAD(bk_media_video_t * (*)(const void *, uint32_t), open_video, 6, "bk_media_video_open");
        check(!open_video("broken", 6), "MEDIA corrupt stream rejected");
        LOAD(bool (*)(bk_gui_image_t *, const void *, uint32_t), bmp, 6, "bk_gui_image_decode_bmp");
        LOAD(void (*)(bk_gui_image_t *), free_image, 6, "bk_gui_image_free");
        uint8_t data[58] = {0};
        bk_gui_image_t image = {0};
        data[0] = 'B';
        data[1] = 'M';
        data[2] = 58;
        data[10] = 54;
        data[14] = 40;
        data[18] = 1;
        data[22] = 1;
        data[26] = 1;
        data[28] = 24;
        data[54] = 0x11;
        data[55] = 0x22;
        data[56] = 0x33;
        check(bmp(&image, data, sizeof(data)) && image.width == 1 && image.height == 1,
              "MEDIA BMP decode");
        free_image(&image);
        void *gif = 0;
        uint32_t size = 0;
        check(bk_file_read_all("/SYSTEM/TESTS/NATIVE/FRAME.GIF", &gif, &size),
              "MEDIA video fixture");
        bk_media_video_t *video = open_video(gif, size);
        check(video != 0, "MEDIA real GIF video stream");
        LOAD(__typeof__(&bk_media_video_frame), frame, 6, "bk_media_video_frame");
        LOAD(__typeof__(&bk_media_video_close), close_video, 6, "bk_media_video_close");
        check(!frame(video, 0, &image) && image.width == 8 && image.height == 8,
              "MEDIA decoded video frame");
        LOAD(__typeof__(&bk_media_video_info), video_info, 6, "bk_media_video_info");
        LOAD(__typeof__(&bk_media_video_prepare), prepare, 6, "bk_media_video_prepare");
        LOAD(__typeof__(&bk_media_video_invalidate), invalidate, 6, "bk_media_video_invalidate");
        LOAD(__typeof__(&bk_media_video_frame_duration), duration, 6, "bk_media_video_frame_duration");
        LOAD(__typeof__(&bk_media_audio_seek), audio_seek, 6, "bk_media_audio_seek");
        LOAD(__typeof__(&bk_media_audio_decode), audio_decode, 6, "bk_media_audio_decode");
        bk_media_video_info_t metadata = {0};
        check(video_info(video, &metadata) == BK_LBS_INVALID, "MEDIA DTO size validated");
        metadata.size = sizeof(metadata);
        check(!video_info(video, &metadata) && metadata.width == 8 && metadata.height == 8 &&
              metadata.format == BK_MEDIA_GIF && metadata.frame_count == 1,
              "MEDIA opaque metadata");
        invalidate(video);
        check(prepare(video, 0) && duration(video, 0) > 0 && !prepare(video, 99),
              "MEDIA invalidate, decode, timing and frame bounds");
        uint32_t chunk=0, offset=0; uint8_t sample=0;
        audio_seek(video, 0, &chunk, &offset);
        check(!audio_decode(video, &chunk, &offset, &sample, 1), "MEDIA no fictitious GIF audio");
        close_video(video);
        bk_sys_free(gif);
        LOAD(__typeof__(&bk_media_video_open_path), open_path, 6, "bk_media_video_open_path");
        LOAD(__typeof__(&bk_media_video_draw), draw_movie, 6, "bk_media_video_draw");
        for (unsigned stream=0;stream<2;stream++) {
            const char *path=stream?"/SYSTEM/TESTS/NATIVE/STREAM.MPG":"/SYSTEM/TESTS/NATIVE/STREAM.AVI";
            int fd=bk_file_open(path,BK_FILE_READ_ONLY);
            check(fd>=0,"MEDIA streaming fixture exists");
            uint32_t length=bk_file_size(fd);bk_file_close(fd);
            snprintf(line,sizeof(line),"[MEDIA:TEST] format=%u length=%u\n",stream?BK_MEDIA_MPEG1:BK_MEDIA_AVI,length);bk_sys_log(line);
            video=open_path(path,length,stream?BK_MEDIA_MPEG1:BK_MEDIA_AVI);
            check(video!=0,"MEDIA file-backed open");
            metadata.size=sizeof(metadata);
            check(!video_info(video,&metadata) && metadata.width==32 && metadata.height==32 && metadata.frame_count>=2,
                  "MEDIA stream metadata");
            check(prepare(video,1) && prepare(video,0),"MEDIA stream seek backward");
            if(stream) {
                uint8_t samples[256];chunk=offset=0;
                audio_seek(video,0,&chunk,&offset);
                check(metadata.audio_available && audio_decode(video,&chunk,&offset,samples,sizeof(samples))>0,
                      "MEDIA MPEG streamed audio and seek");
            }
            bk_gui_desktop_t *desktop=bk_gui_desktop();
            bk_gui_window_t *w=bk_gui_create_window(desktop,80,80,100,100,"MEDIA render");
            bk_gui_surface_t *surface=0;
            check(w && bk_gui_window_begin_immediate_paint(w,&surface),"MEDIA rendering surface");
            bk_gui_rect_t r;
            check(bk_gui_window_content_rect(w,&r),"MEDIA content coordinates");
            r.w=r.h=32;bk_media_render_stats_t render_stats={0};
            check(draw_movie(surface,video,r,BK_MEDIA_QUALITY_EXACT,&render_stats) && render_stats.samples>0,
                  "MEDIA shared video renderer");
            bk_gui_window_end_immediate_paint(w,surface);bk_gui_destroy_window(desktop,w);
            close_video(video);
        }
        void *bkv=0;uint32_t bkv_size=0;
        check(bk_file_read_all("/SYSTEM/TESTS/NATIVE/DEMO.BKV",&bkv,&bkv_size),"MEDIA BKV fixture");
        video=open_video(bkv,bkv_size);
        check(video && prepare(video,0),"MEDIA BKV decode");
        close_video(video);bk_sys_free(bkv);
    }

    {
        LOAD(bk_print_job_t * (*)(const char *, const char *), begin, 9, "print_begin");
        LOAD(void (*)(bk_print_job_t *), cancel, 9, "print_cancel");
        bk_print_job_t *job = begin("LBS fixture", "PSFILE");
        check(job != 0, "PRINT job creation");
        cancel(job);
        LOAD(__typeof__(&print_text), text, 9, "print_text");
        LOAD(__typeof__(&print_submit), submit, 9, "print_submit");
        LOAD(__typeof__(&print_job_path), job_path, 9, "print_job_path");
        LOAD(__typeof__(&bk_print_job_status), status, 9, "bk_print_job_status");
        LOAD(__typeof__(&bk_print_job_cancel_queued), cancel_queued, 9,
             "bk_print_job_cancel_queued");
        char queue[40], again[40], state[400], hold[40];
        for (uint32_t canceled = 0; canceled < 2; canceled++) {
            job = begin("LBS PostScript fixture", "PSFILE");
            check(job && job_path(job, queue, sizeof(queue)), "PRINT stable job identifier");
            bk_sys_sleep_ms(20);
            check(job_path(job, again, sizeof(again)) && !strcmp(queue, again),
                  "PRINT path stable across timer ticks");
            if (canceled) {
                strcpy(hold, queue);
                strcpy(hold + strlen(hold) - 3, "HLD");
                check(bk_file_write_all(hold, "hold", 4), "PRINT hold queued fixture");
            }
            check(text(job, 20, 20, "LBS real spool test") && submit(job),
                  "PRINT submit real BPJ job");
            if (canceled)
                check(!cancel_queued(queue), "PRINT cancellation request");
            uint32_t wait = bk_uptime_ms();
            int result = -2;
            state[0] = 0;
            do {
                result = status(queue, state, sizeof(state));
                if (!result && (strstr(state, "state=done") || strstr(state, "state=canceled") ||
                                strstr(state, "state=error")))
                    break;
                bk_sys_sleep_ms(50);
            } while (bk_uptime_ms() - wait < 8000);
            check(!result && strstr(state, canceled ? "state=canceled" : "state=done"),
                  canceled ? "PRINT real cancellation acknowledged"
                           : "PRINT PostScript file job completed");
        }
        LOAD(bool (*)(void), ready, 9, "bk_print_spooler_is_ready");
        snprintf(line, sizeof(line), "[LBSTEST] spooler_ready=%u", ready());
        bk_sys_log(line);
    }
    {
        LOAD(uint32_t (*)(void), count, 8, "bk_font_count");
        check(count() > 0, "FONT enumeration");
    }
    {
        LOAD(uint32_t (*)(uint32_t), preference, 7, "bk_input_preference_get");
        check(preference(0) < 10000, "INPUT preferences");
    }
    dl_iterate_phdr(log_module, 0);
    /* Exercise both existing renderers; GL lookup follows backend selection. */
    {
        LOAD(__typeof__(&bk_ngs3d_create), create, 5, "bk_ngs3d_create");
        LOAD(__typeof__(&bk_ngs3d_destroy), destroy, 5, "bk_ngs3d_destroy");
        LOAD(__typeof__(&bk_ngs3d_present), present, 5, "bk_ngs3d_present");
        LOAD(__typeof__(&bk_ngs3d_pixels), pixels, 5, "bk_ngs3d_pixels");
        LOAD(__typeof__(&bk_ngs3d_get_proc), proc, 5, "bk_ngs3d_get_proc");
        LOAD(__typeof__(&bk_ngs3d_capabilities), caps, 5, "bk_ngs3d_capabilities");
        LOAD(__typeof__(&bk_ngs3d_vertices_create), vertices_create, 5, "bk_ngs3d_vertices_create");
        LOAD(__typeof__(&bk_ngs3d_triangles), triangles, 5, "bk_ngs3d_triangles");
        LOAD(__typeof__(&bk_ngs3d_stats), stats, 5, "bk_ngs3d_stats");
        static const uint32_t renderers[] = {BK_NGS3D_SOFTWARE, BK_NGS3D_AUTO,
                                             BK_NGS3D_TINYGL_SOFTWARE, BK_NGS3D_TINYGL_AUTO};
        static const float xyz[] = {-0.8f, -0.8f, 0, 0.8f, -0.8f, 0, 0, 0.8f, 0};
        for (i = 0; i < 4; i++) {
            uint32_t frame_start = bk_uptime_ms();
            bk_ngs3d_context_t *c = create(64, 64, renderers[i]);
            check(c != 0, "NGS3D selected backend context");
            bk_ngs3d_caps_t cp = {sizeof(cp), 0, 0, 0, 0, 0};
            bk_ngs3d_stats_t st = {sizeof(st), 0, 0, 0, 0, 0, 0};
            check(!caps(c, &cp), "NGS3D capabilities");
            __typeof__(&glClearColor) color = proc(c, "glClearColor");
            __typeof__(&glClear) clear = proc(c, "glClear");
            check(color && clear, "backend GL resolution");
            color(1, 0, 0, 1);
            clear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            check(!present(c, 0), "NGS3D clear/present");
            check((pixels(c)[32 * 64 + 32] & 0xffffff) == 0xff0000,
                  "NGS3D actual rendered red pixel");
            bk_ngs3d_vertices_t *v = vertices_create(c, xyz, 3);
            clear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            check(v != 0 && !triangles(c, v) && !present(c, 0), "NGS3D vertex buffer and triangle");
            check((pixels(c)[32 * 64 + 32] & 0xffffff) != 0xff0000,
                  "NGS3D actual rasterized triangle");
            __typeof__(&glGenTextures) gen_texture = proc(c, "glGenTextures");
            __typeof__(&glBindTexture) bind_texture = proc(c, "glBindTexture");
            __typeof__(&glTexImage2D) texture_image = proc(c, "glTexImage2D");
            __typeof__(&glTexParameteri) texture_parameter = proc(c, "glTexParameteri");
            __typeof__(&glEnable) enable = proc(c, "glEnable");
            __typeof__(&glDisable) disable = proc(c, "glDisable");
            __typeof__(&glDeleteTextures) delete_texture = proc(c, "glDeleteTextures");
            check(gen_texture && bind_texture && texture_image && texture_parameter && enable &&
                      disable && delete_texture,
                  "NGS3D texture entrypoints");
            uint32_t texture;
            uint8_t blue[4 * 4 * 3];
            for (uint32_t texel = 0; texel < 16; texel++) {
                blue[texel * 3] = 0;
                blue[texel * 3 + 1] = 0;
                blue[texel * 3 + 2] = 255;
            }
            gen_texture(1, &texture);
            bind_texture(GL_TEXTURE_2D, texture);
            texture_parameter(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            texture_parameter(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            texture_image(GL_TEXTURE_2D, 0, 3, 4, 4, 0, GL_RGB, GL_UNSIGNED_BYTE, blue);
            enable(GL_TEXTURE_2D);
            clear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            int draw_result = triangles(c, v), present_result = present(c, 0);
            uint32_t texture_pixel = pixels(c)[32 * 64 + 32] & 0xffffff;
            snprintf(line, sizeof(line), "[LBSTEST] texture_pixel=%x draw_rc=%d present_rc=%d\n",
                     texture_pixel, draw_result, present_result);
            bk_sys_log(line);
            /* TinyGL's fixed-point modulation can differ by one 8-bit level. */
            check(!draw_result && !present_result && (texture_pixel & 0xffff00) == 0 &&
                      (texture_pixel & 255) >= 254,
                  "NGS3D real sampled texture");
            if (renderers[i] == BK_NGS3D_SOFTWARE) {
                __typeof__(&glActiveTextureARB) active_texture = proc(c, "glActiveTextureARB");
                check(active_texture != 0, "Mesa multitexture entrypoint");
                active_texture(GL_TEXTURE1_ARB);
                bind_texture(GL_TEXTURE_2D, texture);
                enable(GL_TEXTURE_2D);
                active_texture(GL_TEXTURE0_ARB);
                enable(GL_POLYGON_SMOOTH);
                check(!triangles(c, v) && !present(c, 0) &&
                          (pixels(c)[32 * 64 + 32] & 0xffffff) == 0x0000ff,
                      "Mesa software antialiased multitexture fallback");
                disable(GL_POLYGON_SMOOTH);
                active_texture(GL_TEXTURE1_ARB);
                disable(GL_TEXTURE_2D);
                active_texture(GL_TEXTURE0_ARB);
            }
            disable(GL_TEXTURE_2D);
            delete_texture(1, &texture);
            if (renderers[i] < BK_NGS3D_TINYGL_SOFTWARE) {
                gen_texture(1, &texture);
                bind_texture(GL_TEXTURE_CUBE_MAP_ARB, texture);
                for (uint32_t face = 0; face < 6; face++)
                    texture_image(GL_TEXTURE_CUBE_MAP_POSITIVE_X_ARB + face, 0, 3, 4, 4, 0, GL_RGB,
                                  GL_UNSIGNED_BYTE, blue);
                delete_texture(1, &texture);
            }
            check(!stats(c, &st), "NGS3D backend statistics");
            snprintf(line, sizeof(line),
                     "[LBSTEST] renderer=%u active=%u gpu=%u driver=%s draws=%u triangles=%u "
                     "fallbacks=%u downloads=%u frame_ms=%u\n",
                     renderers[i], cp.active, cp.gpu_available, cp.renderer, st.draw_calls,
                     st.triangles, st.fallbacks, st.downloads, bk_uptime_ms() - frame_start);
            bk_sys_log(line);
            if (cp.gpu_available && cp.active == BK_NGS3D_GPU)
                check(st.draw_calls > 0 && st.triangles > 0, "real GPU draw submission");
            destroy(c); /* Also releases the owned vertex buffers and optional plugin. */
        }
    }

    {
        LOAD(bk_ngs_surface_t * (*)(uint32_t, uint32_t), create, 4, "bk_ngs_create");
        LOAD(void (*)(bk_ngs_surface_t *), destroy, 4, "bk_ngs_destroy");
        LOAD(int (*)(bk_ngs_surface_t *, bk_gui_rect_t, uint32_t), fill, 4, "bk_ngs_fill");
        LOAD(uint32_t *(*)(bk_ngs_surface_t *, uint32_t *), pixels, 4, "bk_ngs_pixels");
        bk_ngs_surface_t *s = create(32, 32);
        check(s != 0, "NGS surface");
        check(!fill(s, (bk_gui_rect_t){-4, -4, 12, 12}, 0xff123456) &&
                  pixels(s, 0)[0] == 0xff123456 && pixels(s, 0)[9 * 32 + 9] == 0,
              "NGS clipped fill");
        LOAD(__typeof__(&bk_ngs_blit), blit, 4, "bk_ngs_blit");
        LOAD(__typeof__(&bk_ngs_clip), clip, 4, "bk_ngs_clip");
        bk_ngs_surface_t *source = create(2, 2), *destination = create(4, 4);
        check(source && destination, "NGS alpha test buffers");
        fill(source, (bk_gui_rect_t){0, 0, 2, 2}, 0x80ff0000);
        fill(destination, (bk_gui_rect_t){0, 0, 4, 4}, 0xff0000ff);
        check(!clip(destination, (bk_gui_rect_t){1, 1, 2, 2}) &&
                  !blit(destination, (bk_gui_rect_t){0, 0, 4, 4}, source,
                        (bk_gui_rect_t){0, 0, 2, 2}) &&
                  pixels(destination, 0)[5] == 0xff80007f &&
                  pixels(destination, 0)[0] == 0xff0000ff,
              "NGS scaled alpha blit and clipping");
        destroy(source);
        destroy(destination);
        destroy(s);
    }
    {
        LOAD(bk_gui_desktop_t * (*)(void), desktop, 1, "bk_gui_desktop");
        LOAD(bk_gui_window_t * (*)(bk_gui_desktop_t *, int, int, int, int, const char *), create, 1,
             "bk_gui_create_window");
        LOAD(void (*)(bk_gui_desktop_t *, bk_gui_window_t *), destroy, 1, "bk_gui_destroy_window");
        LOAD(void (*)(bk_gui_window_t *, bk_gui_paint_t, void *), content, 1,
             "bk_gui_set_window_content");
        LOAD(void (*)(bk_gui_window_t *, bk_gui_event_handler_t, void *), events, 7,
             "bk_gui_set_window_event_handler");
        LOAD(void (*)(bk_gui_window_t *, bool), extended, 7, "bk_gui_window_set_extended_events");
        font_draw = (void *)symbol(8, "bk_font_draw_text");
        ngs_fill_native = symbol(4, "bk_gui_surface_fill_rect");
        LOAD(__typeof__(&bk_input_state_create), create_input, 7, "bk_input_state_create");
        LOAD(__typeof__(&bk_input_state_destroy), destroy_input, 7, "bk_input_state_destroy");
        feed_input = symbol(7, "bk_input_state_event");
        key_down = symbol(7, "bk_input_state_key");
        tracked_input = create_input();
        check(tracked_input != 0, "INPUT state context");
        bk_gui_desktop_t *d = desktop();
        bk_gui_window_t *w = create(d, 50, 50, 400, 250, "LBS platform test");
        check(w != 0, "VISION native window");
        content(w, paint, 0);
        events(w, input, 0);
        extended(w, true);
        bk_sys_log("[LBSTEST] INPUT READY\n");
        bk_sys_sleep_ms(3000);

        LOAD(__typeof__(&bk_ngs_create), create_frame, 4, "bk_ngs_create");
        LOAD(__typeof__(&bk_ngs_fill), fill_frame, 4, "bk_ngs_fill");
        LOAD(__typeof__(&bk_ngs_present), present_frame, 4, "bk_ngs_present");
        LOAD(__typeof__(&bk_ngs_destroy), destroy_frame, 4, "bk_ngs_destroy");
        bk_ngs_surface_t *frame = create_frame(400, 250);
        check(frame != 0 && !fill_frame(frame, (bk_gui_rect_t){0, 0, 400, 250}, 0xff204060) &&
                  !present_frame(frame, w),
              "NGS present to Vision window");
        started = bk_uptime_ms();
        for (int i = 0; i < 100; ++i)
            fill_frame(frame, (bk_gui_rect_t){0, 0, 400, 250}, 0xff204060);
        uint32_t fill_ms = bk_uptime_ms() - started;
        started = bk_uptime_ms();
        check(!present_frame(frame, w), "NGS repeated frame presentation");
        snprintf(line, sizeof(line), "[LBSTEST] ngs_fill_100_ms=%u ngs_present_ms=%u\n",
                 fill_ms, bk_uptime_ms() - started);
        bk_sys_log(line);
        bk_sys_log("[LBSTEST] NGS FRAME READY\n");
        bk_sys_sleep_ms(500);
        destroy_frame(frame);
        destroy(d, w);
        bk_input_state_t *input_to_destroy = tracked_input;
        tracked_input = 0;
        destroy_input(input_to_destroy);
        check(keys > 0 && mouse > 0 && key_state_verified > 0, "INPUT real events and key state");
        snprintf(line, sizeof(line),
                 "[LBSTEST] input_keys=%u input_mouse=%u key_state_verified=%u\n", keys, mouse,
                 key_state_verified);
        bk_sys_log(line);
    }
    /* Repeated load/unload must return to the same module count. */
    before = 0;
    dl_iterate_phdr(count_module, &before);
    started = bk_uptime_ms();
    for (i = 0; i < 100; i++) {
        void *h = bk_lbs_open("CRYPTO.LBS", 1);
        if (!h || dlclose(h))
            check(0, "repeat reference/release");
    }
    check(1, "100 repeated reference/release operations");
    after = 0;
    dl_iterate_phdr(count_module, &after);
    check(before == after, "no module reference leak");
    snprintf(line, sizeof(line), "[LBSTEST] reference_100_ms=%u\n", bk_uptime_ms() - started);
    bk_sys_log(line);
    for (i = 12; i; i--)
        check(!dlclose(modules[i - 1]), "unload");
    n = 0;
    dl_iterate_phdr(count_module, &n);
    check(n == 0, "all dependencies unloaded");
    {
        void *h = bk_lbs_open("NGS.LBS", 1);
        check(h != 0, "lifetime guard module");
        bk_ngs_surface_t *(*create)(uint32_t, uint32_t) = dlsym(h, "bk_ngs_create");
        void (*destroy)(bk_ngs_surface_t *) = dlsym(h, "bk_ngs_destroy");
        check(create && destroy, "lifetime guard exports");
        bk_ngs_surface_t *s = create(8, 8);
        check(s != 0, "lifetime guard object");
        check(dlclose(h) < 0, "live surface prevents unload");
        destroy(s);
        check(!dlclose(h), "unload after resource release");
    }
    {
        uint32_t before_bytes = own_memory(), load_start = bk_uptime_ms();
        bk_posix_heap_statistics_t heap_before = {sizeof(heap_before), 0, 0, 0, 0},
                                   heap_after = {sizeof(heap_after), 0, 0, 0, 0};
        check(!bk_posix_heap_statistics(&heap_before), "allocator baseline statistics");
        check(heap_before.live_bytes == 0 && heap_before.allocations == 0,
              "all demo allocator resources released");
        for (i = 0; i < 5; i++) {
            void *h = bk_lbs_open("NGS3D.LBS", 1);
            check(h != 0, "cold module cycle");
            __typeof__(&bk_ngs3d_create) create = dlsym(h, "bk_ngs3d_create");
            __typeof__(&bk_ngs3d_destroy) destroy = dlsym(h, "bk_ngs3d_destroy");
            check(create && destroy, "cold cycle entrypoints");
            bk_ngs3d_context_t *c = create(32, 32, BK_NGS3D_SOFTWARE);
            check(c != 0, "cold cycle context");
            destroy(c);
            check(!dlclose(h), "cold cycle unload");
        }
        bk_dl_statistics_t stats = {sizeof(stats), 0, 0, 0, 0};
        check(!bk_dl_statistics(&stats) && !stats.modules && !stats.image_bytes,
              "cold cycles release every image");
        check(!bk_posix_heap_statistics(&heap_after), "allocator final statistics");
        snprintf(line, sizeof(line),
                 "[LBSTEST] heap_live_before=%u heap_live_after=%u heap_arenas_before=%u "
                 "heap_arenas_after=%u allocations_before=%u allocations_after=%u\n",
                 heap_before.live_bytes, heap_after.live_bytes, heap_before.reserved_bytes,
                 heap_after.reserved_bytes, heap_before.allocations, heap_after.allocations);
        bk_sys_log(line);
        check(heap_after.live_bytes == heap_before.live_bytes &&
                  heap_after.allocations == heap_before.allocations,
              "no live allocator leak over cold cycles");
        snprintf(line, sizeof(line),
                 "[LBSTEST] cold_cycles_5_ms=%u process_before=%u process_after=%u\n",
                 bk_uptime_ms() - load_start, before_bytes, own_memory());
        bk_sys_log(line);
    }
    {
        int32_t pid = bk_spawn("/SYSTEM/TESTS/NATIVE/LBSSTART.BEX", 0);
        int32_t status;
        check(pid > 0 && bk_waitpid(pid, &status) >= 0 && status == 0,
              "declarative startup requirements");
    }
    {
        int32_t pid = bk_spawn("/SYSTEM/PROGRAMS/NETWORK_STATUS.BEX", 0);
        check(pid > 0, "migrated Network Status launch");
        bk_sys_sleep_ms(5000);
        check(bk_kill(pid) >= 0, "migrated app cleanup");
    }
    {
        static const char *tests[] = {"POSIX_TEST.BEX", "SDL_DEMO.BEX"};
        for (i = 0; i < 2; i++) {
            char name[128];
            int32_t status;
            snprintf(name, sizeof(name), "/SYSTEM/TESTS/NATIVE/%s", tests[i]);
            int32_t pid = bk_spawn(name, 0);
            check(pid > 0 && bk_waitpid(pid, &status) >= 0 && status == 0, tests[i]);
        }
    }
    {
        int32_t status;
        int32_t pid = bk_spawn("/SYSTEM/TESTS/NATIVE/LBSAPPS.BEX", 0);
        check(pid > 0 && bk_waitpid(pid, &status) >= 0 && status == 0,
              "native application startup regressions");
    }
    bk_sys_log("[LBSTEST] ALL PASS\n");
    bk_exit(0);
}
