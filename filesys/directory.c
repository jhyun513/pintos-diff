#include "filesys/directory.h"
#include <stdio.h>
#include <string.h>
#include <list.h>
#include "filesys/filesys.h"
#include "filesys/inode.h"
#include "threads/malloc.h"
#include "filesys/fat.h"
#include "threads/thread.h"

/* A directory. */
struct dir {
	struct inode *inode;                /* Backing store. */
	off_t pos;                          /* Current position. */
};

/* A single directory entry. */
struct dir_entry {
	disk_sector_t inode_sector;         /* Sector number of header. */
	char name[NAME_MAX + 1];            /* Null terminated file name. */
	bool in_use;                        /* In use or free? */
};

/* Creates a directory with space for ENTRY_CNT entries in the
 * given SECTOR.  Returns true if successful, false on failure. */
bool
dir_create (disk_sector_t sector, size_t entry_cnt) {
	return inode_create (sector, entry_cnt * sizeof (struct dir_entry), F_DIR);
}

/* Opens and returns the directory for the given INODE, of which
 * it takes ownership.  Returns a null pointer on failure. */
struct dir *
dir_open (struct inode *inode) {
	struct dir *dir = calloc (1, sizeof *dir);
	if (inode != NULL && dir != NULL) {
		ASSERT(inode_is_dir(inode));
		dir->inode = inode;
		dir->pos = 0;
		return dir;
	} else {
		inode_close (inode);
		free (dir);
		return NULL;
	}
}

/* Opens the root directory and returns a directory for it.
 * Return true if successful, false on failure. */
struct dir *
dir_open_root (void) {
	#ifdef EFILESYS
	return dir_open(inode_open(cluster_to_sector(ROOT_DIR_CLUSTER)));
	#else
	return dir_open (inode_open (ROOT_DIR_SECTOR));
	#endif
}

/* Opens and returns a new directory for the same inode as DIR.
 * Returns a null pointer on failure. */
struct dir *
dir_reopen (struct dir *dir) {
	return dir_open (inode_reopen (dir->inode));
}

/* Destroys DIR and frees associated resources. */
void
dir_close (struct dir *dir) {
	if (dir != NULL) {
		inode_close (dir->inode);
		free (dir);
	}
}

/* Returns the inode encapsulated by DIR. */
struct inode *
dir_get_inode (struct dir *dir) {
	return dir->inode;
}

/* Searches DIR for a file with the given NAME.
 * If successful, returns true, sets *EP to the directory entry
 * if EP is non-null, and sets *OFSP to the byte offset of the
 * directory entry if OFSP is non-null.
 * otherwise, returns false and ignores EP and OFSP. */
static bool
lookup (const struct dir *dir, const char *name,
		struct dir_entry *ep, off_t *ofsp) {
	struct dir_entry e;
	size_t ofs;

	ASSERT (dir != NULL);
	ASSERT (name != NULL);

	for (ofs = 0; inode_read_at (dir->inode, &e, sizeof e, ofs) == sizeof e;
			ofs += sizeof e)
		if (e.in_use && !strcmp (name, e.name)) {
			if (ep != NULL)
				*ep = e;
			if (ofsp != NULL)
				*ofsp = ofs;
			return true;
		}
	return false;
}

/* Searches DIR for a file with the given NAME
 * and returns true if one exists, false otherwise.
 * On success, sets *INODE to an inode for the file, otherwise to
 * a null pointer.  The caller must close *INODE. */
bool
dir_lookup (const struct dir *dir, const char *name,
		struct inode **inode) {
	struct dir_entry e;

	ASSERT (dir != NULL);
	ASSERT (name != NULL);

	if (lookup (dir, name, &e, NULL))
		*inode = inode_open (e.inode_sector);
	else
		*inode = NULL;

	return *inode != NULL;
}

/* Adds a file named NAME to DIR, which must not already contain a
 * file by that name.  The file's inode is in sector
 * INODE_SECTOR.
 * Returns true if successful, false on failure.
 * Fails if NAME is invalid (i.e. too long) or a disk or memory
 * error occurs. */
bool
dir_add (struct dir *dir, const char *name, disk_sector_t inode_sector) {
	struct dir_entry e;
	off_t ofs;
	bool success = false;

	ASSERT (dir != NULL);
	ASSERT (name != NULL);

	/* Check NAME for validity. */
	if (*name == '\0' || strlen (name) > NAME_MAX)
		return false;

	/* Check that NAME is not in use. */
	if (lookup (dir, name, NULL, NULL))
		goto done;

	/* Set OFS to offset of free slot.
	 * If there are no free slots, then it will be set to the
	 * current end-of-file.

	 * inode_read_at() will only return a short read at end of file.
	 * Otherwise, we'd need to verify that we didn't get a short
	 * read due to something intermittent such as low memory. */
	for (ofs = 0; inode_read_at (dir->inode, &e, sizeof e, ofs) == sizeof e;
			ofs += sizeof e)
		if (!e.in_use)
			break;

	/* Write slot. */
	e.in_use = true;
	strlcpy (e.name, name, sizeof e.name);
	e.inode_sector = inode_sector;
	success = inode_write_at (dir->inode, &e, sizeof e, ofs) == sizeof e;

done:
	return success;
}

/* Removes any entry for NAME in DIR.
 * Returns true if successful, false on failure,
 * which occurs only if there is no file with the given NAME. */
bool
dir_remove (struct dir *dir, const char *name) {
	struct dir *tar = NULL;
	struct dir_entry e, dent;
	struct inode *inode = NULL;
	bool success = false;
	off_t ofs;

	ASSERT (dir != NULL);
	ASSERT (name != NULL);

	/* Find directory entry. */
	if (!lookup (dir, name, &e, &ofs))
		goto done;

	/* Open inode. */
	inode = inode_open (e.inode_sector);
	if (inode == NULL)
		goto done;

	/* If file is directory. */
	if (inode_is_dir(inode)){
		if (thread_current()->working_dir &&
				inode == dir_get_inode(thread_current()->working_dir))
			return false;

		tar = dir_open(inode);
		while (inode_read_at(tar->inode, &dent, sizeof(dent), tar->pos) == sizeof(dent)) {
			tar->pos += sizeof(dent);
			if (dent.in_use && strcmp(dent.name, ".") && strcmp(dent.name, "..")) {
				dir_close(tar);
				return false;
			}
		}

		if (inode_open_cnt(inode) > 2) {
			dir_close(tar);
			return false;
		}
	}

	/* Erase directory entry. */
	e.in_use = false;
	if (inode_write_at (dir->inode, &e, sizeof e, ofs) != sizeof e)
		goto done;

	/* Remove inode. */
	inode_remove (inode);
	success = true;

done:
	inode_close (inode);
	return success;
}

/* Reads the next directory entry in DIR and stores the name in
 * NAME.  Returns true if successful, false if the directory
 * contains no more entries. */
bool
dir_readdir (struct dir *dir, char name[NAME_MAX + 1]) {
	struct dir_entry e;

	if (dir->pos == 0)
		dir->pos += sizeof(e) * 2;

	while (inode_read_at (dir->inode, &e, sizeof e, dir->pos) == sizeof e) {
		dir->pos += sizeof e;
		if (e.in_use) {
			strlcpy (name, e.name, NAME_MAX + 1);
			ASSERT(!strcmp(e.name, ".") || strcmp(e.name, ".."));
			return true;
		}
	}
	return false;
}

/* Get the name of file from the full path and store. */
bool
get_fname_from_path (const char* path, char* name) {
	char *last_slash = strrchr(path, '/');

	if (last_slash) {
		if (strlen(last_slash) > NAME_MAX + 1)
			return false;
		strlcpy(name, last_slash + 1, NAME_MAX + 1);
	}
	else {
		if (strlen(path) > NAME_MAX + 1)
			return false;
		strlcpy(name, path, NAME_MAX + 1);
	}
	return true;
}

/* Get last directory from the path and open. */
struct dir *
get_dir_from_path (const char *__path) {
	struct dir *dir = NULL;
	char *old, *path = NULL;
	char *parsing = NULL;
	char *remain = NULL;
	char *save = NULL;
	struct inode *inode = NULL;
	struct dir *working_dir = thread_current()->working_dir;

	if (strlen(__path) == 0)
		return NULL;

	path = (char *) malloc (strlen(__path) + 1);
	if (!path)
		return NULL;
	memcpy(path, __path, strlen(__path) + 1);
	old = path;

	/* Absolute path */
	if (path[0] == '/') {
		dir = dir_open_root();
		path += 1;
		if (strlen(path) == 0) {
			free(old);
			return dir;
		}
	}
	else /* Relative path */
		dir = dir_reopen(working_dir);

	parsing = strtok_r(path, "/", &save);
	remain = strtok_r(NULL, "/", &save);

	while (parsing != NULL && remain != NULL) {
		dir_lookup(dir, parsing, &inode);
		if (inode == NULL)
			goto fail;
		dir_close(dir);

		/* symlink case */
		if (inode_is_symlink(inode)) {
			char *name_file = (char *) malloc(NAME_MAX + 1);
			char *symlink_path = inode_symlink_path(inode);

			get_fname_from_path(symlink_path, name_file);
			dir = get_dir_from_path(symlink_path);
			if (dir == NULL) {
				free(name_file);
				PANIC("PATH PARSING: soft_link error");
			}
			inode_close(inode);
			dir_lookup(dir, name_file, &inode);
			free(name_file);
			if (inode == NULL)
				goto fail;
		}

		if (inode_is_dir(inode) == false)
			goto fail;

		dir = dir_open(inode);
		parsing = remain;
		remain = strtok_r(NULL, "/", &save);
	}

	free(old);
	return dir;

fail:
	free(old);
	dir_close(dir);
	inode_close(inode);
	return NULL;
}

/* Change directory if name is exist in current directory. */
bool
dir_chdir (const char* path) {
	struct inode *inode = NULL;
	struct dir *new_dir = NULL;
	char *name_dir = NULL;
	bool ret = false;

	/* Root directory */
	if (strcmp(path, "/") == 0) {
		dir_close(thread_current()->working_dir);
		thread_current()->working_dir = dir_open_root();
		ret = true;
		goto ret;
	}

	name_dir = (char *) malloc(NAME_MAX + 1);
	if (name_dir == NULL)
		goto ret;

	if (!get_fname_from_path(path, name_dir))
		goto free;

	new_dir = get_dir_from_path(path);
	if (new_dir == NULL)
		goto free;

	if (!dir_lookup(new_dir, name_dir, &inode)
		|| inode == NULL || !inode_is_dir(inode)) {
		inode_close(inode);
		goto close;
	}

	dir_close(thread_current()->working_dir);
	thread_current()->working_dir = dir_open(inode);
	ret = true;

close:
	dir_close(new_dir);
free:
	free(name_dir);
ret:
	return ret;
}

/* Make directory */
bool
dir_mkdir(const char* path) {
	struct dir *curr_dir = NULL;
	struct dir *new_dir = NULL;
	char *new_dir_name = NULL;
	struct inode *inode = NULL;
	disk_sector_t inode_sector = 0;
	bool ret = false;

	new_dir_name = (char *) malloc(NAME_MAX + 1);
	if (new_dir_name == NULL)
		return ret;

	if (!get_fname_from_path(path, new_dir_name))
		goto free;

	curr_dir = get_dir_from_path(path);
	if (curr_dir == NULL)
		goto free;

	dir_lookup(curr_dir, new_dir_name, &inode);
	if (inode != NULL) {
		inode_close(inode);
		goto close;
	}

	ret = ((inode_sector = cluster_to_sector(fat_create_chain(0)))
			&& dir_create (inode_sector, 0)
			&& dir_add (curr_dir, new_dir_name, inode_sector));

	if (!ret && inode_sector != 0)
		fat_remove_chain(sector_to_cluster(inode_sector), 0);

	new_dir = dir_open(inode_open(inode_sector));
	dir_add(new_dir, ".", inode_sector);
	dir_add(new_dir, "..", inode_get_inumber(dir_get_inode(curr_dir)));
	dir_close(new_dir);

close:
	dir_close(curr_dir);
free:
	free(new_dir_name);
	return ret;
}
