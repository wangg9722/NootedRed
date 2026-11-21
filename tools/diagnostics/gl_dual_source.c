// 双源混合（dual-source blending）GL 测试：GL 3.2 core，fragment shader 输出两个颜色，
// blendFunc 使用 GL_SRC1_COLOR。若 GFX 引擎随即挂死（FenceNotRetired），则证明
// GL 路径与 Metal 路径共享同一 dual-quad 硬件缺陷，修法可对齐 LegacyBlend gate+allow。
// clang -Wno-deprecated-declarations -o /tmp/gltest2 /tmp/gltest2.c -framework OpenGL -framework CoreFoundation
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static const char *VS =
    "#version 150\n"
    "in vec2 pos; void main() { gl_Position = vec4(pos, 0.0, 1.0); }\n";
static const char *FS =
    "#version 150\n"
    "out vec4 col0;\n"
    "out vec4 col1;\n"
    "void main() { col0 = vec4(0.9, 0.2, 0.2, 1.0); col1 = vec4(0.2, 0.8, 0.3, 1.0); }\n";

static GLuint compile(GLenum type, const char *src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[512]; glGetShaderInfoLog(s, 512, NULL, log); printf("shader err: %s\n", log); }
    return s;
}

int main(void) {
    CGLPixelFormatAttribute attrs[] = {kCGLPFAAccelerated, kCGLPFADoubleBuffer,
                                       kCGLPFAOpenGLProfile, (CGLPixelFormatAttribute)kCGLOGLPVersion_GL3_Core, 0};
    CGLPixelFormatObj pf; GLint npv = 0;
    if (CGLChoosePixelFormat(attrs, &pf, &npv) != kCGLNoError) { printf("pf fail\n"); return 1; }
    CGLContextObj ctx;
    if (CGLCreateContext(pf, NULL, &ctx) != kCGLNoError) { printf("ctx fail\n"); return 1; }
    CGLSetCurrentContext(ctx);
    GLint vsync = 0; CGLSetParameter(ctx, kCGLCPSwapInterval, &vsync);
    printf("GL_VERSION=%s\n", glGetString(GL_VERSION));

    GLuint prog = glCreateProgram();
    glAttachShader(prog, compile(GL_VERTEX_SHADER, VS));
    glAttachShader(prog, compile(GL_FRAGMENT_SHADER, FS));
    glBindFragDataLocation(prog, 0, "col0");
    glBindFragDataLocation(prog, 1, "col1");
    glLinkProgram(prog);
    GLint ok = 0; glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) { char log[512]; glGetProgramInfoLog(prog, 512, NULL, log); printf("link err: %s\n", log); return 1; }
    glUseProgram(prog);

    const GLfloat verts[] = {-0.8f, -0.8f, 0.8f, -0.8f, 0.0f, 0.8f};
    GLuint vao, vbo;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
    GLint loc = glGetAttribLocation(prog, "pos");
    glEnableVertexAttribArray(loc);
    glVertexAttribPointer(loc, 2, GL_FLOAT, GL_FALSE, 0, 0);

    // 关键：双源混合 —— src factor 用第二个输出
    glEnable(GL_BLEND);
    glBlendFuncSeparate(GL_SRC1_COLOR, GL_DST_ALPHA, GL_SRC1_ALPHA, GL_DST_ALPHA);
    printf("blend set: GL_SRC1_COLOR (dual-source). drawing...\n");

    for (int f = 0; f < 300; f++) {
        glClearColor(0.1f, 0.2f, 0.3f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glFinish();
        CGLFlushDrawable(ctx);
        if (f % 50 == 0) printf("frame %d ok\n", f);
    }
    printf("done 300 dual-source frames, no crash\n");
    CGLDestroyContext(ctx);
    return 0;
}
