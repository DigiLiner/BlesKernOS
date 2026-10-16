#include "bleskernos.h"
#include <errno.h>
#include <ctype.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct __blesk_posix_FILE {
    int fd;
    unsigned char dynamic;
    unsigned char eof;
    unsigned char error;
};

static FILE standard_input = {0, 0, 0, 0};
static FILE standard_output = {1, 0, 0, 0};
static FILE standard_error = {2, 0, 0, 0};
FILE *stdin = &standard_input;
FILE *stdout = &standard_output;
FILE *stderr = &standard_error;

FILE *fopen(const char *path, const char *mode) {
    FILE *stream;
    int flags;
    if (!path || !mode || !mode[0]) { errno = EINVAL; return 0; }
    if (mode[0] == 'r') flags = strchr(mode, '+') ? O_RDWR : O_RDONLY;
    else if (mode[0] == 'w') flags = (strchr(mode, '+') ? O_RDWR : O_WRONLY) |
                                     O_CREAT | O_TRUNC;
    else if (mode[0] == 'a') flags = (strchr(mode, '+') ? O_RDWR : O_WRONLY) |
                                     O_CREAT | O_APPEND;
    else { errno = EINVAL; return 0; }
    stream = (FILE *)malloc(sizeof(*stream));
    if (!stream) return 0;
    stream->fd = open(path, flags, 0666);
    if (stream->fd < 0) { free(stream); return 0; }
    stream->dynamic = 1;
    stream->eof = stream->error = 0;
    return stream;
}

FILE *freopen(const char *path, const char *mode, FILE *stream) {
    FILE *replacement;
    int old_fd;
    if (!stream) { errno = EBADF; return 0; }
    replacement = fopen(path, mode);
    if (!replacement) return 0;
    old_fd = stream->fd;
    stream->fd = replacement->fd;
    stream->eof = stream->error = 0;
    replacement->dynamic = 0;
    free(replacement);
    if (old_fd > STDERR_FILENO) close(old_fd);
    return stream;
}

int fclose(FILE *stream) {
    int result;
    if (!stream || !stream->dynamic) { errno = EBADF; return EOF; }
    result = close(stream->fd);
    free(stream);
    return result < 0 ? EOF : 0;
}

size_t fread(void *buffer, size_t size, size_t count, FILE *stream) {
    ssize_t result;
    size_t bytes;
    if (!stream || (!buffer && size && count)) { errno = EFAULT; return 0; }
    if (!size || !count) return 0;
    if (count > 0xFFFFFFFFU / size) { errno = EINVAL; stream->error = 1; return 0; }
    bytes = size * count;
    result = read(stream->fd, buffer, bytes);
    if (result < 0) { stream->error = 1; return 0; }
    if ((size_t)result < bytes) stream->eof = 1;
    return (size_t)result / size;
}

size_t fwrite(const void *buffer, size_t size, size_t count, FILE *stream) {
    ssize_t result;
    size_t bytes;
    if (!stream || (!buffer && size && count)) { errno = EFAULT; return 0; }
    if (!size || !count) return 0;
    if (count > 0xFFFFFFFFU / size) { errno = EINVAL; stream->error = 1; return 0; }
    bytes = size * count;
    result = write(stream->fd, buffer, bytes);
    if (result < 0) { stream->error = 1; return 0; }
    return (size_t)result / size;
}

int fflush(FILE *stream) {
    if (!stream) return 0;
    if (stream->fd <= STDERR_FILENO) return 0;
    if (fsync(stream->fd) < 0) { stream->error = 1; return EOF; }
    return 0;
}

int fseek(FILE *stream, long offset, int whence) {
    if (!stream || lseek(stream->fd, (off_t)offset, whence) < 0) {
        if (stream) stream->error = 1;
        return -1;
    }
    stream->eof = 0;
    return 0;
}

long ftell(FILE *stream) {
    if (!stream) { errno = EBADF; return -1L; }
    return (long)lseek(stream->fd, 0, SEEK_CUR);
}

int feof(FILE *stream) { return stream ? stream->eof : 0; }
int ferror(FILE *stream) { return stream ? stream->error : 0; }
int fileno(FILE *stream) {
    if (!stream) { errno = EBADF; return -1; }
    return stream->fd;
}
FILE *fdopen(int fd, const char *mode) {
    FILE *stream;
    if (fd < 0 || !mode || !*mode) { errno=EINVAL; return 0; }
    stream=(FILE*)malloc(sizeof(*stream));
    if(!stream)return 0;
    stream->fd=fd; stream->dynamic=1; stream->eof=stream->error=0;
    return stream;
}
char *tmpnam(char *buffer){static char internal[32];char*name=buffer?buffer:internal;static unsigned int sequence;snprintf(name,32,"/TEMP/P%uT%u.TMP",(unsigned int)getpid(),sequence++);return name;}
FILE *tmpfile(void){char name[32];FILE*stream;tmpnam(name);stream=fopen(name,"w+b");return stream;}
int fgetc(FILE *stream){unsigned char c;if(!stream){errno=EBADF;return EOF;}ssize_t n=read(stream->fd,&c,1);if(n==1)return c;if(n==0)stream->eof=1;else stream->error=1;return EOF;}
int getc(FILE *stream){return fgetc(stream);} int getchar(void){return fgetc(stdin);}
int fputc(int c,FILE *stream){unsigned char value=(unsigned char)c;if(!stream||write(stream->fd,&value,1)!=1){if(stream)stream->error=1;return EOF;}return value;}
int putc(int c,FILE *stream){return fputc(c,stream);}
char *fgets(char *buffer,int capacity,FILE *stream){int c,count=0;if(!buffer||capacity<=0||!stream){errno=EINVAL;return 0;}while(count+1<capacity&&(c=fgetc(stream))!=EOF){buffer[count++]=(char)c;if(c=='\n')break;}if(!count)return 0;buffer[count]=0;return buffer;}
int fputs(const char *text,FILE *stream){size_t n;if(!text||!stream){errno=EINVAL;return EOF;}n=strlen(text);return fwrite(text,1,n,stream)==n?0:EOF;}
void clearerr(FILE *stream){if(stream)stream->eof=stream->error=0;}
void rewind(FILE *stream){if(stream){(void)fseek(stream,0,SEEK_SET);clearerr(stream);}}
void perror(const char *prefix){const char*message=strerror(errno);if(prefix&&*prefix){fputs(prefix,stderr);fputs(": ",stderr);}fputs(message,stderr);fputc('\n',stderr);}
int setvbuf(FILE *stream,char *buffer,int mode,size_t size){(void)buffer;(void)size;if(!stream||mode!=_IONBF){errno=EINVAL;return-1;}return 0;}
int remove(const char *path) { return unlink(path); }
int rename(const char *old_path, const char *new_path) {
    bk_i32 result = bk_rename(old_path, new_path);
    if (result < 0) { errno = -result; return -1; }
    return 0;
}

typedef struct {
    char *buffer;
    size_t capacity;
    size_t length;
} format_output_t;

static void format_character(format_output_t *output, char value) {
    if (output->capacity && output->length + 1U < output->capacity)
        output->buffer[output->length] = value;
    output->length++;
}

static void format_repeat(format_output_t *output, char value, int count) {
    while (count-- > 0) format_character(output, value);
}

static int format_text_length(const char *text, int precision) {
    int length = 0;
    if (!text) text = "(null)";
    while (text[length] && (precision < 0 || length < precision)) length++;
    return length;
}

static void format_text_width(format_output_t *output, const char *text,
                              int width, int precision, int left_align) {
    int length;
    int padding;
    if (!text) text = "(null)";
    length = format_text_length(text, precision);
    padding = width > length ? width - length : 0;
    if (!left_align) format_repeat(output, ' ', padding);
    for (int i = 0; i < length; i++) format_character(output, text[i]);
    if (left_align) format_repeat(output, ' ', padding);
}

static int format_unsigned_text(unsigned long value, unsigned int base,
                                int upper, char *buffer, int capacity) {
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    int count = 0;
    if (capacity <= 0) return 0;
    do {
        buffer[count++] = digits[value % base];
        value /= base;
    } while (value && count < capacity);
    for (int i = 0; i < count / 2; i++) {
        char temporary = buffer[i];
        buffer[i] = buffer[count - 1 - i];
        buffer[count - 1 - i] = temporary;
    }
    return count;
}

static void format_integer(format_output_t *output, unsigned long value,
                           int negative, unsigned int base, int upper,
                           int width, int precision, int left_align,
                           int zero_pad, int force_sign, int space_sign,
                           const char *prefix) {
    char digits[32];
    int digit_count;
    int prefix_length = prefix ? (int)strlen(prefix) : 0;
    int sign_length = negative || force_sign || space_sign ? 1 : 0;
    int zeros = 0;
    int padding;

    digit_count = format_unsigned_text(value, base, upper, digits,
                                       (int)sizeof(digits));
    if (precision == 0 && value == 0U) digit_count = 0;
    if (precision > digit_count) zeros = precision - digit_count;
    if (zero_pad && !left_align && precision < 0) {
        int content = sign_length + prefix_length + digit_count;
        if (width > content) zeros += width - content;
    }
    padding = width - (sign_length + prefix_length + zeros + digit_count);
    if (padding < 0) padding = 0;

    if (!left_align) format_repeat(output, ' ', padding);
    if (negative) format_character(output, '-');
    else if (force_sign) format_character(output, '+');
    else if (space_sign) format_character(output, ' ');
    if (prefix) while (*prefix) format_character(output, *prefix++);
    format_repeat(output, '0', zeros);
    for (int i = 0; i < digit_count; i++) format_character(output, digits[i]);
    if (left_align) format_repeat(output, ' ', padding);
}

static int format_float_text(double value, int precision, int trim_zeroes,
                             char *text, int capacity) {
    unsigned long integer;
    unsigned long fraction;
    unsigned long scale = 1U;
    char integer_text[32];
    char fraction_text[16];
    int negative = 0;
    int integer_length;
    int position = 0;

    if (capacity <= 1) return 0;
    if (precision < 0) precision = 6;
    if (precision > 9) precision = 9;
    if (value < 0.0) { negative = 1; value = -value; }

    integer = (unsigned long)value;
    for (int i = 0; i < precision; i++) scale *= 10U;
    fraction = precision > 0
             ? (unsigned long)((value - (double)integer) * (double)scale + 0.5)
             : (unsigned long)((value - (double)integer) + 0.5);
    if (precision == 0) {
        if (fraction) integer++;
        fraction = 0U;
    } else if (fraction >= scale) {
        integer++;
        fraction -= scale;
    }

    integer_length = format_unsigned_text(integer, 10U, 0, integer_text,
                                          (int)sizeof(integer_text));
    if (negative && position + 1 < capacity) text[position++] = '-';
    for (int i = 0; i < integer_length && position + 1 < capacity; i++)
        text[position++] = integer_text[i];

    if (precision > 0 && position + 1 < capacity) {
        int digits = precision;
        for (int i = precision - 1; i >= 0; i--) {
            fraction_text[i] = (char)('0' + fraction % 10U);
            fraction /= 10U;
        }
        if (trim_zeroes) {
            while (digits > 0 && fraction_text[digits - 1] == '0') digits--;
        }
        if (digits > 0) {
            text[position++] = '.';
            for (int i = 0; i < digits && position + 1 < capacity; i++)
                text[position++] = fraction_text[i];
        }
    }
    text[position] = '\0';
    return position;
}

static void format_float(format_output_t *output, double value, int width,
                         int precision, int left_align, int zero_pad,
                         int force_sign, int space_sign, int trim_zeroes) {
    char text[96];
    char signed_text[96];
    int length;
    int padding;
    int negative = value < 0.0;

    length = format_float_text(value, precision, trim_zeroes, text,
                               (int)sizeof(text));
    if (!negative && (force_sign || space_sign) && length + 1 < (int)sizeof(signed_text)) {
        signed_text[0] = force_sign ? '+' : ' ';
        memcpy(signed_text + 1, text, (size_t)length + 1U);
        memcpy(text, signed_text, (size_t)length + 2U);
        length++;
    }
    padding = width > length ? width - length : 0;
    if (!left_align && !zero_pad) format_repeat(output, ' ', padding);
    if (!left_align && zero_pad && padding > 0 &&
        (text[0] == '-' || text[0] == '+' || text[0] == ' ')) {
        format_character(output, text[0]);
        format_repeat(output, '0', padding);
        for (int i = 1; i < length; i++) format_character(output, text[i]);
    } else {
        if (!left_align && zero_pad) format_repeat(output, '0', padding);
        for (int i = 0; i < length; i++) format_character(output, text[i]);
    }
    if (left_align) format_repeat(output, ' ', padding);
}

int vsnprintf(char *buffer, size_t size, const char *format,
              va_list arguments) {
    format_output_t output = {buffer, size, 0U};
    if (!format || (!buffer && size)) { errno = EINVAL; return -1; }

    while (*format) {
        int left_align = 0;
        int zero_pad = 0;
        int force_sign = 0;
        int space_sign = 0;
        int alternate = 0;
        int width = 0;
        int precision = -1;
        int length_l = 0;
        int length_h = 0;
        char conversion;

        if (*format != '%') {
            format_character(&output, *format++);
            continue;
        }
        format++;
        if (*format == '%') {
            format_character(&output, '%');
            format++;
            continue;
        }

        for (;;) {
            if (*format == '-') left_align = 1;
            else if (*format == '0') zero_pad = 1;
            else if (*format == '+') force_sign = 1;
            else if (*format == ' ') space_sign = 1;
            else if (*format == '#') alternate = 1;
            else break;
            format++;
        }

        if (*format == '*') {
            width = va_arg(arguments, int);
            if (width < 0) { left_align = 1; width = -width; }
            format++;
        } else {
            while (*format >= '0' && *format <= '9') {
                width = width * 10 + (*format - '0');
                format++;
            }
        }

        if (*format == '.') {
            precision = 0;
            format++;
            if (*format == '*') {
                precision = va_arg(arguments, int);
                if (precision < 0) precision = -1;
                format++;
            } else {
                while (*format >= '0' && *format <= '9') {
                    precision = precision * 10 + (*format - '0');
                    format++;
                }
            }
        }

        while (*format == 'h') { length_h++; format++; }
        while (*format == 'l') { length_l++; format++; }
        if (*format == 'z' || *format == 't' || *format == 'j') {
            length_l = 1;
            format++;
        }

        conversion = *format;
        if (!conversion) break;
        format++;

        switch (conversion) {
        case 'c': {
            char text[2] = {(char)va_arg(arguments, int), '\0'};
            format_text_width(&output, text, width, 1, left_align);
            break;
        }
        case 's':
            format_text_width(&output, va_arg(arguments, const char *),
                              width, precision, left_align);
            break;
        case 'd':
        case 'i': {
            long signed_value;
            unsigned long value;
            int negative;
            if (length_l >= 2) signed_value = (long)va_arg(arguments, long long);
            else if (length_l) signed_value = va_arg(arguments, long);
            else signed_value = (long)va_arg(arguments, int);
            negative = signed_value < 0;
            value = negative ? 0UL - (unsigned long)signed_value
                             : (unsigned long)signed_value;
            format_integer(&output, value, negative, 10U, 0, width, precision,
                           left_align, zero_pad, force_sign, space_sign, NULL);
            break;
        }
        case 'u':
        case 'x':
        case 'X':
        case 'o': {
            unsigned long value;
            unsigned int base = conversion == 'o' ? 8U
                              : (conversion == 'u' ? 10U : 16U);
            const char *prefix = NULL;
            if (length_l >= 2) value = (unsigned long)va_arg(arguments, unsigned long long);
            else if (length_l) value = va_arg(arguments, unsigned long);
            else value = (unsigned long)va_arg(arguments, unsigned int);
            if (alternate && value) {
                if (conversion == 'x') prefix = "0x";
                else if (conversion == 'X') prefix = "0X";
                else if (conversion == 'o') prefix = "0";
            }
            format_integer(&output, value, 0, base, conversion == 'X', width,
                           precision, left_align, zero_pad, 0, 0, prefix);
            break;
        }
        case 'p': {
            unsigned long value = (unsigned long)(uintptr_t)va_arg(arguments, void *);
            format_integer(&output, value, 0, 16U, 0, width ? width : 10,
                           precision, left_align, 1, 0, 0, "0x");
            break;
        }
        case 'f':
        case 'F':
            format_float(&output, va_arg(arguments, double), width, precision,
                         left_align, zero_pad, force_sign, space_sign, 0);
            break;
        case 'g':
        case 'G':
            format_float(&output, va_arg(arguments, double), width,
                         precision < 0 ? 6 : precision,
                         left_align, zero_pad, force_sign, space_sign, 1);
            break;
        case 'n': {
            if (length_l >= 2) *va_arg(arguments, long long *) = (long long)output.length;
            else if (length_l) *va_arg(arguments, long *) = (long)output.length;
            else if (length_h >= 2) *va_arg(arguments, signed char *) = (signed char)output.length;
            else if (length_h) *va_arg(arguments, short *) = (short)output.length;
            else *va_arg(arguments, int *) = (int)output.length;
            break;
        }
        default:
            format_character(&output, '%');
            format_character(&output, conversion);
            break;
        }
    }

    if (size) buffer[output.length < size ? output.length : size - 1U] = '\0';
    return (int)output.length;
}

int vsprintf(char *buffer, const char *format, va_list arguments) {
    return vsnprintf(buffer, (size_t)-1, format, arguments);
}

int sprintf(char *buffer, const char *format, ...) {
    va_list arguments;
    int result;
    va_start(arguments, format);
    result = vsprintf(buffer, format, arguments);
    va_end(arguments);
    return result;
}

int snprintf(char *buffer, size_t size, const char *format, ...) {
    int result;
    va_list arguments;
    va_start(arguments, format);
    result = vsnprintf(buffer, size, format, arguments);
    va_end(arguments);
    return result;
}

int vfprintf(FILE *stream, const char *format, va_list arguments) {
    char buffer[1024];
    int result = vsnprintf(buffer, sizeof(buffer), format, arguments);
    size_t available;
    if (result < 0) return result;
    available = (size_t)result < sizeof(buffer) ? (size_t)result
                                                : sizeof(buffer) - 1U;
    if (fwrite(buffer, 1U, available, stream) != available) return -1;
    return result;
}

int fprintf(FILE *stream, const char *format, ...) {
    int result;
    va_list arguments;
    va_start(arguments, format);
    result = vfprintf(stream, format, arguments);
    va_end(arguments);
    return result;
}

int printf(const char *format, ...) {
    int result;
    va_list arguments;
    va_start(arguments, format);
    result = vfprintf(stdout, format, arguments);
    va_end(arguments);
    return result;
}

int putchar(int character) {
    unsigned char value = (unsigned char)character;
    return write(STDOUT_FILENO, &value, 1U) == 1 ? character : EOF;
}

int puts(const char *text) {
    size_t length = strlen(text);
    if (write(STDOUT_FILENO, text, length) != (ssize_t)length ||
        write(STDOUT_FILENO, "\n", 1U) != 1) return EOF;
    return 0;
}

static int scan_digit(int character,int base){if(character>='0'&&character<='9')character-='0';else if(character>='a'&&character<='f')character=character-'a'+10;else if(character>='A'&&character<='F')character=character-'A'+10;else return-1;return character<base?character:-1;}
int vsscanf(const char *text,const char *format,va_list arguments){
    int assigned=0;
    if(!text||!format){errno=EINVAL;return EOF;}
    while(*format){
        if(isspace((unsigned char)*format)){while(isspace((unsigned char)*format))format++;while(isspace((unsigned char)*text))text++;continue;}
        if(*format!='%'){if(*text!=*format)return assigned;format++;text++;continue;}
        format++;if(*format=='%'){if(*text!='%')return assigned;format++;text++;continue;}
        int suppress=0,width=0,long_value=0;if(*format=='*'){suppress=1;format++;}
        while(*format>='0'&&*format<='9'){width=width*10+(*format++-'0');}
        if(*format=='l'){long_value=1;format++;}
        if(*format!='c'&&*format!='[')while(isspace((unsigned char)*text))text++;
        if(*format=='s'){
            const char*start=text;while(*text&&!isspace((unsigned char)*text)&&(!width||text-start<width))text++;
            if(text==start)return assigned;
            if(!suppress){char*out=va_arg(arguments,char*);while(start<text)*out++=*start++;*out=0;assigned++;}
        }else if(*format=='c'){
            int count=width?width:1;if(!*text)return assigned;if(!suppress){char*out=va_arg(arguments,char*);for(int i=0;i<count&&*text;i++)*out++=*text++;assigned++;}else while(count--&&*text)text++;
        }else if(*format=='d'||*format=='i'||*format=='u'||*format=='x'||*format=='X'||*format=='o'){
            int base=(*format=='x'||*format=='X')?16:(*format=='o'?8:10),negative=0,digits=0;unsigned long value=0;const char*start=text;
            if((*format=='d'||*format=='i')&&(*text=='+'||*text=='-')){negative=*text=='-';text++;}
            while(*text&&(!width||text-start<width)){int d=scan_digit((unsigned char)*text,base);if(d<0)break;value=value*(unsigned int)base+(unsigned int)d;text++;digits++;}
            if(!digits)return assigned;
            if(!suppress){if(long_value){long*out=va_arg(arguments,long*);*out=negative?-(long)value:(long)value;}else{int*out=va_arg(arguments,int*);*out=negative?-(int)value:(int)value;}assigned++;}
        }else return assigned;
        format++;
    }
    return assigned;
}
int sscanf(const char *text,const char *format,...){int result;va_list arguments;va_start(arguments,format);result=vsscanf(text,format,arguments);va_end(arguments);return result;}

int vfscanf(FILE *stream, const char *format, va_list arguments) {
    char line[1024];

    if (!stream || !format) { errno = EINVAL; return EOF; }
    if (!fgets(line, (int)sizeof(line), stream)) return EOF;
    return vsscanf(line, format, arguments);
}

int fscanf(FILE *stream, const char *format, ...) {
    int result;
    va_list arguments;
    va_start(arguments, format);
    result = vfscanf(stream, format, arguments);
    va_end(arguments);
    return result;
}

int vscanf(const char *format, va_list arguments) {
    return vfscanf(stdin, format, arguments);
}

int scanf(const char *format, ...) {
    int result;
    va_list arguments;
    va_start(arguments, format);
    result = vscanf(format, arguments);
    va_end(arguments);
    return result;
}
int vasprintf(char **text,const char *format,va_list arguments){va_list copy;int length;if(!text){errno=EINVAL;return-1;}va_copy(copy,arguments);length=vsnprintf(0,0,format,copy);va_end(copy);if(length<0){*text=0;return-1;}*text=(char*)malloc((size_t)length+1U);if(!*text)return-1;return vsnprintf(*text,(size_t)length+1U,format,arguments);}
int asprintf(char **text,const char *format,...){int result;va_list arguments;va_start(arguments,format);result=vasprintf(text,format,arguments);va_end(arguments);return result;}
int getdelim(char **line,size_t *capacity,int delimiter,FILE *stream){size_t used=0;if(!line||!capacity||!stream){errno=EINVAL;return-1;}if(!*line||!*capacity){*capacity=128U;*line=(char*)malloc(*capacity);if(!*line)return-1;}for(;;){int character=fgetc(stream);if(character==EOF){if(!used)return-1;break;}if(used+1U>=*capacity){size_t next=*capacity<=0x7FFFFFFFU?*capacity*2U:0U;char*grown;if(!next){errno=ENOMEM;return-1;}grown=(char*)realloc(*line,next);if(!grown)return-1;*line=grown;*capacity=next;}(*line)[used++]=(char)character;if(character==delimiter)break;}(*line)[used]=0;return(int)used;}
int getline(char **line,size_t *capacity,FILE *stream){return getdelim(line,capacity,'\n',stream);}
