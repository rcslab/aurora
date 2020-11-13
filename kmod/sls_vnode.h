#ifndef _SLS_VNODE_H_
#define _SLS_VNODE_H_

#include "sls_internal.h"

bool slsckpt_vnode_istty(struct vnode *vp);
bool slsckpt_vnode_ckptbyname(struct vnode *vp);

int slsckpt_vnode(struct vnode *vp, struct slsckpt_data *sckpt_data);
int slsrest_vnode(struct slsvnode *info, struct slsrest_data *restdata);

#endif /* _SLS_VNODE_H_ */
