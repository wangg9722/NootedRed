// 解码规格守卫：在 AMDRadeonVADriver2 的 VADecoder::Init 入口执行。
// HEVC（codec=5）且宽>4096 或 高>2304 时直接返回 3（沿用原函数内既有的失败返回值），
// 让解码会话创建失败、播放器回退软解，避免 VCN 收到超规格提交而挂死。
// 否则重放入口前 24 字节并跳回原函数继续。
// 参数：rdi=self(VADecoder*), rsi=codec。
.text
.globl _vad_decode_guard
_vad_decode_guard:
    cmpl $5, 120(%rdi)          // [self+0x78] codec
    jne Lrestore
    cmpl $4096, 124(%rdi)       // [self+0x7c] width
    ja Lreject
    cmpl $2304, 128(%rdi)       // [self+0x80] height
    ja Lreject
Lrestore:
    pushq %rbp
    movq %rsp, %rbp
    pushq %r14
    pushq %rbx
    movl %esi, 296(%rdi)        // [self+0x128] = codec
    movq $0, 240(%rdi)          // [self+0xf0] = 0
    movabsq $0x1122334455667788, %r11
    jmpq *%r11
Lreject:
    movl $3, %eax
    ret
