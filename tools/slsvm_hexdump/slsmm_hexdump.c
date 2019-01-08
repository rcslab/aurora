#include "slsmm.h"

#include <stdio.h>
#include <stdlib.h>
#include <memory.h>

#include <machine/reg.h>

struct page {
	vm_pindex_t poffset;
	uint8_t data[PAGE_SIZE];
	struct page *next;
};

struct entry {
	struct vm_map_entry_info info;
	struct page *page;
};

struct hexdump {
	int idx;
	struct proc_info *proc_info;
	struct thread_info *thread_info;
	struct memckpt_info *memckpt_info;
	struct hexdump *prev, *next;
};

static void help() {
	printf("n		next dump\n");
	printf("p		previous dump\n");
	printf("r <tid>		print regs of thread <tid>\n");
	printf("f <tid>		print fpregs of thread <tid>\n");
	printf("v		print vmspace info of current dump\n");
	printf("e <eid>		print vm_map_entry info of <eid>th entry\n");
	printf("c <eid>		print the up-to-current-dump data of <eid>th entry\n");
	printf("d <eid>		print the data of <eid>th entry from delta dump\n");
	printf("q		quit\n");
}

static int has_next_dump(FILE *f) {
	if (fgetc(f) == EOF) return 0;
	fseek(f, -1, SEEK_CUR);
	return 1;
}

static struct proc_info *load_proc_info(FILE *f) {
	struct proc_info *proc_info = NULL;
	proc_info = malloc(sizeof(struct proc_info));
	if (proc_info) fread(proc_info, sizeof(struct proc_info), 1, f);
	return proc_info;
}

static void print_proc_info(struct proc_info *proc_info) {
	printf("pid: %d	nthreads: %zu\n", proc_info->pid, proc_info->nthreads);
}

static struct thread_info *load_thread_info(FILE *f, size_t nthreads) {
	struct thread_info *thread_info = NULL;
	thread_info = malloc(sizeof(struct thread_info) * nthreads);
	if (thread_info) fread(thread_info, sizeof(struct thread_info), nthreads, f);
	return thread_info;
}

static void print_reg(struct thread_info *thread_info, int t_idx) {
	struct reg *reg = &thread_info[t_idx].regs;
	printf("GPRs			Flags\n");
	printf("RAX: %016lx	%016lx\n", reg->r_rax, reg->r_rflags);
	printf("RBX: %016lx	Instr Ptr\n", reg->r_rbx);
	printf("RCX: %016lx	%016lx\n", reg->r_rcx, reg->r_rip);
	printf("RDX: %016lx	Other\n", reg->r_rdx);
	printf("RBP: %016lx	cs:  %016lx\n", reg->r_rbp, reg->r_cs);
	printf("RSI: %016lx	ss:  %016lx\n", reg->r_rsi, reg->r_ss);
	printf("RDI: %016lx	rsp: %016lx\n", reg->r_rdi, reg->r_rsp);
	printf("RSP: %016lx	trapno: %08x\n", reg->r_rsp, reg->r_trapno);
	printf("R8:  %016lx	fs:     %04hx\n", reg->r_r8, reg->r_fs);
	printf("R9:  %016lx	gs:     %04hx\n", reg->r_r9, reg->r_gs);
	printf("R10: %016lx	err:    %08x\n", reg->r_r10, reg->r_err);
	printf("R11: %016lx	es:     %04hx\n", reg->r_r11, reg->r_es);
	printf("R12: %016lx	ds:     %04hx\n", reg->r_r12, reg->r_ds);
	printf("R13: %016lx\n", reg->r_r13);
	printf("R14: %016lx\n", reg->r_r14);
	printf("R15: %016lx\n", reg->r_r15);
}

static void print_fpreg(struct thread_info *thread_info, int t_idx) {
	struct fpreg *fpreg = &thread_info[t_idx].fpregs;

	printf("acc\n");
	for (int i = 0; i < 8; i ++) {
		for (int j = 0; j < 16; j ++)
			printf("%02hhx	", fpreg->fpr_acc[i][j]);
		printf("\n");
	}
	
	printf("xacc\n");
	for (int i = 0; i < 16; i ++) {
		for (int j = 0; j < 16; j ++)
			printf("%02hhx	", fpreg->fpr_xacc[i][j]);
		printf("\n");
	}

	printf("env\n");
	for (int i = 0; i < 4; i ++)
		printf("%016lx	", fpreg->fpr_env[i]);
	printf("\n");

	printf("spare\n");
	for (int i = 0; i < 12; i ++)
		if (i % 4 == 3) printf("%016lx\n", fpreg->fpr_env[i]);
		else printf("%016lx	", fpreg->fpr_env[i]);
}

static struct vmspace_info *load_vmspace_info(FILE *f) {
	struct vmspace_info *vmspace_info = NULL;
	vmspace_info = malloc(sizeof(struct vmspace_info));
	if (vmspace_info) fread(vmspace_info, sizeof(struct vmspace_info), 1, f);
	return vmspace_info; 
}

static void print_vmspace_info(struct vmspace_info *vmspace_info) {
	printf("swrss: %ld	", vmspace_info->vm_swrss);
	printf("tsize: %ld	", vmspace_info->vm_tsize);
	printf("dsize: %ld	", vmspace_info->vm_dsize);
	printf("ssize: %ld\n", vmspace_info->vm_ssize);
	printf("taddr: %016lx	", (uint64_t)vmspace_info->vm_taddr);
	printf("daddr: %016lx	", (uint64_t)vmspace_info->vm_daddr);
	printf("maxsaddr: %016lx\n", (uint64_t)vmspace_info->vm_maxsaddr);
	printf("nentries: %d\n", vmspace_info->nentries);
}

static struct entry *load_entries(FILE *f, int nentries) {
	struct entry *entries = NULL;
	entries = malloc(sizeof(struct entry) * nentries);
	if (!entries) return NULL;
	for (int i = 0; i < nentries; i ++) {
		fread(&entries[i].info, sizeof(struct vm_map_entry_info), 1, f);
		if (entries[i].info.size == ULONG_MAX) continue;

		struct page **tail = &entries[i].page;
		for (;;) {
			vm_pindex_t poffset;
			fread(&poffset, sizeof(vm_pindex_t), 1, f); 
			if (poffset == ULONG_MAX) break;

			struct page *page = malloc(sizeof(struct page));
			page->poffset = poffset;
			page->next = NULL;
			fread(page->data, PAGE_SIZE, 1, f);

			*tail = page;
			tail = &page->next;
		}
	}
	return entries;
}

static void print_entry_info(struct entry *entry) {
	printf("start:	%016lx	end:	%016lx\n", entry->info.start, entry->info.end);
	printf("offset:	%016lx	eflags:	%d\n", entry->info.offset, entry->info.eflags);
	printf("prot:	%02hhx	max_prot:	%02hhx	size %lx\n", 
			entry->info.protection, entry->info.max_protection, entry->info.size);
}	


static void print_bytes(__vm_offset_t addr, size_t size, FILE *f) {
	if (size == 0) return;
	uint8_t buf[16];
	fread(buf, size, 1, f);

	printf("%08lx: ", addr);
	for (int i = 0; i < (size>>1); i ++)
		printf("%02x%02x ", buf[i<<1]&0xff, buf[(i<<1)+1]&0xff);
	if (size&1) printf("%02x", buf[size-1]&0xff);
	printf("\n");
}

static void print_page(struct page *p, vm_offset_t addr) {
	printf("poffset:	%016lx\n", p->poffset);
	addr += PAGE_SIZE * p->poffset;

	for (int i = 0; i < PAGE_SIZE; i += 16) {
		printf("%012lx: ", addr+i);
		for (int j = 0; j < 16; j += 2)
			printf("%02x%02x ", p->data[i + j], p->data[i + j + 1]); 
		printf(" ");
		for (int j = 0; j < 16; j ++) {
			char c = p->data[i + j];
			if (c >= 32 && c <= 126) printf("%c", c);
			else printf(".");
		}
		printf("\n");
	}
}

static struct hexdump *load_hexdump(FILE *f) {
	struct hexdump *dump = NULL;
	dump = malloc(sizeof(struct hexdump));
	if (!dump) return NULL;
	dump->proc_info = load_proc_info(f);
	dump->thread_info = load_thread_info(f, dump->proc_info->nthreads);
	dump->vmspace_info = load_vmspace_info(f);
	dump->entries = load_entries(f, dump->vmspace_info->nentries);
	dump->next = NULL;
	dump->prev = NULL;
	return dump;
}

static void free_pages(struct page *page) {
	if (!page) return;
	free_pages(page->next);
	free(page);
}

static void free_hexdump(struct hexdump *dump) {
	if (!dump) return;
	free_hexdump(dump->next);

	for (int i = 0; i < dump->vmspace_info->nentries; i ++)
		free_pages(dump->entries[i].page);
	free(dump->entries);
	free(dump->vmspace_info);
	free(dump->thread_info);
	free(dump->proc_info);
}

struct page collapsed_data(struct hexdump *head, int d_idx, int e_idx, 
		vm_offset_t poffset) {
	struct page cpage;

	for (int i = 0; i <= d_idx; i ++) {
		for (struct page *p = head->entries[e_idx].page; p; p = p->next) {
			if (p->poffset == poffset) {
				memcpy(&cpage, p, sizeof(struct page));
				break;
			}
		}
		head = head->next;
	}

	return cpage;
}

static void cmd(struct hexdump *dump) {
	struct hexdump *head = dump;
	char op;
	int t_idx, e_idx;
	while (1) {
		printf("\ndump	%d	", dump->idx);
		print_proc_info(dump->proc_info);
		printf("> ");

		op = getchar();
		if (op == EOF) break;

		switch (op) {
			case 'n':
				dump = dump->next;
				break;
			case 'p':
				dump = dump->prev;
				break;
			case 'r':
				scanf("%d", &t_idx);
				if (t_idx < dump->proc_info->nthreads) 
					print_reg(dump->thread_info, t_idx);
				break;
			case 'f':
				scanf("%d", &t_idx);
				if (t_idx < dump->proc_info->nthreads) 
					print_fpreg(dump->thread_info, t_idx);
				break;
			case 'v':
				print_vmspace_info(dump->vmspace_info);
				break;
			case 'e':
				scanf("%d", &e_idx);
				if (e_idx < dump->vmspace_info->nentries)
					print_entry_info(dump->entries + e_idx);
				break;
			case 'c':
				// TODO: does not work for new page added
				scanf("%d", &e_idx);
				if (e_idx < dump->vmspace_info->nentries) {
					vm_offset_t addr = dump->entries[e_idx].info.start +
						dump->entries[e_idx].info.offset;
					for (struct page *p = head->entries[e_idx].page; p; p = p->next) {
						struct page cp = collapsed_data(head, dump->idx, e_idx, 
								p->poffset);
						print_page(&cp, addr);
					}
				}
				break;
			case 'd':
				scanf("%d", &e_idx);
				if (e_idx < dump->vmspace_info->nentries) {
					vm_offset_t addr = dump->entries[e_idx].info.start +
						dump->entries[e_idx].info.offset;
					for (struct page *p = dump->entries[e_idx].page; p; p = p->next) {
						print_page(p, addr);	
					}
				}
				break;
			case 'q':
				return;
			case 'h':
				help();
				break;
		}

		if (op != 10) getchar();
	}
}

int main(int argc, char** argv) {
	FILE *f = fopen(argv[1], "r");

	struct hexdump *dump = NULL, *tail = NULL;

	int cnt = 0;
	if (has_next_dump(f)) {

		dump = load_hexdump(f);
		dump->idx = cnt ++;
		tail = dump;

		while (has_next_dump(f)) {
			tail->next = load_hexdump(f);
			tail->next->idx = cnt ++;
			tail->next->prev = tail;
			tail = tail->next;
		}
	}
	tail->next = dump;
	dump->prev = tail;

	printf("%d dumps loaded\n", cnt);

	cmd(dump);

	tail->next = NULL;
	free_hexdump(dump);
	fclose(f);
	return 0;
}
