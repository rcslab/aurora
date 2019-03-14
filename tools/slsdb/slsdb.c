
#include <stdio.h>
#include <stdlib.h>
#include <memory.h>

#include <machine/reg.h>

#include "slsmm.h"
#include "sls_user.h"

extern struct slspage **pagelist;

static void
help() {

    /*
     * XXX Not yet
     * printf("n		next dump\n");
     * printf("p		previous dump\n");
     */
	printf("p		print process info\n");
	printf("r <tid>		print regs of thread <id> in decimal\n");
	printf("s <tid>		print regs of thread <id> in hex\n");
	printf("f <tid>		print fpregs of thread <id>\n");
	printf("v		    print vmspace info of current dump\n");
	printf("e <eid>		print vm_map_entry info of <eid>th entry\n");
	printf("c <eid>		print the up-to-current-dump data of <eid>th entry\n");
	printf("d <eid>		print the data of <eid>th entry from delta dump\n");
	printf("q/EOF		quit\n");
}

static int
has_next_dump(FILE *f) {

	if (fgetc(f) == EOF)
		return 0;

	fseek(f, -1, SEEK_CUR);

	return 1;
}

static void
print_proc_info(struct proc_info *proc_info) {

	printf("pid: %d	nthreads: %zu\n", proc_info->pid, proc_info->nthreads);
}

static void
print_reg_dec(struct thread_info *thread) {
	struct reg *reg = &thread->regs;

	printf("GPRs			Flags\n");
	printf("RAX: %016ld	%016ld\n", reg->r_rax, reg->r_rflags);
	printf("RBX: %016ld	Instr Ptr\n", reg->r_rbx);
	printf("RCX: %016ld	%016ld\n", reg->r_rcx, reg->r_rip);
	printf("RDX: %016ld	Other\n", reg->r_rdx);
	printf("RBP: %016ld	cs:  %016ld\n", reg->r_rbp, reg->r_cs);
	printf("RSI: %016ld	ss:  %016ld\n", reg->r_rsi, reg->r_ss);
	printf("RDI: %016ld	rsp: %016ld\n", reg->r_rdi, reg->r_rsp);
	printf("RSP: %016ld	trapno: %08x\n", reg->r_rsp, reg->r_trapno);
	printf("R8:  %016ld	fs:	%04hx\n", reg->r_r8, reg->r_fs);
	printf("R9:  %016ld	gs:	%04hx\n", reg->r_r9, reg->r_gs);
	printf("R10: %016ld	err:	%08x\n", reg->r_r10, reg->r_err);
	printf("R11: %016ld	es:	%04hx\n", reg->r_r11, reg->r_es);
	printf("R12: %016ld	ds:	%04hx\n", reg->r_r12, reg->r_ds);
	printf("R13: %016ld\n", reg->r_r13);
	printf("R14: %016ld\n", reg->r_r14);
	printf("R15: %016ld\n", reg->r_r15);
}

static void
print_reg_hex(struct thread_info *thread) {
	struct reg *reg = &thread->regs;

	printf("GPRs			Flags\n");
	printf("RAX: %016lx	%016lx\n", reg->r_rax, reg->r_rflags);
	printf("RBX: %016lx	Instr Ptr\n", reg->r_rbx);
	printf("RCX: %016lx	%016lx\n", reg->r_rcx, reg->r_rip);
	printf("RDX: %016lx	Other\n", reg->r_rdx);
	printf("RBP: %016lx	cs:  %016lx\n", reg->r_rbp, reg->r_cs);
	printf("RSI: %016lx	ss:  %016lx\n", reg->r_rsi, reg->r_ss);
	printf("RDI: %016lx	rsp: %016lx\n", reg->r_rdi, reg->r_rsp);
	printf("RSP: %016lx	trapno: %08x\n", reg->r_rsp, reg->r_trapno);
	printf("R8:  %016lx	fs:	%04hx\n", reg->r_r8, reg->r_fs);
	printf("R9:  %016lx	gs:	%04hx\n", reg->r_r9, reg->r_gs);
	printf("R10: %016lx	err:	%08x\n", reg->r_r10, reg->r_err);
	printf("R11: %016lx	es:	%04hx\n", reg->r_r11, reg->r_es);
	printf("R12: %016lx	ds:	%04hx\n", reg->r_r12, reg->r_ds);
	printf("R13: %016lx\n", reg->r_r13);
	printf("R14: %016lx\n", reg->r_r14);
	printf("R15: %016lx\n", reg->r_r15);
}

static void
print_fpreg(struct thread_info *thread) {
	struct fpreg *fpreg = &thread->fpregs;

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
		if (i % 4 == 3)
			printf("%016lx\n", fpreg->fpr_env[i]);
		else
			printf("%016lx	", fpreg->fpr_env[i]);
}

static void
print_vmspace_info(struct vmspace_info *vmspace_info) {

	printf("swrss: %ld	", vmspace_info->vm_swrss);
	printf("tsize: %ld	", vmspace_info->vm_tsize);
	printf("dsize: %ld	", vmspace_info->vm_dsize);
	printf("ssize: %ld\n", vmspace_info->vm_ssize);
	printf("taddr: %016lx	", (uint64_t)vmspace_info->vm_taddr);
	printf("daddr: %016lx	", (uint64_t)vmspace_info->vm_daddr);
	printf("maxsaddr: %016lx\n", (uint64_t)vmspace_info->vm_maxsaddr);
	printf("nentries: %d\n", vmspace_info->nentries);
}

static void
print_entry_info(struct vm_map_entry_info *entry) {

	printf("start:	%016lx	end:	%016lx\n", entry->start, entry->end);
	printf("offset:	%016lx	eflags:	%d\n", entry->offset, entry->eflags);
	printf("prot:	%02hhx	max_prot:	%02hhx	size %lx\n", 
	    entry->protection, entry->max_protection, entry->end - entry->start);
}


static void 
print_bytes(vm_offset_t addr, size_t size, FILE *f) {
	uint8_t buf[16];

	if (size == 0)
		return;

	fread(buf, size, 1, f);

	printf("%08lx: ", addr);
	for (int i = 0; i < (size>>1); i ++)
		printf("%02x%02x ", buf[i<<1]&0xff, buf[(i<<1)+1]&0xff);
	if (size&1)
	    printf("%02x", buf[size-1]&0xff);
	printf("\n");
}

static void 
print_page(struct slspage *page) {
	int i, j;
	char c;
	vm_offset_t addr = page->vaddr;

	printf("address:	%016lx\n", addr);

	for (i = 0; i < PAGE_SIZE; i += 16) {
		printf("%012lx: ", addr+i);

		for (j = 0; j < 16; j += 2)
			printf("%02x%02x ", page->data[i + j], page->data[i + j + 1]); 

		printf(" ");

		for (j = 0; j < 16; j ++) {
			c = page->data[i + j];

			if (c >= 32 && c <= 126) 
				printf("%c", c);
			else 
				printf(".");
		}
		printf("\n");
	}
}


/*
 * XXX Used for finding the freshest version of a page out of
 * all dumps, will be rewritten to be simpler once we handle
 * multiple dumps (i.e. why traverse from the front and update
 * the page when you can traverse from the back and return the
 * first instance you find)
 *
 * Also, why aer we copying and not returning a reference?
 */
/* 
struct page 
collapsed_data(struct dump *dump, int d_idx, int e_idx, 
		vm_offset_t poffset) 
{
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
*/

static void
accept_commands(struct dump *dump) {
	struct thread_info *threads;
	struct file_info *files;
	struct vm_map_entry_info *entries;
	size_t numthreads, numfiles, numentries;
	struct slspage *page;
	int t_idx, e_idx;
	char op;
	int i;

	numthreads = dump->proc.nthreads;
	numentries = dump->memory.vmspace.nentries;
	numfiles = dump->filedesc.num_files;

	threads = dump->threads;
	entries = dump->memory.entries;
	files = dump->filedesc.infos;

	while (1) {
		print_proc_info(&dump->proc);
		printf("> ");

		op = getchar();
		if (op == EOF || op == 'q') {
			printf("Exiting...\n");
			break;
		}
		switch (op) {
		/*
		 * XXX Not yet
		 * case 'n':
		 *     dump = dump->next;
		 *     break;
		 * case 'p':
		 *     dump = dump->prev;
		 *     break;
		 */
		case 'r':
			scanf("%d", &t_idx);
			if (t_idx < dump->proc.nthreads) 
				print_reg_dec(&dump->threads[t_idx]);
			else
				printf("Invalid thread number\n");
			break;
		case 's':
			scanf("%d", &t_idx);
			if (t_idx < dump->proc.nthreads) 
				print_reg_hex(&dump->threads[t_idx]);
			else
				printf("Invalid thread number\n");
			break;
		case 'f':
			scanf("%d", &t_idx);
			if (t_idx < dump->proc.nthreads) 
				print_fpreg(&dump->threads[t_idx]);
			else
				printf("Invalid thread number\n");
			break;
		case 'v':
			print_vmspace_info(&dump->memory.vmspace);
			break;
		case 'e':
			scanf("%d", &e_idx);
			if (e_idx < numentries) 
				print_entry_info(&dump->memory.entries[e_idx]);
			else
				printf("Invalid entry number\n");
				break;
		case 'c':
			// TODO: does not work for new page added
			scanf("%d", &e_idx);
			if (e_idx < numentries) {
				for (page = pagelist[e_idx]; page != NULL; page = page->next)
					print_page(page);
			} else {
				printf("Invalid entry number\n");
			}
			break;
		case 'd':
			printf("Not yet\n");
			break;
			scanf("%d", &e_idx);
			if (e_idx < numentries) 
				print_page(pagelist[i]);
			else
				printf("Invalid entry number\n");
			break;
		case 'q':
			return;
		case 'h':
		default:
			help();
			break;
		}
		if (op != 10)
			getchar();
	}
}

int
main(int argc, char** argv) 
{
	FILE *fp;
	struct dump *dump;

	if (argc != 2) {
		printf("Usage: ./sls_user_dump <dump file>\n"); 
		return 0;
	}

	fp= fopen(argv[1], "r");
	if (fp == NULL) {
		perror("fopen");
	    return 0;
	}

	dump = sls_load_dump(fp);
	if (dump == NULL) {
		printf("Dump load failed\n");
	    return 0;
	}

	accept_commands(dump);

	/* XXX free dump routine */
	fclose(fp);

	return 0;
}
