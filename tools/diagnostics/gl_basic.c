// 最小 CGL 测试：创建 GL 上下文，清屏+画三角形 200 帧。
// 用途：判定 GL 路径是否全局挂死 GFX 引擎。
// clang -o /tmp/gltest /tmp/gltest.c -framework OpenGL -framework CoreFoundation
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl.h>
#include <stdio.h>
#include <unistd.h>

int main(int argc, char **argv) {
    CGLPixelFormatAttribute attrs[] = {kCGLPFAAccelerated, kCGLPFADoubleBuffer, 0};
    CGLPixelFormatObj pf; GLint npv = 0, vsync = 0;
    CGLError e = CGLChoosePixelFormat(attrs, &pf, &npv);
    if (e != kCGLNoError) { printf("choosePixelFormat err %d\n", e); return 1; }
    printf("pixel formats: %d (accelerated)\n", npv);
    CGLContextObj ctx;
    e = CGLCreateContext(pf, NULL, &ctx);
    if (e != kCGLNoError) { printf("createContext err %d\n", e); return 1; }
    CGLSetCurrentContext(ctx);
    CGLSetParameter(ctx, kCGLCPSwapInterval, &vsync);
    const char *ver = (const char *)glGetString(GL_VERSION);
    const char *ren = (const char *)glGetString(GL_RENDERER);
    printf("GL_VERSION=%s\nGL_RENDERER=%s\n", ver ? ver : "?", ren ? ren : "?");
    for (int f = 0; f < 200; f++) {
        glClearColor(0.1f, 0.2f, 0.3f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glBegin(GL_TRIANGLES);
        glColor3f(1, 0, 0); glVertex2f(-0.8f, -0.8f);
        glColor3f(0, 1, 0); glVertex2f(0.8f, -0.8f);
        glColor3f(0, 0, 1); glVertex2f(0.0f, 0.8f);
        glEnd();
        glFinish();
        CGLFlushDrawable(ctx);
        if (f % 50 == 0) printf("frame %d ok\n", f);
    }
    printf("done 200 frames\n");
    CGLDestroyContext(ctx);
    return 0;
}
