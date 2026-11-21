// gl_window_present.m — 真窗口 NSOpenGLView 呈现压力测试（覆盖 flip/合成路径）
// 编译： clang -O1 -o /tmp/glw gl_window_present.m -framework Cocoa -framework OpenGL
// 用法： /tmp/glw [宽] [高] [帧数]   默认 1080 1920 1800
#import <Cocoa/Cocoa.h>
#import <OpenGL/gl3.h>
#import <OpenGL/OpenGL.h>

static int gW=1080, gH=1920, gN=1800, gFrame=0;
static NSOpenGLContext *gCtx; static GLuint gTex; static unsigned char *gPix;

@interface AppDel : NSObject <NSApplicationDelegate>
@end
@implementation AppDel
- (void)applicationDidFinishLaunching:(NSNotification*)n {
    NSRect r = NSMakeRect(0,0,gW/3.0,gH/3.0);
    NSWindow *w = [[NSWindow alloc] initWithContentRect:r styleMask:(NSWindowStyleMaskTitled|NSWindowStyleMaskClosable)
        backing:NSBackingStoreBuffered defer:NO];
    [w setTitle:@"glw"]; [w makeKeyAndOrderFront:nil]; [NSApp activateIgnoringOtherApps:YES];
    NSOpenGLPixelFormatAttribute a[] = {NSOpenGLPFAAccelerated, NSOpenGLPFADoubleBuffer, NSOpenGLPFAColorSize,24, 0};
    NSOpenGLPixelFormat *pf = [[NSOpenGLPixelFormat alloc] initWithAttributes:a];
    NSOpenGLView *v = [[NSOpenGLView alloc] initWithFrame:r pixelFormat:pf];
    [w setContentView:v];
    gCtx = [v openGLContext]; [gCtx makeCurrentContext];
    printf("RENDERER=%s VERSION=%s\n", glGetString(GL_RENDERER), glGetString(GL_VERSION)); fflush(stdout);
    glGenTextures(1,&gTex); glBindTexture(GL_TEXTURE_RECTANGLE,gTex);
    glTexParameteri(GL_TEXTURE_RECTANGLE,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
    glTexParameteri(GL_TEXTURE_RECTANGLE,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
    glTexImage2D(GL_TEXTURE_RECTANGLE,0,GL_RGBA8,gW,gH,0,GL_BGRA,GL_UNSIGNED_INT_8_8_8_8_REV,NULL);
    gPix = malloc((size_t)gW*gH*4); memset(gPix,0x55,(size_t)gW*gH*4);
    [NSTimer scheduledTimerWithTimeInterval:1.0/60 target:self selector:@selector(tick:) userInfo:nil repeats:YES];
}
- (void)tick:(NSTimer*)t {
    if (gFrame>=gN) { printf("done frames=%d\n", gFrame); fflush(stdout); [NSApp terminate:nil]; return; }
    [gCtx makeCurrentContext];
    for (int k=0;k<16;k++){ size_t o=((size_t)((gFrame+k)%gH))*gW*4 + (size_t)((gFrame*17+k*53)%gW)*4;
        if(o+4<(size_t)gW*gH*4){ gPix[o]=(unsigned char)gFrame; gPix[o+1]=(unsigned char)(gFrame>>3); gPix[o+2]=(unsigned char)k*16; } }
    glBindTexture(GL_TEXTURE_RECTANGLE,gTex);
    glTexSubImage2D(GL_TEXTURE_RECTANGLE,0,0,0,gW,gH,GL_BGRA,GL_UNSIGNED_INT_8_8_8_8_REV,gPix);
    glClearColor(0.1,0.1,0.1,1.0); glClear(GL_COLOR_BUFFER_BIT);
    glFlush();
    [gCtx flushBuffer];            // 关键：present + flip
    if((gFrame%120)==0){ printf("frame %d\n", gFrame); fflush(stdout); }
    gFrame++;
}
@end
int main(int argc,char**argv){
    if(argc>1) gW=atoi(argv[1]); if(argc>2) gH=atoi(argv[2]); if(argc>3) gN=atoi(argv[3]);
    @autoreleasepool { [NSApplication sharedApplication]; AppDel *d=[AppDel new];
        [NSApp setDelegate:d]; [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular]; [NSApp run]; }
    return 0;
}
