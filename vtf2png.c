#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <png.h>
#include <jpeglib.h>
#include <sys/mman.h>
#include <argp.h>

enum {
  IMAGE_FORMAT_NONE = -1,
  IMAGE_FORMAT_RGBA8888 = 0,
  IMAGE_FORMAT_ABGR8888,
  IMAGE_FORMAT_RGB888,
  IMAGE_FORMAT_BGR888,
  IMAGE_FORMAT_RGB565,
  IMAGE_FORMAT_I8,
  IMAGE_FORMAT_IA88,
  IMAGE_FORMAT_P8,
  IMAGE_FORMAT_A8,
  IMAGE_FORMAT_RGB888_BLUESCREEN,
  IMAGE_FORMAT_BGR888_BLUESCREEN,
  IMAGE_FORMAT_ARGB8888,
  IMAGE_FORMAT_BGRA8888,
  IMAGE_FORMAT_DXT1,
  IMAGE_FORMAT_DXT3,
  IMAGE_FORMAT_DXT5,
  IMAGE_FORMAT_BGRX8888,
  IMAGE_FORMAT_BGR565,
  IMAGE_FORMAT_BGRX5551,
  IMAGE_FORMAT_BGRA4444,
  IMAGE_FORMAT_DXT1_ONEBITALPHA,
  IMAGE_FORMAT_BGRA5551,
  IMAGE_FORMAT_UV88,
  IMAGE_FORMAT_UVWQ8888,
  IMAGE_FORMAT_RGBA16161616F,
  IMAGE_FORMAT_RGBA16161616,
  IMAGE_FORMAT_UVLX8888
};

#pragma pack(1)
typedef struct {
  char           signature[4];
  unsigned int   version[2];
  unsigned int   header_size;
  unsigned short width;
  unsigned short height;
  unsigned int   flags;
  unsigned short frames;
  unsigned short first_frame;
  unsigned char  padding0[4];
  float          reflectivity[3];
  unsigned char  padding1[4];
  float          bumpmap_scale;
  int            image_format;
  unsigned char  mipmap_count;
  unsigned int   low_image_format;
  unsigned char  low_width;
  unsigned char  low_height;
  unsigned short depth;
  unsigned char  padding2[3];
  unsigned int    numResources;
  unsigned char   padding3[8];
} vtf_header_t;

typedef struct {
  unsigned char   tag[3];
  unsigned char   flags;
  unsigned int    offset;
} vtf_resource_entry_t;

// Utility to convert RGB565 to 8-bit per channel
static void rgb565_to_rgb888(uint16_t in, uint8_t *out) {
  uint8_t r = (in >> 11) & 0x1F;
  uint8_t g = (in >> 5 ) & 0x3F;
  uint8_t b = (in >> 0 ) & 0x1F;
  out[0] = (r << 3) | (r >> 2);
  out[1] = (g << 2) | (g >> 4);
  out[2] = (b << 3) | (b >> 2);
}

// Decode uncompressed RGBA/RGB formats to 4-byte-per-pixel rows
static void decode_rgba(vtf_header_t* h, uint8_t *end, int frame_off, uint8_t** rows) {
  int has_alpha = (h->image_format==IMAGE_FORMAT_RGBA8888 ||
                   h->image_format==IMAGE_FORMAT_ARGB8888||
                   h->image_format==IMAGE_FORMAT_ABGR8888||
                   h->image_format==IMAGE_FORMAT_BGRA8888);
  int w = h->width, ht = h->height;
  int pixel_bytes = has_alpha ? 4 : 3;
  int framesize = w * ht * pixel_bytes;
  uint8_t *data = end - framesize*frame_off;
  int p=0;
  for(int y=0;y<ht;y++){
    for(int x=0;x<w;x++){
      uint8_t r,g,b,a=255;
      switch(h->image_format){
        case IMAGE_FORMAT_RGBA8888:
          r=data[p++];g=data[p++];b=data[p++];a=data[p++];break;
        case IMAGE_FORMAT_ARGB8888:
          a=data[p++];r=data[p++];g=data[p++];b=data[p++];break;
        case IMAGE_FORMAT_ABGR8888:
          a=data[p++];b=data[p++];g=data[p++];r=data[p++];break;
        case IMAGE_FORMAT_BGRA8888:
          b=data[p++];g=data[p++];r=data[p++];a=data[p++];break;
        case IMAGE_FORMAT_RGB888:
          r=data[p++];g=data[p++];b=data[p++];break;
        case IMAGE_FORMAT_BGR888:
          b=data[p++];g=data[p++];r=data[p++];break;
        default:
          r=g=b=255; a=255;
      }
      int idx = x*4;
      rows[y][4*x+0]=r;
      rows[y][4*x+1]=g;
      rows[y][4*x+2]=b;
      rows[y][4*x+3]=a;
    }
  }
}

// Common DXT1 color decode block
static void decode_dxt_colors(int bx,int by,uint16_t c0,uint16_t c1,uint32_t ci,uint8_t** rows) {
  uint8_t cols[4][3];
  rgb565_to_rgb888(c0, cols[0]);
  rgb565_to_rgb888(c1, cols[1]);
  // two interpolated
  for(int i=0;i<3;i++) cols[2][i] = (2*cols[0][i] + cols[1][i] +1)/3;
  for(int i=0;i<3;i++) cols[3][i] = (cols[0][i] + 2*cols[1][i] +1)/3;
  for(int py=0;py<4;py++) for(int px=0;px<4;px++){
    int idx=(ci&3);
    rows[by+py][4*(bx+px)+0]=cols[idx][0];
    rows[by+py][4*(bx+px)+1]=cols[idx][1];
    rows[by+py][4*(bx+px)+2]=cols[idx][2];
    rows[by+py][4*(bx+px)+3]=255;
    ci>>=2;
  }
}

// DXT1 decode
static void decode_dxt1(vtf_header_t* h, uint8_t *end, int frame_off, uint8_t** rows) {
  int w = h->width, ht = h->height;
  int blockSize = 8;
  int framesize = ((w+3)/4)*((ht+3)/4)*blockSize;
  uint8_t* data = end - framesize*frame_off;
  int p=0;
  for(int y=0;y<ht;y+=4) for(int x=0;x<w;x+=4){
    uint16_t c0 = data[p] | (data[p+1]<<8);
    uint16_t c1 = data[p+2] | (data[p+3]<<8);
    uint32_t ci = data[p+4] | (data[p+5]<<8) | (data[p+6]<<16) | (data[p+7]<<24);
    p+=8;
    decode_dxt_colors(x,y,c0,c1,ci,rows);
  }
}

// DXT3 decode (explicit alpha)
static void decode_dxt3(vtf_header_t* h, uint8_t *end, int frame_off, uint8_t** rows) {
  int w=h->width, ht=h->height;
  int blockSize=16;
  int framesize=((w+3)/4)*((ht+3)/4)*blockSize;
  uint8_t* data = end - framesize*frame_off;
  int p=0;
  for(int y=0;y<ht;y+=4) for(int x=0;x<w;x+=4){
    uint64_t alpha=0;
    for(int i=0;i<8;i++) alpha |= (uint64_t)data[p++]<<(i*8);
    uint16_t c0 = data[p]|(data[p+1]<<8);
    uint16_t c1 = data[p+2]|(data[p+3]<<8);
    uint32_t ci = data[p+4]|(data[p+5]<<8)|(data[p+6]<<16)|(data[p+7]<<24);
    p+=8;
    // colors
    decode_dxt_colors(x,y,c0,c1,ci,rows);
    // apply alpha
    for(int py=0;py<4;py++) for(int px=0;px<4;px++){
      rows[y+py][4*(x+px)+3] = alpha & 0xF;
      alpha >>= 4;
    }
  }
}

// DXT5 decode (alpha interpolated)
static void decode_dxt5(vtf_header_t* h, uint8_t *end, int frame_off, uint8_t** rows) {
  int w=h->width, ht=h->height;
  int blockSize=16;
  int framesize=((w+3)/4)*((ht+3)/4)*blockSize;
  uint8_t* data = end - framesize*frame_off;
  int p=0;
  for(int y=0;y<ht;y+=4) for(int x=0;x<w;x+=4){
    uint8_t a0 = data[p++], a1=data[p++];
    uint8_t avals[8]; avals[0]=a0; avals[1]=a1;
    if(a0>a1) for(int i=2;i<8;i++) avals[i] = ((8-i)*a0 + (i-1)*a1)/7;
    else { for(int i=2;i<6;i++) avals[i]=((6-i)*a0 + (i-1)*a1)/5; avals[6]=0; avals[7]=255; }
    uint64_t ai=0;
    for(int i=0;i<6;i++) ai |= (uint64_t)data[p++]<<(8*i);
    uint16_t c0 = data[p]|(data[p+1]<<8);
    uint16_t c1 = data[p+2]|(data[p+3]<<8);
    uint32_t ci = data[p+4]|(data[p+5]<<8)|(data[p+6]<<16)|(data[p+7]<<24);
    p+=8;
    // alpha
    for(int py=0;py<4;py++) for(int px=0;px<4;px++){
      rows[y+py][4*(x+px)+3] = avals[ai & 0x7]; ai >>= 3;
    }
    // color
    decode_dxt_colors(x,y,c0,c1,ci,rows);
  }
}

// -- argp, main, resize, and PNG/JPEG output as before --
struct options { char* in_path; char* out_path; int frame, verbose, resize_w, resize_h; enum{FMT_PNG,FMT_JPEG} out_fmt; };
static struct argp_option options_desc[] = {
  { "frame", 'f', "FRAME", 0, "Frame index"},
  { "verbose", 'v', NULL, 0, "Verbose"},
  { "resize", 's', "WxH", 0, "Resize"},
  { "format", 't', "FMT", 0, "png|jpeg"}, {0}
};
static int parse_opt(int key, char* arg, struct argp_state* st) { struct options* o = st->input;
  switch(key){ case ARGP_KEY_INIT: o->frame=1;o->verbose=0;o->resize_w=o->resize_h=0;o->out_fmt=FMT_PNG; break;
    case 'f': o->frame=atoi(arg); break;
    case 'v': o->verbose=1; break;
    case 's': if(sscanf(arg,"%dx%d",&o->resize_w,&o->resize_h)!=2) argp_usage(st); break;
    case 't': if(!strcasecmp(arg,"png")) o->out_fmt=FMT_PNG; else if(!strcasecmp(arg,"jpeg")||!strcasecmp(arg,"jpg")) o->out_fmt=FMT_JPEG; else argp_usage(st); break;
    case ARGP_KEY_ARG: if(st->arg_num==0) o->in_path=arg; else if(st->arg_num==1) o->out_path=arg; else argp_usage(st); break;
    case ARGP_KEY_END: if(st->arg_num<2) argp_usage(st); break; default: return ARGP_ERR_UNKNOWN; }
  return 0;
}
int main(int argc,char**argv){
  struct options opt; struct argp argp={options_desc,parse_opt,"IN.VTF OUT.(png|jpg)",0}; argp_parse(&argp,argc,argv,0,0,&opt);
  int fd=open(opt.in_path,O_RDONLY); if(fd<0){perror(opt.in_path);return 1;} int sz=lseek(fd,0,SEEK_END);
  uint8_t* data=mmap(NULL,sz,PROT_READ,MAP_PRIVATE,fd,0); vtf_header_t* h=(vtf_header_t*)data;
  int img_end=sz; if(h->version[1]>2){ vtf_resource_entry_t*r=(void*)(data+sizeof(vtf_header_t));int found=0;
    for(unsigned i=0;i<h->numResources;i++){ if(!found&&r[i].tag[0]==0x30) found=1; else if(found&&r[i].tag[0]!='C'){img_end=r[i].offset;break;} }
  }
  int foff=1+h->frames-opt.frame;
  int iw=h->width, ih=h->height;
  uint8_t** rows=malloc(ih*sizeof(void*)); for(int y=0;y<ih;y++){rows[y]=malloc(iw*4);memset(rows[y],255,iw*4);}  
  switch(h->image_format){case IMAGE_FORMAT_RGBA8888:case IMAGE_FORMAT_ARGB8888:case IMAGE_FORMAT_ABGR8888:case IMAGE_FORMAT_BGRA8888:case IMAGE_FORMAT_RGB888:case IMAGE_FORMAT_BGR888: decode_rgba(h,data+img_end,foff,rows);break;case IMAGE_FORMAT_DXT1:decode_dxt1(h,data+img_end,foff,rows);break;case IMAGE_FORMAT_DXT3:decode_dxt3(h,data+img_end,foff,rows);break;case IMAGE_FORMAT_DXT5:decode_dxt5(h,data+img_end,foff,rows);break;default:fprintf(stderr,"fmt %d?\n",h->image_format);return 1;}
  int ow=iw, oh=ih; uint8_t** out=rows;
  if(opt.resize_w&&opt.resize_h){ow=opt.resize_w;oh=opt.resize_h; out=malloc(oh*sizeof(void*)); for(int y=0;y<oh;y++){out[y]=malloc(ow*4);int sy=y*ih/oh;for(int x=0;x<ow;x++){int sx=x*iw/ow; memcpy(&out[y][4*x],&rows[sy][4*sx],4);} }}
  if(opt.out_fmt==FMT_PNG){FILE*f=fopen(opt.out_path,"wb");png_structp png=png_create_write_struct(PNG_LIBPNG_VER_STRING,NULL,NULL,NULL);
    png_infop info=png_create_info_struct(png);png_init_io(png,f);
    png_set_IHDR(png,info,ow,oh,8,PNG_COLOR_TYPE_RGB_ALPHA,PNG_INTERLACE_NONE,PNG_COMPRESSION_TYPE_BASE,PNG_FILTER_TYPE_BASE);
    png_write_info(png,info);png_write_image(png,out);png_write_end(png,NULL);fclose(f);
  } else {
    struct jpeg_compress_struct jc;struct jpeg_error_mgr jerr;
    jc.err=jpeg_std_error(&jerr);jpeg_create_compress(&jc);
    FILE*f=fopen(opt.out_path,"wb");jpeg_stdio_dest(&jc,f);
    jc.image_width=ow;jc.image_height=oh;jc.input_components=4;jc.in_color_space=JCS_EXT_RGBA;
    jpeg_set_defaults(&jc);jpeg_set_quality(&jc,90,TRUE);jpeg_start_compress(&jc,TRUE);
    JSAMPROW rowptr[1];while(jc.next_scanline<jc.image_height){rowptr[0]=out[jc.next_scanline];jpeg_write_scanlines(&jc,rowptr,1);}
    jpeg_finish_compress(&jc);jpeg_destroy_compress(&jc);fclose(f);
  }
  if(out!=rows){for(int y=0;y<oh;y++)free(out[y]);free(out);} for(int y=0;y<ih;y++)free(rows[y]);free(rows);
  munmap(data,sz);close(fd);
  return 0;
}
