#ifndef _SLS_IO_H_
#define _SLS_IO_H_

int slsio_open_vfs(char *name, int *fdp);
int slsio_open_sls(uint64_t oid, bool create, struct file **fpp);
int slsio_fpread(struct file *fp, void *buf, size_t size);
int slsio_fpwrite(struct file *fp, void *buf, size_t size);
int slsio_fdread(int fd, char *buf, size_t len, off_t *offp);
int slsio_fdwrite(int fd, char *buf, size_t len, off_t *offp);

#endif /* _SLS_IO_H_ */
