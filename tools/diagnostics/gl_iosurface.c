// IOSurface → GL 纹理路径测试：模拟 Chromium 视频帧上传。
// 每帧分配/写入 IOSurface，CGLTexImageIOSurface2D 绑定为纹理后采样绘制。
// clang -Wno-deprecated-declarations -o /tmp/gltest3 /tmp/gltest3.c -framework OpenGL -framework CoreFoundation -framework IOSurface -framework CoreVideo
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>
#include <CoreFoundation/CoreFoundation.h>
#include <Foundation/Foundation.h>
#include <IOSurface/IOSurface.h>
#include <stdio.h>
#include <string.h>

static const char *VS =
    "#version 150\n"
    "in vec2 pos; out vec2 uv; void main() { uv = pos * 0.5 + 0.5; gl_Position = vec4(pos, 0.0, 1.0); }\n";
static const char *FS =
    "#version 150\n"
    "in vec2 uv; out vec4 col; uniform sampler2D tex;\n"
    "void main() { col = vec4(texture(tex, uv).rgb, 1.0); }\n";

static GLuint compile(GLenum type, const char *src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[512]; glGetShaderInfoLog(s, 512, NULL, log); printf("shader err: %s\n", log); }
    return s;
}

int main(void) {
    setbuf(stdout, NULL);
    printf("start\n");
    CGLPixelFormatAttribute attrs[] = {kCGLPFAAccelerated, kCGLPFADoubleBuffer,
                                       kCGLPFAOpenGLProfile, (CGLPixelFormatAttribute)kCGLOGLPVersion_GL3_Core, 0};
    CGLPixelFormatObj pf; GLint npv = 0;
    if (CGLChoosePixelFormat(attrs, &pf, &npv) != kCGLNoError) { printf("pf fail\n"); return 1; }
    CGLContextObj ctx;
    if (CGLCreateContext(pf, NULL, &ctx) != kCGLNoError) { printf("ctx fail\n"); return 1; }
    CGLSetCurrentContext(ctx);
    GLint vsync = 0; CGLSetParameter(ctx, kCGLCPSwapInterval, &vsync);

    GLuint prog = glCreateProgram();
    glAttachShader(prog, compile(GL_VERTEX_SHADER, VS));
    glAttachShader(prog, compile(GL_FRAGMENT_SHADER, FS));
    glLinkProgram(prog);
    GLint ok = 0; glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) { char log[512]; glGetProgramInfoLog(prog, 512, NULL, log); printf("link err: %s\n", log); return 1; }
    glUseProgram(prog);

    const GLfloat verts[] = {-1, -1, 1, -1, -1, 1, 1, 1};
    GLuint vao, vbo;
    glGenVertexArrays(1, &vao); glBindVertexArray(vao);
    glGenBuffers(1, &vbo); glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
    GLint loc = glGetAttribLocation(prog, "pos");
    glEnableVertexAttribArray(loc);
    glVertexAttribPointer(loc, 2, GL_FLOAT, GL_FALSE, 0, 0);

    const int W = 1920, H = 1080;
    NSDictionary *dict = [NSDictionary dictionaryWithObjectsAndKeys:
        [NSNumber numberWithInt:W], (id)kIOSurfaceWidth,
        [NSNumber numberWithInt:H], (id)kIOSurfaceHeight,
        [NSNumber numberWithInt:W * 4], (id)kIOSurfaceBytesPerRow,
        [NSNumber numberWithInt:4], (id)CFSTR("IOSurfaceBytesPerElem"),
        [NSNumber numberWithInt:'BGRA'], (id)kIOSurfacePixelFormat, nil];
    IOSurfaceRef surf = IOSurfaceCreate((__bridge CFDictionaryRef)dict);
    if (!surf) { printf("IOSurface create fail\n"); return 1; }

    GLuint tex;
    glGenTextures(1, &tex);
    GLint uni = glGetUniformLocation(prog, "tex");
    glUniform1i(uni, 0);

    printf("drawing with IOSurface texture (1920x1080, per-frame rewrite)...\n");
    for (int f = 0; f < 300; f++) {
        if (f == 0) printf("step: lock\n");
        IOSurfaceLock(surf, 0, NULL);
        void *base = IOSurfaceGetBaseAddress(surf);
        if (f == 0) printf("step: base=%p\n", base);
        if (!base) { printf("NULL base\n"); return 1; }
        size_t asz = IOSurfaceGetAllocSize(surf);
        if (f == 0) printf("step: allocSize=%zu\n", asz);
        memset(base, (f * 3) & 0xFF, asz);
        IOSurfaceUnlock(surf, 0, NULL);

        if (f == 0) printf("step: bindtex\n");
        glBindTexture(GL_TEXTURE_RECTANGLE, tex);
        if (f == 0) printf("step: teximage-iosurface\n");
        CGLError err = CGLTexImageIOSurface2D(ctx, GL_TEXTURE_RECTANGLE, GL_RGBA8,
                                              W, H, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, surf, 0);
        if (err != kCGLNoError) { printf("CGLTexImageIOSurface2D err %d\n", err); return 1; }
        glTexParameteri(GL_TEXTURE_RECTANGLE, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_RECTANGLE, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

        if (f == 0) printf("step: clear\n");
        glClearColor(0, 0, 0, 1); glClear(GL_COLOR_BUFFER_BIT);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        if (f == 0) printf("step: draw\n");
        if (f == 0) printf("step: finish\n");
        glFinish();
        CGLFlushDrawable(ctx);
        if (f % 50 == 0) printf("frame %d ok\n", f);
    }
    printf("done 300 IOSurface frames, no crash\n");
    CGLDestroyContext(ctx);
    return 0;
}
