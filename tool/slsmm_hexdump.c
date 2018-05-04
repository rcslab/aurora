#include <stdio.h>

void load_vm_page(__vm_offset_t vaddr_base, FILE *f) {
    __vm_paddr_t phys_addr;
    __vm_pindex_t pindex;
    size_t pagesize;
    fread(&phys_addr, sizeof(__vm_paddr_t), 1, f);
    fread(&pindex, sizeof(__vm_pindex_t), 1, f);
    fread(&pagesize, sizeof(size_t), 1, f);
    printf("phys_addr %lx pagesize %lu pindex %lx vaddr base %lx\n", phys_addr, pagesize, pindex, vaddr_base);
    char buf[16];
    for (size_t i = 0; i < pagesize/16; i ++) {
        fread(buf, 16, 1, f);
        printf("%08lx: ", vaddr_base+(pindex<<12)+i*16);
        for (int j = 0; j < 8; j ++)
            printf("%02x%02x ", buf[j*2]&0xff, buf[j*2+1]&0xff);
        printf("\n");
    }
}

void load_vm_object(__vm_offset_t vaddr_base, FILE *f) {
    int page_count;
    fread(&page_count, sizeof(int), 1, f);
    printf("page_count %d\n", page_count);
    for (int i = 0; i < page_count; i ++)
        load_vm_page(vaddr_base, f);
}

void load_vm_map_entry(FILE *f) {
    while(1) {
        __vm_offset_t start, end;
        int error = fread(&start, sizeof(__vm_offset_t), 1, f);
        if (error != 1) return;
        fread(&end, sizeof(__vm_offset_t), 1, f);
        printf("vm_entry start %lx end %lx\n", start, end);
        load_vm_object(start, f);
    }
}

int main(int argc, char** argv) {
    if (argc != 2) return 1;
    FILE *f = fopen(argv[1], "r");
    load_vm_map_entry(f);
    return 0;
}
