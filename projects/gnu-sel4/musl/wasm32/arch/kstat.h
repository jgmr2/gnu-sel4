/* The same layout as struct stat (generic bits/stat.h): the runtime writes a struct stat where
 * musl's fstatat passes its struct kstat. A timespec is 8 bytes of seconds, 4 of nanoseconds
 * and 4 of padding on wasm32 */
struct kstat {
	dev_t st_dev;
	ino_t st_ino;
	mode_t st_mode;
	nlink_t st_nlink;
	uid_t st_uid;
	gid_t st_gid;
	dev_t st_rdev;
	unsigned long long __pad;
	off_t st_size;
	blksize_t st_blksize;
	int __pad2;
	blkcnt_t st_blocks;
	long long st_atime_sec;
	long st_atime_nsec;
	int __pad3;
	long long st_mtime_sec;
	long st_mtime_nsec;
	int __pad4;
	long long st_ctime_sec;
	long st_ctime_nsec;
	int __pad5;
	unsigned __unused[2];
};
