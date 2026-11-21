// gl_stream_upload.c — 模拟 Electron/Chromium 旧版 GL 合成路径的持续纹理上传压力
// 编译： clang -O1 -o /tmp/glsu gl_stream_upload.c -framework OpenGL -framework CoreFoundation -framework CoreGraphics
// 用法： /tmp/glsu [宽] [高] [帧数] [fps]   默认 1080 1920 1800 30
// 判据：进程不退出/卡住，或内核日志出现 "channel 0 GFX is hung" 即复现成功
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <time.h>
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>
#include <CoreFoundation/CoreFoundation.h>

static double now_ms(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec*1000.0+t.tv_nsec/1e6; }

int main(int argc, char**argv){
    int W = argc>1?atoi(argv[1]):1080;
    int H = argc>2?atoi(argv[2]):1920;
    int N = argc>3?atoi(argv[3]):1800;
    int FPS = argc>4?atoi(argv[4]):30;

    CGLPixelFormatAttribute attrs[] = {kCGLPFAAccelerated, kCGLPFAWindow, kCGLPFADoubleBuffer, kCGLPFAColorSize,(CGLPixelFormatAttribute)24, (CGLPixelFormatAttribute)0};
    CGLPixelFormatObj pix=NULL; GLint npix=0;
    if (CGLChoosePixelFormat(attrs,&pix,&npix)!=kCGLNoError||!pix){fprintf(stderr,"pixfmt fail\n");return 2;}
    CGLContextObj ctx=NULL;
    if (CGLCreateContext(pix,NULL,&ctx)!=kCGLNoError){fprintf(stderr,"ctx fail\n");return 2;}
    CGLSetCurrentContext(ctx);

    printf("GL_VENDOR=%s\nGL_RENDERER=%s\nGL_VERSION=%s\n", glGetString(GL_VENDOR), glGetString(GL_RENDERER), glGetString(GL_VERSION));
    fflush(stdout);

    // 帧缓冲（窗口尺寸 = W x H 的缩小表示）
    GLuint fbo=0, rbo=0, tex=0;
    glGenFramebuffers(1,&fbo); glBindFramebuffer(GL_FRAMEBUFFER,fbo);
    glGenRenderbuffers(1,&rbo); glBindRenderbuffer(GL_RENDERBUFFER,rbo);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, W/2, H/2);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rbo);
    printf("fbo status 0x%x\n", glCheckFramebufferStatus(GL_FRAMEBUFFER)); fflush(stdout);

    // 视频纹理：GL_TEXTURE_RECTANGLE + BGRA（Chromium CGL 路径同款）
    glGenTextures(1,&tex); glBindTexture(GL_TEXTURE_RECTANGLE, tex);
    glTexParameteri(GL_TEXTURE_RECTANGLE, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_RECTANGLE, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_RECTANGLE, 0, GL_RGBA8, W, H, 0, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, NULL);

    uint8_t *px = malloc((size_t)W*H*4);
    if(!px){fprintf(stderr,"malloc fail\n");return 2;}
    // 预填
    for(size_t i=0;i<(size_t)W*H*4;i+=4){px[i]=0x20;px[i+1]=0x40;px[i+2]=0x80;px[i+3]=0xff;}

    double t0 = now_ms();
    int sub_mode = 0;
    for(int f=0; f<N; f++){
        // 每帧改一小块，制造脏页 —— 真实视频内容逐帧全变
        for(int k=0;k<8;k++){
            size_t off = ((size_t)((f+k)%H))*W*4 + (size_t)((f*13+k*37)%W)*4;
            if(off+4 < (size_t)W*H*4){ px[off]= (uint8_t)(f*7+k); px[off+1]=(uint8_t)(f*3); px[off+2]=(uint8_t)(k*11); }
        }
        if(sub_mode){
            // 子矩形上传（Chromium 的 dirty-rect 路径）
            int rh = 64, rw = W, ry = (f*3)%(H-rh);
            glBindTexture(GL_TEXTURE_RECTANGLE, tex);
            glTexSubImage2D(GL_TEXTURE_RECTANGLE, 0, 0, ry, rw, rh, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, px + (size_t)ry*W*4);
        } else {
            // 全帧上传（默认路径）
            glBindTexture(GL_TEXTURE_RECTANGLE, tex);
            glTexSubImage2D(GL_TEXTURE_RECTANGLE, 0, 0, 0, W, H, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, px);
        }
        if((f%300)==0) sub_mode = !sub_mode;

        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glViewport(0,0,W/2,H/2);
        glClearColor(0.1f,0.1f,0.1f,1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glFlush();
        glFinish(); // 强制提交，逼出 hang
        if((f%60)==0){ printf("frame %d t=%.1fms\n", f, now_ms()-t0); fflush(stdout); }
        usleep((useconds_t)(1000000/FPS));
    }
    printf("done frames=%d elapsed=%.0fms\n", N, now_ms()-t0);
    free(px);
    return 0;
}
