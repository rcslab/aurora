#ifndef _SLSCTL_H_
#define _SLSCTL_H_


void checkpoint_usage(void);
int checkpoint_main(int argc, char *argv[]);

void restore_usage(void);
int restore_main(int argc, char *argv[]);

void snaplist_usage(void);
int snaplist_main(int argc, char *argv[]);

void snapdel_usage(void);
int snapdel_main(int argc, char *argv[]);

void attach_usage(void);
int attach_main(int argc, char *argv[]);

void partadd_usage(void);
int partadd_main(int argc, char *argv[]);

void partdel_usage(void);
int partdel_main(int argc, char *argv[]);

void epoch_usage(void);
int epoch_main(int argc, char *argv[]);

#endif /* _SLSCTL_H_ */

