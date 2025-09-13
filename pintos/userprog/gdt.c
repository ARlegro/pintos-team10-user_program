#include "userprog/gdt.h"
#include <debug.h>
#include "userprog/tss.h"
#include "threads/mmu.h"
#include "threads/palloc.h"
#include "threads/vaddr.h"
#include "intrinsic.h"

/* The Global Descriptor Table (GDT).
 *
 * The GDT, an x86-64 specific structure, defines segments that can
 * potentially be used by all processes in a system, subject to
 * their permissions.  There is also a per-process Local
 * Descriptor Table (LDT) but that is not used by modern
 * operating systems.
 *
 * Each entry in the GDT, which is known by its byte offset in
 * the table, identifies a segment.  For our purposes only three
 * types of segments are of interest: code, data, and TSS or
 * Task-State Segment descriptors.  The former two types are
 * exactly what they sound like.  The TSS is used primarily for
 * stack switching on interrupts. */
/* 전역 디스크립터 테이블(GDT).
 *
 * GDT는 x86-64 아키텍처에 특화된 구조체로,
 * 시스템의 모든 프로세스가 권한에 따라 사용할 수 있는 세그먼트를 정의한다.
 * 프로세스마다 존재하는 LDT(Local Descriptor Table)도 있지만,
 * 현대 운영체제에서는 사용되지 않는다.
 *
 * GDT의 각 엔트리는 바이트 오프셋으로 식별되며,
 * 세그먼트를 나타낸다. 여기서 중요한 세그먼트는 세 가지:
 * 코드(code), 데이터(data), 그리고 TSS(Task-State Segment).
 * 코드와 데이터 세그먼트는 이름 그대로다.
 * TSS는 주로 인터럽트 발생 시 스택 전환(stack switching)에 사용된다. */

struct segment_desc {
	unsigned lim_15_0 : 16;
	unsigned base_15_0 : 16;
	unsigned base_23_16 : 8;
	unsigned type : 4;
	unsigned s : 1;
	unsigned dpl : 2;
	unsigned p : 1;
	unsigned lim_19_16 : 4;
	unsigned avl : 1;
	unsigned l : 1;
	unsigned db : 1;
	unsigned g : 1;
	unsigned base_31_24 : 8;
};

struct segment_descriptor64 {
	unsigned lim_15_0 : 16;
	unsigned base_15_0 : 16;
	unsigned base_23_16 : 8;
	unsigned type : 4;
	unsigned s : 1;
	unsigned dpl : 2;
	unsigned p : 1;
	unsigned lim_19_16 : 4;
	unsigned avl : 1;
	unsigned rsv1 : 2;
	unsigned g : 1;
	unsigned base_31_24 : 8;
	uint32_t base_63_32;
	unsigned res1 : 8;
	unsigned clear : 8;
	unsigned res2 : 16;
};

#define SEG64(type, base, lim, dpl) (struct segment_desc) \
{ ((lim) >> 12) & 0xffff, (base) & 0xffff, ((base) >> 16) & 0xff, \
	type, 1, dpl, 1, (unsigned) (lim) >> 28, 0, 1, 0, 1, \
	(unsigned) (base) >> 24 }

static struct segment_desc gdt[SEL_CNT] = {
	[SEL_NULL >> 3] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
	[SEL_KCSEG >> 3] = SEG64 (0xa, 0x0, 0xffffffff, 0),
	[SEL_KDSEG >> 3] = SEG64 (0x2, 0x0, 0xffffffff, 0),
	[SEL_UDSEG >> 3] = SEG64 (0x2, 0x0, 0xffffffff, 3),
	[SEL_UCSEG >> 3] = SEG64 (0xa, 0x0, 0xffffffff, 3),
	[SEL_TSS >> 3] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
	[6] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
	[7] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
};

struct desc_ptr gdt_ds = {
	.size = sizeof(gdt) - 1,
	.address = (uint64_t) gdt
};

/* Sets up a proper GDT.  The bootstrap loader's GDT didn't
   include user-mode selectors or a TSS, but we need both now. */
/* 올바른 GDT를 설정한다.
   부트스트랩 로더의 GDT에는 사용자 모드 셀렉터나 TSS가 없으므로,
   이제 이들을 추가해야 한다. */
void
gdt_init (void) {
	/* Initialize GDT. */
	/* GDT 초기화 */
	struct segment_descriptor64 *tss_desc =
		(struct segment_descriptor64 *) &gdt[SEL_TSS >> 3];
	struct task_state *tss = tss_get ();

	*tss_desc = (struct segment_descriptor64) {
		.lim_15_0 = (uint64_t) (sizeof (struct task_state)) & 0xffff,
		.base_15_0 = (uint64_t) (tss) & 0xffff,
		.base_23_16 = ((uint64_t) (tss) >> 16) & 0xff,
		.type = 0x9,
		.s = 0,
		.dpl = 0,
		.p = 1,
		.lim_19_16 = ((uint64_t)(sizeof (struct task_state)) >> 16) & 0xf,
		.avl = 0,
		.rsv1 = 0,
		.g = 0,
		.base_31_24 = ((uint64_t)(tss) >> 24) & 0xff,
		.base_63_32 = ((uint64_t)(tss) >> 32) & 0xffffffff,
		.res1 = 0,
		.clear = 0,
		.res2 = 0
	};

	lgdt (&gdt_ds);
	/* reload segment registers */
	/* 세그먼트 레지스터를 다시 로드한다 */
	asm volatile("movw %%ax, %%gs" :: "a" (SEL_UDSEG));
	asm volatile("movw %%ax, %%fs" :: "a" (0));
	asm volatile("movw %%ax, %%es" :: "a" (SEL_KDSEG));
	asm volatile("movw %%ax, %%ds" :: "a" (SEL_KDSEG));
	asm volatile("movw %%ax, %%ss" :: "a" (SEL_KDSEG));
	asm volatile("pushq %%rbx\n"
			"movabs $1f, %%rax\n"
			"pushq %%rax\n"
			"lretq\n"
			"1:\n" :: "b" (SEL_KCSEG):"cc","memory");
	/* Kill the local descriptor table */
	/* 로컬 디스크립터 테이블(LDT)을 비운다 */
	lldt (0);
}
