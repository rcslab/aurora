#include <stdio.h>

#include "../kmod/slsmm.h"


void print_bytes(__vm_offset_t addr, size_t size, FILE *f) {
    if (size == 0) return;
    uint8_t buf[16];
    fread(buf, size, 1, f);

    printf("%08lx: ", addr);
    for (int i = 0; i < (size>>1); i ++)
        printf("%02x%02x ", buf[i<<1]&0xff, buf[(i<<1)+1]&0xff);
    if (size&1) printf("%02x", buf[size-1]&0xff);
    printf("\n");
}

void load_vm_page(struct vm_object_info *object, struct vm_map_entry_info *entry, FILE *f) {
    struct vm_page_info header;
    fread(&header, sizeof(struct vm_page_info), 1, f);
    printf("phys_addr %lx pagesize %lu pindex %lx\n", header.phys_addr, 
            header.pagesize, header.pindex);
    __vm_offset_t vaddr_base = entry->start + (header.pindex << 12);
    for (size_t i = 0; i < (header.pagesize >> 4); i ++)
        print_bytes(vaddr_base+(i<<4), 16, f);
    print_bytes(vaddr_base+(header.pagesize|(~0xf)), header.pagesize&0xf, f);
}

void load_vm_object(struct vm_map_entry_info *entry, FILE *f) {
    struct vm_object_info header;
    fread(&header, sizeof(struct vm_object_info), 1, f);
    printf("page_count %d\n", header.dump_page_count);

    for (int i = 0; i < header.dump_page_count; i ++)
        load_vm_page(&header, entry, f);
}

void load_vm_map_entry(FILE *f) {
    while(1) {
        struct vm_map_entry_info header;
        int error = fread(&header, sizeof(struct vm_map_entry_info), 1, f);
        if (error != 1) return;

        printf("vm_entry start %lx end %lx\n", header.start, header.end);
        load_vm_object(&header, f);
    }
}

int main(int argc, char** argv) {
    if (argc != 2) return 1;
    FILE *f = fopen(argv[1], "r");
    load_vm_map_entry(f);
    return 0;
}
