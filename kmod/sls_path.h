#ifndef _PATH_H_
#define _PATH_H_

int sls_path_to_vn(struct sbuf *sb, struct vnode **vpp);
int sls_vn_to_path(struct vnode *vp, struct sbuf **sb);
int sls_vn_to_path_append(struct vnode *vp, struct sbuf *sb);

#endif /* _PATH_H_ */
