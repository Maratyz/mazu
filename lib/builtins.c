/* Self-contained replacements for the libgcc helpers GCC emits at
 * -march=rv64imac_zicsr -mabi=lp64. Single-multilib prebuilt toolchains
 * (e.g. riscv-collab/riscv-gnu-toolchain elf releases) ship libgcc.a only
 * in their default ABI -- typically rv64gc/lp64d -- which the linker then
 * refuses to link into a soft-float kernel. Providing the helpers here
 * leaves libgcc.a on the link line as an empty fallback: nothing is pulled
 * in, no ABI mismatch can occur.
 *
 * Implementations follow libgcc2.c semantics; correctness over speed
 * because callers are __builtin_clz / __builtin_bswap{16,32,64}, not hot
 * paths.  When Zbb is enabled GCC inlines the instruction and these are
 * dead code -- left in to keep the kernel link self-contained regardless
 * of march.
 */

typedef unsigned int u32;
typedef unsigned long u64;

/* O(log n) bit scan; the scheduler picks tasks via __builtin_clz on every
 * dispatch, so a naive bit-by-bit loop here would starve secondary harts.
 */
int __clzsi2(u32 x)
{
    int n = 0;
    if (x == 0)
        return 32;
    if ((x & 0xffff0000U) == 0) {
        n += 16;
        x <<= 16;
    }
    if ((x & 0xff000000U) == 0) {
        n += 8;
        x <<= 8;
    }
    if ((x & 0xf0000000U) == 0) {
        n += 4;
        x <<= 4;
    }
    if ((x & 0xc0000000U) == 0) {
        n += 2;
        x <<= 2;
    }
    if ((x & 0x80000000U) == 0)
        n += 1;
    return n;
}

int __ctzsi2(u32 x)
{
    int n = 0;
    if (x == 0)
        return 32;
    if ((x & 0x0000ffffU) == 0) {
        n += 16;
        x >>= 16;
    }
    if ((x & 0x000000ffU) == 0) {
        n += 8;
        x >>= 8;
    }
    if ((x & 0x0000000fU) == 0) {
        n += 4;
        x >>= 4;
    }
    if ((x & 0x00000003U) == 0) {
        n += 2;
        x >>= 2;
    }
    if ((x & 0x00000001U) == 0)
        n += 1;
    return n;
}

int __clzdi2(u64 x)
{
    if ((x >> 32) == 0)
        return 32 + __clzsi2((u32) x);
    return __clzsi2((u32) (x >> 32));
}

int __ctzdi2(u64 x)
{
    if ((x & 0xffffffffU) == 0)
        return 32 + __ctzsi2((u32) (x >> 32));
    return __ctzsi2((u32) x);
}

u32 __bswapsi2(u32 x)
{
    return ((x & 0x000000ffU) << 24) | ((x & 0x0000ff00U) << 8) |
           ((x & 0x00ff0000U) >> 8) | ((x & 0xff000000U) >> 24);
}

u64 __bswapdi2(u64 x)
{
    return ((u64) __bswapsi2((u32) x) << 32) |
           (u64) __bswapsi2((u32) (x >> 32));
}
