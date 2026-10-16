/* Linux OSS playback bridge to the installed native PCM driver.
 * OSSv4 public ABI records; playback only, U8/S16_LE/FLOAT_LE, mono/stereo input.
 * Each open description owns a bounded queue. The worker mixes into native
 * 44100-Hz U8 mono; descriptors never submit unchecked user pointers to DMA. */
#define BK_OSS_QUEUE 16384U
#define BK_OSS_CHUNK 4410U
#define BK_OSS_U8 8U
#define BK_OSS_S16 16U
#define BK_OSS_FLOAT 0x4000U
static uint32_t linux_pcm_sample_bytes(uint32_t format) {
    return format==BK_OSS_FLOAT?4U:format==BK_OSS_S16?2U:1U;
}
/* Decode IEEE-754 without using kernel FPU state. Non-finite NaNs become
 * silence, infinities and out-of-range amplitudes saturate. */
static int32_t linux_pcm_float_sample(uint32_t bits) {
    uint32_t exponent=(bits>>23)&255U, fraction=bits&0x7fffffU;
    if(exponent==255U && fraction)return 0;
    if(exponent<120U)return 0;
    int32_t sample=exponent>=127U?128:(int32_t)((fraction|0x800000U)>>(143U-exponent));
    if(bits>>31)return -sample;
    return sample>127?127:sample;
}

typedef struct {
    char product[32],version[32]; int32_t versionnum; char options[128];
    int32_t numaudios,openedaudio[8],numsynths,nummidis,numtimers,nummixers;
    int32_t openedmidi[8],numcards,numaudioengines; char license[16],revision_info[256];
    int32_t filler[172];
} bk_oss_sysinfo;
typedef struct {
    int32_t dev; char name[64]; int32_t busy,pid,caps,iformats,oformats,magic;
    char cmd[64]; int32_t card_number,port_number,mixer_dev,legacy_device,enabled,flags;
    int32_t min_rate,max_rate,min_channels,max_channels,binding,rate_source;
    char handle[32]; uint32_t nrates,rates[20]; char song_name[64],label[16];
    int32_t latency; char devnode[32]; int32_t next_play_engine,next_rec_engine,filler[184];
} bk_oss_audioinfo;
_Static_assert(sizeof(bk_oss_sysinfo)==1248 && sizeof(bk_oss_audioinfo)==1180,"OSSv4 i386 ABI");
static volatile uint32_t linux_pcm_worker_started;

static void linux_pcm_worker(void *unused UNUSED) {
    uint8_t mixed[BK_OSS_CHUNK];
    uint32_t consumed[128], serial[128];
    unsigned active[128];
    for (;;) {
        sound_poll();
        if (!sound_pcm_is_busy()) {
            uint32_t lock=kspin_lock_irqsave(&linux_io_lock);
            uint32_t frames=0,active_count=0;
            kmemset(consumed,0,sizeof(consumed));
            for(unsigned i=0;i<128;++i) {
                linux_device *d=&linux_devices[i];
                d->pcm_inflight=0;
                if(d->refs && d->kind==4 && d->pcm && d->pcm_count) {
                    active[active_count++]=i;
                    serial[i]=d->pcm_serial;
                    if(d->pcm_count>frames)frames=d->pcm_count;
                }
            }
            if(frames>BK_OSS_CHUNK)frames=BK_OSS_CHUNK;
            for(uint32_t n=0;n<frames;++n) {
                int32_t sum=0;
                for(unsigned a=0;a<active_count;++a) {
                    linux_device *d=&linux_devices[active[a]];
                    if(n<d->pcm_count)
                        sum+=(int32_t)d->pcm[(d->pcm_read+n)&(BK_OSS_QUEUE-1)]-128;
                }
                if(sum>127)sum=127;
                if(sum< -128)sum=-128;
                mixed[n]=(uint8_t)(sum+128);
            }
            for(unsigned a=0;a<active_count;++a) {
                unsigned i=active[a];
                consumed[i]=linux_devices[i].pcm_count<frames?linux_devices[i].pcm_count:frames;
            }
            kspin_unlock_irqrestore(&linux_io_lock,lock);
            if(frames && sound_play_pcm_u8(mixed,frames,44100,255)) {
                lock=kspin_lock_irqsave(&linux_io_lock);
                for(unsigned i=0;i<128;++i) {
                    linux_device *d=&linux_devices[i];
                    if(!consumed[i] || !d->refs || d->kind!=4 || !d->pcm || d->pcm_serial!=serial[i])continue;
                    uint32_t n=consumed[i];if(n>d->pcm_count)n=d->pcm_count;
                    d->pcm_read=(d->pcm_read+n)&(BK_OSS_QUEUE-1);d->pcm_count-=n;d->pcm_inflight=n;
                }
                kspin_unlock_irqrestore(&linux_io_lock,lock);
            }
        }
        task_sleep(1);
    }
}

static int32_t linux_pcm_write(linux_device *d,const uint8_t *data,uint32_t bytes) {
    if(!d->pcm || !sound_pcm_available())return -19;
    uint32_t block=linux_pcm_sample_bytes(d->pcm_format)*d->pcm_channels;
    if(bytes%block)return -BK_EINVAL;
    uint32_t lock=kspin_lock_irqsave(&linux_io_lock);
    uint32_t available=BK_OSS_QUEUE-d->pcm_count-d->pcm_inflight;
    uint32_t frames=bytes/block;
    uint32_t max_input=(available*d->pcm_rate)/44100U;
    if(frames>max_input)frames=max_input;
    if(!frames) {kspin_unlock_irqrestore(&linux_io_lock,lock);return bytes?-BK_EAGAIN:0;}
    uint32_t output=(frames*44100U+d->pcm_rate-1)/d->pcm_rate;
    if(output>available)output=available;
    for(uint32_t n=0;n<output;++n) {
        uint32_t f=(n*d->pcm_rate)/44100U;
        int32_t sum=0;
        for(uint32_t ch=0;ch<d->pcm_channels;++ch) {
            uint32_t index=f*block+ch*linux_pcm_sample_bytes(d->pcm_format);
            if(d->pcm_format==BK_OSS_S16) {
                int16_t sample=(int16_t)(data[index]|(uint16_t)data[index+1]<<8);
                sum+=sample/256;
            } else if(d->pcm_format==BK_OSS_FLOAT) {
                uint32_t bits=(uint32_t)data[index]|(uint32_t)data[index+1]<<8|
                              (uint32_t)data[index+2]<<16|(uint32_t)data[index+3]<<24;
                sum+=linux_pcm_float_sample(bits);
            } else sum+=(int32_t)data[index]-128;
        }
        d->pcm[(d->pcm_read+d->pcm_count+n)&(BK_OSS_QUEUE-1)]=(uint8_t)(128+sum/(int32_t)d->pcm_channels);
    }
    d->pcm_count+=output;
    kspin_unlock_irqrestore(&linux_io_lock,lock);return frames*block;
}

static int32_t linux_oss_ioctl(linux_device *d,uint32_t request,uint32_t argument) {
    unsigned type=(request>>8)&255U,nr=request&255U;
    if(type=='X' && nr==1) {
        if(request!=0x84e05801U)return -BK_ENOTTY;
        bk_oss_sysinfo info;kmemset(&info,0,sizeof(info));
        kstrncpy(info.product,"BlesKernOS PCM",sizeof(info.product)-1);
        kstrncpy(info.version,"4.0",sizeof(info.version)-1);info.versionnum=0x040000;
        info.numaudios=sound_pcm_available()?1:0;info.numaudioengines=info.numaudios;info.numcards=info.numaudios;
        return copy_to_user((void *)argument,&info,sizeof(info))?0:-BK_EFAULT;
    }
    if(type=='X' && (nr==7 || nr==12)) {
        if(request!=(nr==7?0xc49c5807U:0xc49c580cU))return -BK_ENOTTY;
        bk_oss_audioinfo info;int32_t dev;
        if(!copy_from_user(&dev,(void *)argument,4))return -BK_EFAULT;
        if(dev!=0 && !(dev==-1 && d->kind==4))return -BK_EINVAL;
        if(!sound_pcm_available())return -19;
        kmemset(&info,0,sizeof(info));
        kstrncpy(info.name,sound_pcm_name(),sizeof(info.name)-1);
        kstrncpy(info.devnode,"/dev/dsp",sizeof(info.devnode)-1);
        info.caps=0x00020000;info.oformats=BK_OSS_U8|BK_OSS_S16|BK_OSS_FLOAT;info.enabled=1;
        info.min_rate=8000;info.max_rate=48000;info.min_channels=1;info.max_channels=2;
        info.next_play_engine=info.next_rec_engine=-1;info.latency=100000;
        return copy_to_user((void *)argument,&info,sizeof(info))?0:-BK_EFAULT;
    }
    if(type!='P' || d->kind!=4)return -BK_ENOTTY;
    if(nr==0) {
        uint32_t lock=kspin_lock_irqsave(&linux_io_lock);
        d->pcm_read=d->pcm_count=0; ++d->pcm_serial;
        kspin_unlock_irqrestore(&linux_io_lock,lock);return 0;
    }
    if(nr==12) {
        if(request!=0x8010500cU)return -BK_ENOTTY;
        uint32_t lock=kspin_lock_irqsave(&linux_io_lock);
        uint32_t block=linux_pcm_sample_bytes(d->pcm_format)*d->pcm_channels;
        uint32_t total=(BK_OSS_QUEUE*d->pcm_rate/44100U)*block;
        uint32_t free=(BK_OSS_QUEUE-d->pcm_count-d->pcm_inflight)*d->pcm_rate/44100U*block;
        uint32_t fragment=(d->pcm_rate/100U)*block;
        uint32_t info[4]={free/fragment,total/fragment,fragment,free};
        kspin_unlock_irqrestore(&linux_io_lock,lock);
        return copy_to_user((void *)argument,info,sizeof(info))?0:-BK_EFAULT;
    }
    int32_t value;
    if(!copy_from_user(&value,(void *)argument,4))return -BK_EFAULT;
    uint32_t lock=kspin_lock_irqsave(&linux_io_lock);
    int32_t result=0;
    if(nr==2 || nr==5 || nr==6) {
        if(d->pcm_count || d->pcm_inflight)result=-BK_EBUSY;
        else if(nr==2 && value>=8000 && value<=48000)d->pcm_rate=value;
        else if(nr==5 && (value==BK_OSS_U8 || value==BK_OSS_S16 || value==BK_OSS_FLOAT))d->pcm_format=value;
        else if(nr==6 && (value==1 || value==2))d->pcm_channels=value;
        else if(value)result=-BK_EINVAL;
        if(nr==2)value=d->pcm_rate;
        else if(nr==5)value=d->pcm_format;
        else value=d->pcm_channels;
    } else if(nr==4)value=(d->pcm_rate/100U)*linux_pcm_sample_bytes(d->pcm_format)*d->pcm_channels;
    else if(nr==11)value=BK_OSS_U8|BK_OSS_S16|BK_OSS_FLOAT;
    else result=-BK_ENOTTY;
    kspin_unlock_irqrestore(&linux_io_lock,lock);
    if(result)return result;
    return copy_to_user((void *)argument,&value,4)?0:-BK_EFAULT;
}
