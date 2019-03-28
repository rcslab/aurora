#ifndef _SLSCTL_H_
#define _SLSCTL_H_


void dump_usage(void);
int dump_main(int argc, char *argv[]);

void restore_usage(void);
int restore_main(int argc, char *argv[]);

void plist_usage(void);
int plist_main(int argc, char *argv[]);

void pdel_usage(void);
int pdel_main(int argc, char *argv[]);

void ckptstart_usage(void);
int ckptstart_main(int argc, char *argv[]);

void ckptstop_usage(void);
int ckptstop_main(int argc, char *argv[]);


#endif /* _SLSCTL_H_ */

