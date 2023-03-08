#ifndef _PARTADD_H_
#define _PARTADD_H_

void partadd_base_usage(char *backend, struct option *opts);

void partadd_slos_usage(void);
int partadd_slos_main(int argc, char *argv[]);

void partadd_memory_usage(void);
int partadd_memory_main(int argc, char *argv[]);

void partadd_file_usage(void);
int partadd_file_main(int argc, char *argv[]);

void partadd_send_usage(void);
int partadd_send_main(int argc, char *argv[]);

void partadd_recv_usage(void);
int partadd_recv_main(int argc, char *argv[]);

#endif /* _PARTADD_H_ */
