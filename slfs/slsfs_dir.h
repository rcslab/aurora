#ifndef _SLSFS_DIR_H_
#define _SLSFS_DIR_H_

#define SLSFS_NAME_LEN (255)

int slsfs_init_dir(struct vnode *dvp, struct vnode *vp, 
	struct componentname *);
int slsfs_add_dirent(struct vnode *vp, uint64_t ino, 
	char *nameptr, long namelen, uint8_t type);
int slsfs_lookup_name(struct vnode *vp, 
	struct componentname *name, struct dirent *dir_p);
int slsfs_unlink_dir(struct vnode *dvp, struct vnode *vp, 
	struct componentname *name);
int slsfs_dirempty(struct vnode *vp);
int slsfs_update_dirent(struct vnode *tdvp, 
    struct vnode *fvp, struct vnode *tvp);

void slsfs_declink(struct vnode *vp);

#endif /* _SLSFS_DIR_H_ */
