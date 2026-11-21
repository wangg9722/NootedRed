// Chromium 式 GL 合成管线测试：PBO 纹理上传 → FBO 离屏渲染到纹理 → 二次采样到窗口 → swap。
// clang -Wno-deprecated-declarations -o /tmp/gltest4 /tmp/gltest4.c -framework OpenGL -framework CoreFoundation
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>
#include <stdio.h>
#include <stdlib.h>
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
    if (!ok) { printf("link fail\n"); return 1; }
    glUseProgram(prog);

    const int W = 1920, H = 1080;
    const GLfloat verts[] = {-1, -1, 1, -1, -1, 1, 1, 1};
    GLuint vao, vbo;
    glGenVertexArrays(1, &vao); glBindVertexArray(vao);
    glGenBuffers(1, &vbo); glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
    GLint loc = glGetAttribLocation(prog, "pos");
    glEnableVertexAttribArray(loc);
    glVertexAttribPointer(loc, 2, GL_FLOAT, GL_FALSE, 0, 0);
    GLint uni = glGetUniformLocation(prog, "tex");

    // 源纹理（PBO 上传，模拟视频帧）
    GLuint pbo, srcTex;
    glGenBuffers(1, &pbo);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pbo);
    glBufferData(GL_PIXEL_UNPACK_BUFFER, (GLsizeiptr)W * H * 4, NULL, GL_STREAM_DRAW);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    glGenTextures(1, &srcTex);
    glBindTexture(GL_TEXTURE_2D, srcTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, W, H, 0, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    // FBO 离屏目标纹理
    GLuint fbo, fboTex;
    glGenTextures(1, &fboTex);
    glBindTexture(GL_TEXTURE_2D, fboTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, W, H, 0, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glGenFramebuffers(1, &fbo);

    printf("pipeline: PBO upload -> FBO(1920x1080) -> sample to window\n");
    unsigned char *cpu = malloc((size_t)W * H * 4);
    for (int f = 0; f < 300; f++) {
        // 1) CPU -> PBO -> 纹理（流式上传）
        memset(cpu, (f * 7) & 0xFF, (size_t)W * H * 4);
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pbo);
        void *p = glMapBufferRange(GL_PIXEL_UNPACK_BUFFER, 0, (GLsizeiptr)W * H * 4, GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT);
        if (p) memcpy(p, cpu, (size_t)W * H * 4);
        glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER);
        glBindTexture(GL_TEXTURE_2D, srcTex);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, W, H, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, NULL);
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);

        // 2) 渲染到 FBO
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, fboTex, 0);
        glViewport(0, 0, W, H);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, srcTex);
        glUniform1i(uni, 0);
        glClearColor(0, 0, 0, 1); glClear(GL_COLOR_BUFFER_BIT);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

        // 3) FBO 纹理 → 窗口
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, W, H);
        glBindTexture(GL_TEXTURE_2D, fboTex);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glFlush();
        CGLFlushDrawable(ctx);
        if (f % 50 == 0) printf("frame %d ok\n", f);
    }
    printf("done 300 chromium-style frames, no crash\n");
    return 0;
}
