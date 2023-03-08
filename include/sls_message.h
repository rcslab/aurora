#ifndef _SLS_MESSAGE_H_
#define _SLS_MESSAGE_H_

enum slsmsgtype {
	SLSMSG_REGISTER,
	SLSMSG_CKPTSTART,
	SLSMSG_RECMETA,
	SLSMSG_RECPAGES,
	SLSMSG_CKPTDONE,
	SLSMSG_DONE,
	SLSMSG_TYPES,
};

struct slsmsg_register {
	enum slsmsgtype slsmsg_type;
	uint64_t slsmsg_oid;
};

struct slsmsg_ckptstart {
	enum slsmsgtype slsmsg_type;
	uint64_t slsmsg_epoch;
};

struct slsmsg_recmeta {
	enum slsmsgtype slsmsg_type;
	uint64_t slsmsg_uuid;
	uint64_t slsmsg_metalen;
	uint64_t slsmsg_rectype;
	uint64_t slsmsg_totalsize;
};

struct slsmsg_recpages {
	enum slsmsgtype slsmsg_type;
	uint64_t slsmsg_len;
	uint64_t slsmsg_offset;
};

struct slsmsg_ckptdone {
	enum slsmsgtype slsmsg_type;
};

struct slsmsg_done {
	enum slsmsgtype slsmsg_type;
};

union slsmsg {
	struct slsmsg_register slsmsgregister;
	struct slsmsg_ckptstart slsmsgckpt;
	struct slsmsg_recmeta slsmsgrecmeta;
	struct slsmsg_recpages slsmsgrecpages;
	struct slsmsg_ckptdone slsmsgckptdone;
	struct slsmsg_done slsmsgrecdone;
};

#endif /* _SLS_MESSAGE_H_ */
