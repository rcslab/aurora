#ifndef _PATH_H_
#define _PATH_H_

int vnode_to_filename(struct vnode *vp, char **path, size_t *len);
int filename_to_vnode(char *path, struct vnode **vp);

#endif /* _PATH_H_ */
