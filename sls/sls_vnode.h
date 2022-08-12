#ifndef _SLS_VNODE_H_
#define _SLS_VNODE_H_

#include "sls_internal.h"

int slsckpt_vnode_serialize(struct slsckpt_data *sckpt_data);
int slsckpt_vnode(struct vnode *vp, struct slsckpt_data *sckpt_data);
int slsvn_restore_vnode(struct slsvnode *info, struct slsckpt_data *sckpt);

#endif /* _SLS_VNODE_H_ */
