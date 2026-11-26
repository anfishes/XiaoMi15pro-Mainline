#define _GNU_SOURCE
#include <dirent.h>
#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/bsg.h>
#include <linux/qrtr.h>
#include <pthread.h>
#include <scsi/scsi_bsg_ufs.h>
#include <scsi/sg.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifndef AF_QIPCRTR
#define AF_QIPCRTR 42
#endif

/* ---- Linux qcomtee UAPI ---- */
#define TEE_IOC_MAGIC 0xa4
#define TEE_IOC_BASE 0

struct tee_ioctl_buf_data { uint64_t buf_ptr; uint64_t buf_len; };
struct tee_ioctl_shm_alloc_data { uint64_t size; uint32_t flags; int32_t id; };
struct tee_ioctl_param { uint64_t attr; uint64_t a; uint64_t b; uint64_t c; };

struct tee_ioctl_object_invoke_arg {
    uint64_t id;
    uint32_t op;
    uint32_t ret;
    uint32_t num_params;
    uint32_t pad;
    struct tee_ioctl_param params[];
};
struct tee_iocl_supp_recv_arg {
    uint32_t func;
    uint32_t num_params;
    struct tee_ioctl_param params[];
};
struct tee_iocl_supp_send_arg {
    uint32_t ret;
    uint32_t num_params;
    struct tee_ioctl_param params[];
};

#define TEE_IOC_SHM_ALLOC     _IOWR(TEE_IOC_MAGIC, TEE_IOC_BASE + 1, struct tee_ioctl_shm_alloc_data)
#define TEE_IOC_SUPPL_RECV    _IOR(TEE_IOC_MAGIC, TEE_IOC_BASE + 6, struct tee_ioctl_buf_data)
#define TEE_IOC_SUPPL_SEND    _IOR(TEE_IOC_MAGIC, TEE_IOC_BASE + 7, struct tee_ioctl_buf_data)
#define TEE_IOC_OBJECT_INVOKE _IOR(TEE_IOC_MAGIC, TEE_IOC_BASE + 10, struct tee_ioctl_buf_data)

#define TEE_OBJREF_NULL     ((uint64_t)-1)
#define TEE_ATTR_VALUE_OUT  2
#define TEE_ATTR_VALUE_IO   3
#define TEE_ATTR_UBUF_IN    8
#define TEE_ATTR_UBUF_OUT   9
#define TEE_ATTR_OBJREF_IN  11
#define TEE_ATTR_OBJREF_OUT 12
#define TEE_ATTR_META       0x100

#define QCOMTEE_OBJREF_FLAG_USER (1ULL << 1)
#define QCOMTEE_OBJREF_FLAG_MEM  (1ULL << 2)
#define QCOMTEE_OP_RELEASE       0xffff

/* ---- QMI Service 1059 (QWES License Manager) ---- */
#define QMI_QWES_SERVICE_ID 1059 /* 0x423 */
#define QMI_QWES_VERSION    1
#define QMI_QWES_INSTANCE   0
#define QMI_SECCHAN_SVC_ID  85   /* 0x55 */

#define QMI_TYPE_REQ  0x00
#define QMI_TYPE_RESP 0x02
#define QMI_TYPE_IND  0x04

static volatile sig_atomic_t g_stop = 0;
static void on_sig(int s) { (void)s; g_stop = 1; }

static int g_tee_fd = -1;
static pthread_mutex_t g_fs_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_rpmb_lock = PTHREAD_MUTEX_INITIALIZER;

/* ---- Strictly Read-Only UFS RPMB ---- */
#define RPMB_BLK_SIZE        512
#define SCSI_REQ_SENSE_ID    0x03
#define SCSI_SEC_IN_ID       0xA2
#define SCSI_SEC_OUT_ID      0xB5
#define SCSI_SEC_PROT        0xEC
#define SCSI_SEC_UFS_PROT_ID 0x0001
#define UPIU_RPMB_LUN        0xC4
#define UFS_DWORD(b3,b2,b1,b0) htobe32(((uint32_t)(b3)<<24)|((uint32_t)(b2)<<16)|((uint32_t)(b1)<<8)|(b0))

static uint32_t g_rpmb_size = 0;
static uint32_t g_rpmb_rel_wr = 1;
static int g_rpmb_ready = 0;
static char g_rpmb_bsg_path[128] = "/dev/bsg/0:0:0:49476";

static void find_rpmb_bsg_dev(void) {
    DIR *d = opendir("/dev/bsg");
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        size_t l = strlen(de->d_name);
        if (l > 6 && strcmp(de->d_name + l - 6, ":49476") == 0) {
            snprintf(g_rpmb_bsg_path, sizeof(g_rpmb_bsg_path), "/dev/bsg/%.100s", de->d_name);
            break;
        }
    }
    closedir(d);
}

static int ufs_bsg_query_read_desc(int fd, uint8_t *buf, uint16_t len, uint8_t idn, uint8_t index) {
    struct ufs_bsg_request req;
    struct ufs_bsg_reply rsp;
    memset(&req, 0, sizeof(req));
    memset(&rsp, 0, sizeof(rsp));
    req.msgcode = 0x16;
    req.upiu_req.header.dword_0 = UFS_DWORD(0x16, 0, 0, 0);
    req.upiu_req.header.dword_1 = UFS_DWORD(0, 0x01, 0, 0);
    req.upiu_req.header.dword_2 = UFS_DWORD(0, 0, len >> 8, len & 0xff);
    req.upiu_req.qr.opcode = 0x01;
    req.upiu_req.qr.idn = idn;
    req.upiu_req.qr.index = index;
    req.upiu_req.qr.selector = 0;
    req.upiu_req.qr.length = htobe16(len);

    struct sg_io_v4 sg;
    memset(&sg, 0, sizeof(sg));
    sg.guard = 'Q';
    sg.protocol = BSG_PROTOCOL_SCSI;
    sg.subprotocol = BSG_SUB_PROTOCOL_SCSI_TRANSPORT;
    sg.request_len = sizeof(req);
    sg.request = (uint64_t)(uintptr_t)&req;
    sg.response = (uint64_t)(uintptr_t)&rsp;
    sg.max_response_len = sizeof(rsp);
    sg.din_xfer_len = len;
    sg.din_xferp = (uint64_t)(uintptr_t)buf;
    int ret = ioctl(fd, SG_IO, &sg);
    if (ret || sg.info || rsp.result) return -1;
    return 0;
}

static int scsi_bsg_cmd(int fd, uint8_t *cdb, uint8_t cdb_len, void *buf, uint32_t len, int to_dev) {
    uint8_t sense[96] = {0};
    struct sg_io_v4 sg;
    memset(&sg, 0, sizeof(sg));
    sg.guard = 'Q';
    sg.protocol = BSG_PROTOCOL_SCSI;
    sg.subprotocol = BSG_SUB_PROTOCOL_SCSI_CMD;
    sg.request_len = cdb_len;
    sg.request = (uint64_t)(uintptr_t)cdb;
    sg.response = (uint64_t)(uintptr_t)sense;
    sg.max_response_len = sizeof(sense);
    if (to_dev) {
        sg.dout_xfer_len = len;
        sg.dout_xferp = (uint64_t)(uintptr_t)buf;
    } else {
        sg.din_xfer_len = len;
        sg.din_xferp = (uint64_t)(uintptr_t)buf;
    }
    int ret = ioctl(fd, SG_IO, &sg);
    if (ret || sg.info) return -1;
    return 0;
}

static int rpmb_ro_init(void) {
    if (g_rpmb_ready) return 0;
    find_rpmb_bsg_dev();
    int ufd = open("/dev/bsg/ufs-bsg0", O_RDWR | O_CLOEXEC);
    if (ufd < 0) { perror("open /dev/bsg/ufs-bsg0"); return -1; }
    uint8_t dev_desc[0x40] = {0}, geo_desc[0x48] = {0}, unit_desc[0x23] = {0};
    int rc = ufs_bsg_query_read_desc(ufd, dev_desc, sizeof(dev_desc), 0x0, 0);
    if (!rc) rc = ufs_bsg_query_read_desc(ufd, geo_desc, sizeof(geo_desc), 0x7, 0);
    if (!rc) rc = ufs_bsg_query_read_desc(ufd, unit_desc, sizeof(unit_desc), 0x2, UPIU_RPMB_LUN);
    close(ufd);
    if (rc) return -1;

    uint16_t spec = ((uint16_t)dev_desc[16] << 8) | dev_desc[17];
    g_rpmb_rel_wr = geo_desc[23] ? geo_desc[23] : 1;
    if (spec < 0x300) {
        uint32_t blks = ((uint32_t)unit_desc[15]<<24)|((uint32_t)unit_desc[16]<<16)|
                        ((uint32_t)unit_desc[17]<<8)|unit_desc[18];
        g_rpmb_size = blks / 2;
    } else {
        g_rpmb_size = (uint32_t)unit_desc[19] * 256;
    }

    int rfd = open(g_rpmb_bsg_path, O_RDWR | O_SYNC | O_CLOEXEC);
    if (rfd < 0) { perror("open rpmb bsg"); return -1; }
    uint8_t cdb[6] = { SCSI_REQ_SENSE_ID, 0, 0, 0, 18, 0 };
    uint8_t sbuf[18] = {0};
    rc = scsi_bsg_cmd(rfd, cdb, 6, sbuf, 18, 0);
    close(rfd);
    if (rc) return -1;
    g_rpmb_ready = 1;
    printf("qwes-bridge: UFS RPMB (RO) ready (%s, spec=0x%x size=%u rel_wr=%u)\n",
           g_rpmb_bsg_path, spec, g_rpmb_size, g_rpmb_rel_wr);
    return 0;
}

static int rpmb_ro_read(const uint8_t *req_in, uint32_t blk_cnt, uint8_t *resp_out, uint32_t *resp_len) {
    if (rpmb_ro_init() < 0) return -1;
    if (blk_cnt == 0 || blk_cnt > 38) return -1;

    uint32_t num_trans = (blk_cnt + g_rpmb_rel_wr - 1) / g_rpmb_rel_wr;
    for (uint32_t i = 0; i < num_trans; i++) {
        const uint8_t *f = req_in + i * RPMB_BLK_SIZE;
        uint16_t req_type = ((uint16_t)f[510] << 8) | f[511];
        if (req_type != 0x0002 && req_type != 0x0004 && req_type != 0x0005) {
            fprintf(stderr, "qwes-bridge: SAFETY BLOCK non-read RPMB frame 0x%04x\n", req_type);
            return -1;
        }
    }

    uint8_t *req_copy = malloc(num_trans * RPMB_BLK_SIZE);
    if (!req_copy) return -1;
    memcpy(req_copy, req_in, num_trans * RPMB_BLK_SIZE);

    int rfd = open(g_rpmb_bsg_path, O_RDWR | O_SYNC | O_CLOEXEC);
    if (rfd < 0) { free(req_copy); return -1; }

    int ret = 0;
    uint32_t rem = blk_cnt;
    uint8_t *dst = resp_out;
    for (uint32_t i = 0; i < num_trans; i++) {
        uint32_t cur = rem > g_rpmb_rel_wr ? g_rpmb_rel_wr : rem;
        uint32_t nbytes = cur * RPMB_BLK_SIZE;
        uint8_t cdb_out[12] = {
            SCSI_SEC_OUT_ID, SCSI_SEC_PROT,
            (SCSI_SEC_UFS_PROT_ID >> 8) & 0xff, SCSI_SEC_UFS_PROT_ID & 0xff,
            0, 0,
            (RPMB_BLK_SIZE >> 24) & 0xff, (RPMB_BLK_SIZE >> 16) & 0xff,
            (RPMB_BLK_SIZE >> 8) & 0xff, RPMB_BLK_SIZE & 0xff,
            0, 0
        };
        ret = scsi_bsg_cmd(rfd, cdb_out, 12, req_copy + i * RPMB_BLK_SIZE, RPMB_BLK_SIZE, 1);
        if (ret) break;

        uint8_t cdb_in[12] = {
            SCSI_SEC_IN_ID, SCSI_SEC_PROT,
            (SCSI_SEC_UFS_PROT_ID >> 8) & 0xff, SCSI_SEC_UFS_PROT_ID & 0xff,
            0, 0,
            (nbytes >> 24) & 0xff, (nbytes >> 16) & 0xff,
            (nbytes >> 8) & 0xff, nbytes & 0xff,
            0, 0
        };
        ret = scsi_bsg_cmd(rfd, cdb_in, 12, dst, nbytes, 0);
        if (ret) break;
        dst += nbytes;
        rem -= cur;
    }
    close(rfd);
    free(req_copy);
    *resp_len = blk_cnt * RPMB_BLK_SIZE;
    return ret;
}

typedef struct { uint32_t cmd_id, num_sectors, req_buff_len, req_buff_offset, version, rel_wr_count; } __attribute__((packed)) rpmb_rw_req_t;
typedef struct { uint32_t cmd_id; int32_t status; uint32_t res_buff_len, res_buff_offset, version; } __attribute__((packed)) rpmb_rw_rsp_t;

static int handle_rpmb(uint8_t *buf, size_t len) {
    (void)len;
    pthread_mutex_lock(&g_rpmb_lock);
    uint32_t cmd_id = *(uint32_t *)buf;
    int rc = 0;
    if (cmd_id == 0x101) {
        uint32_t ver = ((uint32_t *)buf)[1];
        int st = rpmb_ro_init();
        memset(buf, 0, 64);
        uint32_t *out = (uint32_t *)buf;
        out[0] = cmd_id; out[1] = ver >= 2 ? 2 : 1; out[2] = (uint32_t)st;
        out[3] = g_rpmb_size; out[4] = g_rpmb_rel_wr;
        if (ver >= 2) out[5] = 9;
    } else if (cmd_id == 0x102) {
        rpmb_rw_req_t req; memcpy(&req, buf, sizeof(req));
        uint32_t resp_len = 0;
        int st = rpmb_ro_read(buf + req.req_buff_offset, req.num_sectors, buf + sizeof(rpmb_rw_rsp_t), &resp_len);
        rpmb_rw_rsp_t rsp = { req.cmd_id, st, resp_len, sizeof(rpmb_rw_rsp_t), req.version };
        memcpy(buf, &rsp, sizeof(rsp));
    } else if (cmd_id == 0x103) {
        rpmb_rw_req_t req; memcpy(&req, buf, sizeof(req));
        fprintf(stderr, "qwes-bridge: SAFETY BLOCKED RPMB WRITE sectors=%u\n", req.num_sectors);
        rpmb_rw_rsp_t rsp = { req.cmd_id, -1, 0, sizeof(rpmb_rw_rsp_t), req.version };
        memcpy(buf, &rsp, sizeof(rsp));
    } else if (cmd_id == 0x104) {
        uint32_t *out = (uint32_t *)buf;
        out[0] = cmd_id; out[1] = (uint32_t)-1; out[2] = 0; out[3] = 16;
    } else {
        rc = -1;
    }
    pthread_mutex_unlock(&g_rpmb_lock);
    return rc;
}

/* ---- Safe local-copy path mapping (/var/lib/haotian-tee/) ---- */
static int map_local_path(const char *orig, int is_persist, char *out, size_t out_sz) {
    if (!orig || !orig[0] || strstr(orig, "..") != NULL) return -1;
    const char *p = orig;
    if (strncmp(p, "/mnt/vendor/persist/", 20) == 0) { p += 20; is_persist = 1; }
    else if (strncmp(p, "/persist/", 9) == 0) { p += 9; is_persist = 1; }
    else if (strncmp(p, "/data/vendor/", 13) == 0) { p += 13; is_persist = 0; }
    else if (strncmp(p, "/data/", 6) == 0) { p += 6; is_persist = 0; }
    while (*p == '/') p++;
    if (is_persist) {
        if (strncmp(p, "data/", 5) == 0)
            snprintf(out, out_sz, "/var/lib/haotian-tee/persist/%s", p);
        else
            snprintf(out, out_sz, "/var/lib/haotian-tee/persist/data/%s", p);
    } else {
        snprintf(out, out_sz, "/var/lib/haotian-tee/data/%s", p);
    }
    return 0;
}

static int handle_gpfs(uint8_t *buf, size_t len) {
    (void)len;
    uint32_t cmd = *(uint32_t *)buf;
    if (cmd == 0x0c) {
        uint32_t *out = (uint32_t *)buf;
        out[0] = cmd; out[1] = 2; out[2] = 0;
        return 0;
    }
    if (cmd == 0x04 || cmd == 0x08) {
        char orig[256]; memcpy(orig, buf + 4, 256); orig[255] = 0;
        int32_t off; uint32_t cnt;
        memcpy(&off, buf + 260, 4); memcpy(&cnt, buf + 264, 4);
        char mapped[512];
        int err = 0; uint32_t nread = 0;
        if (map_local_path(orig, cmd == 0x08, mapped, sizeof(mapped)) < 0) {
            err = EACCES;
        } else {
            int fd = open(mapped, O_RDONLY | O_CLOEXEC);
            if (fd < 0) err = errno;
            else {
                if (lseek(fd, off, SEEK_SET) != off) err = errno;
                else {
                    if (cnt > 500 * 1024) cnt = 500 * 1024;
                    ssize_t n = read(fd, buf + 12, cnt);
                    if (n < 0) err = errno; else nread = (uint32_t)n;
                }
                close(fd);
            }
        }
        *(uint32_t *)(buf + 0) = cmd;
        *(int32_t *)(buf + 4) = err;
        *(uint32_t *)(buf + 8) = nread;
        return 0;
    }
    *(uint32_t *)(buf + 0) = cmd;
    *(int32_t *)(buf + 4) = EACCES;
    return 0;
}

static int g_fs_fds[32];
static DIR *g_fs_dirs[16];
static int g_fs_last_errno = 0;

typedef struct {
    uint64_t d_ino; int64_t d_off; uint16_t d_reclen; uint8_t d_type; char d_name[256];
} __attribute__((packed)) tz_dirent_t;

typedef struct {
    uint64_t st_dev; uint8_t pad0[4]; uint32_t __st_ino;
    uint32_t st_mode, st_nlink, st_uid, st_gid;
    uint64_t st_rdev; uint8_t pad3[4]; int64_t st_size;
    uint32_t st_blksize; uint64_t st_blocks;
    uint32_t st_atim, st_atim_ns, st_mtim, st_mtim_ns, st_ctim, st_ctim_ns;
    uint64_t st_ino;
} __attribute__((packed)) tz_stat_t;

static int handle_fs(uint8_t *buf, size_t len) {
    (void)len;
    pthread_mutex_lock(&g_fs_lock);
    uint32_t cmd = *(uint32_t *)buf;
    int ret = -1;
    size_t res_off = 4;

    if (cmd == 0x202) { // FILE_OPEN
        char orig[256]; memcpy(orig, buf + 4, 256); orig[255] = 0;
        int flags; memcpy(&flags, buf + 260, 4);
        char mapped[512];
        if (map_local_path(orig, strstr(orig, "persist") != NULL, mapped, sizeof(mapped)) < 0) {
            errno = EACCES;
        } else {
            int slot = -1;
            for (int i = 0; i < 32; i++) if (g_fs_fds[i] < 0) { slot = i; break; }
            if (slot >= 0) {
                int fd = open(mapped, flags | O_CLOEXEC, 0600);
                if (fd >= 0) { g_fs_fds[slot] = fd; ret = slot + 100; }
            } else errno = EMFILE;
        }
    } else if (cmd == 0x207) { // FILE_READ
        int h; uint32_t cnt;
        memcpy(&h, buf + 4, 4); memcpy(&cnt, buf + 8, 4);
        res_off = 4 + 20000;
        if (h >= 100 && h < 132 && g_fs_fds[h - 100] >= 0) {
            if (cnt > 20000) cnt = 20000;
            ret = (int)read(g_fs_fds[h - 100], buf + 4, cnt);
        } else errno = EBADF;
    } else if (cmd == 0x208) { // FILE_WRITE (only within /var/lib/haotian-tee/)
        int h; uint32_t cnt;
        memcpy(&h, buf + 4, 4); memcpy(&cnt, buf + 4 + 4 + 20000, 4);
        if (h >= 100 && h < 132 && g_fs_fds[h - 100] >= 0 && cnt <= 20000) {
            ret = (int)write(g_fs_fds[h - 100], buf + 8, cnt);
        } else errno = EBADF;
    } else if (cmd == 0x209) { // FILE_CLOSE
        int h; memcpy(&h, buf + 4, 4);
        if (h >= 100 && h < 132 && g_fs_fds[h - 100] >= 0) {
            ret = close(g_fs_fds[h - 100]);
            g_fs_fds[h - 100] = -1;
        } else errno = EBADF;
    } else if (cmd == 0x20a) { // FILE_LSEEK
        int h; int32_t off; int whence;
        memcpy(&h, buf + 4, 4); memcpy(&off, buf + 8, 4); memcpy(&whence, buf + 12, 4);
        if (h >= 100 && h < 132 && g_fs_fds[h - 100] >= 0)
            ret = (int)lseek(g_fs_fds[h - 100], off, whence);
        else errno = EBADF;
    } else if (cmd == 0x20e || cmd == 0x20f) { // FSTAT / LSTAT
        struct stat st; memset(&st, 0, sizeof(st));
        if (cmd == 0x20e) {
            int h; memcpy(&h, buf + 4, 4);
            if (h >= 100 && h < 132 && g_fs_fds[h - 100] >= 0) ret = fstat(g_fs_fds[h - 100], &st);
            else errno = EBADF;
        } else {
            char orig[256]; memcpy(orig, buf + 4, 256); orig[255] = 0;
            char mapped[512];
            if (map_local_path(orig, strstr(orig, "persist") != NULL, mapped, sizeof(mapped)) == 0)
                ret = lstat(mapped, &st);
            else errno = EACCES;
        }
        tz_stat_t ts; memset(&ts, 0, sizeof(ts));
        if (ret == 0) {
            ts.st_mode = st.st_mode; ts.st_nlink = st.st_nlink;
            ts.st_size = st.st_size; ts.st_blksize = st.st_blksize; ts.st_blocks = st.st_blocks;
        }
        memcpy(buf + 4, &ts, sizeof(ts));
        res_off = 4 + sizeof(ts);
    } else if (cmd == 0x20c || cmd == 0x213) { // UNLINK / REMOVE (strictly inside /var/lib/haotian-tee/)
        char orig[256]; memcpy(orig, buf + 4, 256); orig[255] = 0;
        char mapped[512];
        if (map_local_path(orig, strstr(orig, "persist") != NULL, mapped, sizeof(mapped)) == 0)
            ret = unlink(mapped);
        else errno = EACCES;
    } else if (cmd == 0x210) { // MKDIR (strictly inside /var/lib/haotian-tee/)
        char orig[256]; memcpy(orig, buf + 4, 256); orig[255] = 0;
        char mapped[512];
        if (map_local_path(orig, strstr(orig, "persist") != NULL, mapped, sizeof(mapped)) == 0) {
            ret = mkdir(mapped, 0700);
            if (ret < 0 && errno == EEXIST) ret = 0;
        } else errno = EACCES;
    } else if (cmd == 0x211) { // TESTDIR
        char orig[256]; memcpy(orig, buf + 4, 256); orig[255] = 0;
        char mapped[512];
        if (map_local_path(orig, strstr(orig, "persist") != NULL, mapped, sizeof(mapped)) == 0) {
            DIR *d = opendir(mapped);
            if (d) { closedir(d); ret = 0; }
        } else errno = EACCES;
    } else if (cmd == 0x216) { // SYNC
        int h; memcpy(&h, buf + 4, 4);
        if (h >= 100 && h < 132 && g_fs_fds[h - 100] >= 0) ret = fsync(g_fs_fds[h - 100]);
        else errno = EBADF;
    } else if (cmd == 0x217) { // RENAME (strictly inside /var/lib/haotian-tee/)
        char o1[256], o2[256];
        memcpy(o1, buf + 4, 256); o1[255] = 0;
        memcpy(o2, buf + 260, 256); o2[255] = 0;
        char m1[512], m2[512];
        if (map_local_path(o1, strstr(o1, "persist") != NULL, m1, sizeof(m1)) == 0 &&
            map_local_path(o2, strstr(o2, "persist") != NULL, m2, sizeof(m2)) == 0) {
            ret = rename(m1, m2);
        } else errno = EACCES;
    } else if (cmd == 0x219) { // DIR_OPEN
        char orig[256]; memcpy(orig, buf + 4, 256); orig[255] = 0;
        char mapped[512];
        uint64_t handle = 0;
        if (map_local_path(orig, strstr(orig, "persist") != NULL, mapped, sizeof(mapped)) == 0) {
            int slot = -1;
            for (int i = 0; i < 16; i++) if (!g_fs_dirs[i]) { slot = i; break; }
            DIR *d = (slot >= 0) ? opendir(mapped) : NULL;
            if (d) { g_fs_dirs[slot] = d; handle = (uint64_t)(slot + 200); ret = 0; }
            else ret = -3;
        } else { errno = EACCES; ret = -2; }
        memcpy(buf + 4, &handle, 8);
        res_off = 12;
    } else if (cmd == 0x21a) { // DIR_READ
        uint64_t handle; memcpy(&handle, buf + 4, 8);
        tz_dirent_t td; memset(&td, 0, sizeof(td));
        if (handle >= 200 && handle < 216 && g_fs_dirs[handle - 200]) {
            struct dirent *de = readdir(g_fs_dirs[handle - 200]);
            if (de) {
                td.d_ino = (uint64_t)de->d_ino;
                td.d_off = (int64_t)de->d_off;
                td.d_reclen = (uint16_t)de->d_reclen;
                td.d_type = (uint8_t)de->d_type;
                snprintf(td.d_name, sizeof(td.d_name), "%s", de->d_name);
            }
            ret = 0;
        } else ret = -2;
        memcpy(buf + 4, &td, sizeof(td));
        res_off = 4 + sizeof(td);
    } else if (cmd == 0x21b) { // DIR_CLOSE
        uint64_t handle; memcpy(&handle, buf + 4, 8);
        if (handle >= 200 && handle < 216 && g_fs_dirs[handle - 200]) {
            ret = closedir(g_fs_dirs[handle - 200]);
            g_fs_dirs[handle - 200] = NULL;
        } else ret = -2;
    } else if (cmd == 0x21c) { // GET_ERRNO
        ret = g_fs_last_errno;
    } else {
        fprintf(stderr, "qwes-bridge: FS unhandled cmd=0x%x\n", cmd);
        errno = ENOSYS;
    }
    if (ret < 0) g_fs_last_errno = errno;
    *(uint32_t *)buf = cmd;
    memcpy(buf + res_off, &ret, 4);
    pthread_mutex_unlock(&g_fs_lock);
    return 0;
}

static int handle_time(uint8_t *buf) {
    uint32_t cmd = *(uint32_t *)buf;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    memset(buf, 0, 128);
    *(uint32_t *)buf = cmd;
    if (cmd == 0x302 || cmd == 0x305) {
        uint32_t *out = (uint32_t *)buf;
        out[1] = (uint32_t)ts.tv_sec;
        out[2] = (uint32_t)ts.tv_nsec;
        out[3] = 0;
    } else if (cmd == 0x303) {
        struct tm tm; gmtime_r(&ts.tv_sec, &tm);
        int *out = (int *)(buf + 4);
        out[0]=tm.tm_sec; out[1]=tm.tm_min; out[2]=tm.tm_hour; out[3]=tm.tm_mday;
        out[4]=tm.tm_mon; out[5]=tm.tm_year; out[6]=tm.tm_wday; out[7]=tm.tm_yday; out[8]=tm.tm_isdst;
        out[9]=0;
    } else if (cmd == 0x304) {
        unsigned long ms = (unsigned long)ts.tv_sec * 1000UL + (unsigned long)(ts.tv_nsec / 1000000L);
        memcpy(buf + 4, &ms, sizeof(ms));
    }
    return 0;
}

struct listener_slot {
    uint32_t svc_id;
    const char *name;
    size_t buf_size;
    int shm_fd;
    int32_t shm_id;
    void *shm_addr;
    uint64_t cb_id;
};

static struct listener_slot g_listeners[] = {
    { 0x000a, "FS",   20 * 1024,  -1, -1, NULL, 1001 },
    { 0x000b, "TIME", 20 * 1024,  -1, -1, NULL, 1002 },
    { 0x2000, "RPMB", 20 * 1024,  -1, -1, NULL, 1003 },
    { 0x7000, "GPFS", 504 * 1024, -1, -1, NULL, 1004 },
};
#define NUM_LISTENERS (sizeof(g_listeners) / sizeof(g_listeners[0]))

static int tee_invoke(int fd, uint64_t obj_id, uint32_t op,
                      struct tee_ioctl_param *params, uint32_t nparams,
                      uint32_t *svc_ret) {
    size_t sz = sizeof(struct tee_ioctl_object_invoke_arg) + nparams * sizeof(struct tee_ioctl_param);
    struct tee_ioctl_object_invoke_arg *arg = calloc(1, sz);
    if (!arg) return -ENOMEM;
    arg->id = obj_id; arg->op = op; arg->num_params = nparams;
    if (nparams) memcpy(arg->params, params, nparams * sizeof(struct tee_ioctl_param));
    struct tee_ioctl_buf_data buf = { .buf_ptr = (uint64_t)(uintptr_t)arg, .buf_len = sz };
    int rc = ioctl(fd, TEE_IOC_OBJECT_INVOKE, &buf);
    int saved_err = errno;
    if (rc == 0) {
        if (svc_ret) *svc_ret = arg->ret;
        if (nparams) memcpy(params, arg->params, nparams * sizeof(struct tee_ioctl_param));
    }
    free(arg);
    if (rc < 0) { errno = saved_err; return -1; }
    return 0;
}

static void *supp_worker(void *arg) {
    (void)arg;
    uint8_t ubuf[8192] __attribute__((aligned(8)));
    size_t max_params = 16;
    size_t recv_sz = sizeof(struct tee_iocl_supp_recv_arg) + max_params * sizeof(struct tee_ioctl_param);
    size_t send_sz = sizeof(struct tee_iocl_supp_send_arg) + max_params * sizeof(struct tee_ioctl_param);
    struct tee_iocl_supp_recv_arg *rarg = calloc(1, recv_sz);
    struct tee_iocl_supp_send_arg *sarg = calloc(1, send_sz);

    while (!g_stop) {
        memset(rarg, 0, recv_sz);
        rarg->num_params = max_params;
        rarg->params[0].attr = TEE_ATTR_VALUE_IO | TEE_ATTR_META;
        rarg->params[0].a = (uint64_t)(uintptr_t)ubuf;
        rarg->params[0].b = sizeof(ubuf);
        struct tee_ioctl_buf_data rbuf = { .buf_ptr = (uint64_t)(uintptr_t)rarg, .buf_len = recv_sz };
        int rc = ioctl(g_tee_fd, TEE_IOC_SUPPL_RECV, &rbuf);
        if (rc < 0) {
            if (errno == EINTR) continue;
            break;
        }
        uint64_t obj_id = rarg->params[0].a;
        uint64_t req_id = rarg->params[0].b;
        uint32_t op = rarg->func;
        uint32_t np = rarg->num_params;
        if (op == QCOMTEE_OP_RELEASE) continue;

        struct listener_slot *ls = NULL;
        for (size_t i = 0; i < NUM_LISTENERS; i++)
            if (g_listeners[i].cb_id == obj_id) { ls = &g_listeners[i]; break; }

        memset(sarg, 0, send_sz);
        sarg->num_params = np;
        sarg->params[0].attr = TEE_ATTR_VALUE_OUT | TEE_ATTR_META;
        sarg->params[0].a = req_id;
        uint32_t is64 = 1;
        uint32_t ret_code = 0;

        if (op == 1 && np == 1) {
            ret_code = 0;
        } else if (op == 0 && np == 7 && ls) {
            for (uint32_t i = 1; i < np; i++) sarg->params[i].attr = rarg->params[i].attr;
            sarg->params[1].a = 0; sarg->params[1].b = 0;
            sarg->params[2].a = (uint64_t)(uintptr_t)&is64; sarg->params[2].b = 4;
            for (uint32_t i = 3; i < 7; i++) { sarg->params[i].a = TEE_OBJREF_NULL; sarg->params[i].b = 0; }
            int drc = -1;
            if (ls->svc_id == 0x2000) drc = handle_rpmb(ls->shm_addr, ls->buf_size);
            else if (ls->svc_id == 0x7000) drc = handle_gpfs(ls->shm_addr, ls->buf_size);
            else if (ls->svc_id == 0x000a) drc = handle_fs(ls->shm_addr, ls->buf_size);
            else if (ls->svc_id == 0x000b) drc = handle_time(ls->shm_addr);
            ret_code = (drc == 0) ? 0 : 2;
        } else {
            for (uint32_t i = 1; i < np; i++) sarg->params[i] = rarg->params[i];
            ret_code = 2;
        }

        sarg->ret = ret_code;
        struct tee_ioctl_buf_data sbuf = { .buf_ptr = (uint64_t)(uintptr_t)sarg, .buf_len = send_sz };
        if (ioctl(g_tee_fd, TEE_IOC_SUPPL_SEND, &sbuf) < 0 && !g_stop)
            perror("TEE_IOC_SUPPL_SEND");
    }
    free(rarg); free(sarg);
    return NULL;
}

static void hexdump(const char *prefix, const uint8_t *buf, size_t len) {
    printf("%s (%zu bytes):", prefix, len);
    size_t n = len > 48 ? 48 : len;
    for (size_t i = 0; i < n; i++) printf(" %02x", buf[i]);
    if (len > n) printf(" ...");
    printf("\n");
}

static int ipfm_check_license(int tee_fd, uint64_t ipfm_id, int is_secure,
                              const uint8_t *in_buf, size_t in_len,
                              uint8_t *out_buf, size_t *out_len, int *svc_err) {
    uint32_t op = is_secure ? 4 : 7;
    for (int attempt = 0; attempt < 5; attempt++) {
        struct tee_ioctl_param p[2] = {
            { .attr = TEE_ATTR_UBUF_IN,  .a = (uint64_t)(uintptr_t)in_buf,  .b = in_len },
            { .attr = TEE_ATTR_UBUF_OUT, .a = (uint64_t)(uintptr_t)out_buf, .b = 1024 }
        };
        uint32_t sret = 0xffffffff;
        if (tee_invoke(tee_fd, ipfm_id, op, p, 2, &sret) < 0) {
            perror("tee_invoke IPFM");
            *svc_err = -errno;
            return -1;
        }
        *out_len = (size_t)p[1].b;
        *svc_err = (int32_t)sret;
        if ((int32_t)sret == -99) {
            sleep(1);
            continue;
        }
        return (sret == 0) ? 0 : -1;
    }
    return -1;
}

static int send_qmi(int sock, const struct sockaddr_qrtr *peer,
                    uint8_t type, uint16_t txn_id, uint16_t msg_id,
                    const uint8_t *tlvs, uint16_t tlv_len) {
    uint8_t pkt[2048];
    if (7u + tlv_len > sizeof(pkt)) return -1;
    pkt[0] = type;
    pkt[1] = txn_id & 0xff;
    pkt[2] = (txn_id >> 8) & 0xff;
    pkt[3] = msg_id & 0xff;
    pkt[4] = (msg_id >> 8) & 0xff;
    pkt[5] = tlv_len & 0xff;
    pkt[6] = (tlv_len >> 8) & 0xff;
    if (tlv_len) memcpy(pkt + 7, tlvs, tlv_len);
    ssize_t n = sendto(sock, pkt, 7u + tlv_len, 0,
                       (const struct sockaddr *)peer, sizeof(*peer));
    if (n < 0) { perror("sendto qmi"); return -1; }
    return 0;
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IOLBF, 0);
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_sig;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    for (int i = 0; i < 32; i++) g_fs_fds[i] = -1;

    rpmb_ro_init();

    g_tee_fd = open("/dev/tee0", O_RDWR | O_CLOEXEC);
    if (g_tee_fd < 0) { perror("open /dev/tee0"); return 1; }

    uint32_t uid = geteuid();
    uint8_t cred[] = {
        0xa2, 1, 0x1a, (uint8_t)(uid>>24), (uint8_t)(uid>>16), (uint8_t)(uid>>8), (uint8_t)uid,
        3, 0x65, 'q', 'w', 'e', 's', 'd'
    };
    struct tee_ioctl_param p_reg[2] = {
        { .attr = TEE_ATTR_UBUF_IN, .a = (uint64_t)(uintptr_t)cred, .b = sizeof(cred) },
        { .attr = TEE_ATTR_OBJREF_OUT }
    };
    uint32_t sret = 0;
    if (tee_invoke(g_tee_fd, TEE_OBJREF_NULL, 1, p_reg, 2, &sret) < 0 || sret != 0) {
        fprintf(stderr, "registerLegacy failed: ret=%u errno=%d\n", sret, errno);
        return 1;
    }
    uint64_t env_id = p_reg[1].a;

    /* 1. Register QTEE listeners (FS, TIME, RO-RPMB, GPFS) */
    uint32_t cbo_uid = 87;
    struct tee_ioctl_param p_cbo[2] = {
        { .attr = TEE_ATTR_UBUF_IN, .a = (uint64_t)(uintptr_t)&cbo_uid, .b = 4 },
        { .attr = TEE_ATTR_OBJREF_OUT }
    };
    if (tee_invoke(g_tee_fd, env_id, 0, p_cbo, 2, &sret) < 0 || sret != 0) {
        fprintf(stderr, "open IRegisterListenerCBO(87) failed: ret=%u\n", sret);
        return 1;
    }
    uint64_t reg_cbo_id = p_cbo[1].a;

    pthread_t th[2];
    for (int i = 0; i < 2; i++) pthread_create(&th[i], NULL, supp_worker, NULL);

    for (size_t i = 0; i < NUM_LISTENERS; i++) {
        struct listener_slot *ls = &g_listeners[i];
        struct tee_ioctl_shm_alloc_data sa = { .size = ls->buf_size, .flags = 0, .id = 0 };
        ls->shm_fd = ioctl(g_tee_fd, TEE_IOC_SHM_ALLOC, &sa);
        if (ls->shm_fd < 0) { perror("TEE_IOC_SHM_ALLOC"); return 1; }
        ls->shm_id = sa.id;
        ls->shm_addr = mmap(NULL, sa.size, PROT_READ | PROT_WRITE, MAP_SHARED, ls->shm_fd, 0);
        if (ls->shm_addr == MAP_FAILED) { perror("mmap shm"); return 1; }
        memset(ls->shm_addr, 0, sa.size);
        struct tee_ioctl_param reg_p[3] = {
            { .attr = TEE_ATTR_UBUF_IN, .a = (uint64_t)(uintptr_t)&ls->svc_id, .b = 4 },
            { .attr = TEE_ATTR_OBJREF_IN, .a = ls->cb_id, .b = QCOMTEE_OBJREF_FLAG_USER },
            { .attr = TEE_ATTR_OBJREF_IN, .a = (uint64_t)ls->shm_id, .b = QCOMTEE_OBJREF_FLAG_MEM },
        };
        sret = 0xffffffff;
        int rc = tee_invoke(g_tee_fd, reg_cbo_id, 0, reg_p, 3, &sret);
        printf("qwes-bridge: listener %s (0x%x): ioctl=%d ret=%u\n", ls->name, ls->svc_id, rc, sret);
    }

    /* 2. Open IPFM(119) and verify installed licenses */
    uint32_t pfm_uid = 119;
    struct tee_ioctl_param p_pfm[2] = {
        { .attr = TEE_ATTR_UBUF_IN, .a = (uint64_t)(uintptr_t)&pfm_uid, .b = 4 },
        { .attr = TEE_ATTR_OBJREF_OUT }
    };
    if (tee_invoke(g_tee_fd, env_id, 0, p_pfm, 2, &sret) < 0 || sret != 0) {
        fprintf(stderr, "open IPFM(119) failed: ret=%u errno=%d\n", sret, errno);
        return 1;
    }
    uint64_t ipfm_id = p_pfm[1].a;

    uint8_t fids[1024] = {0};
    struct tee_ioctl_param p_fids[1] = {
        { .attr = TEE_ATTR_UBUF_OUT, .a = (uint64_t)(uintptr_t)fids, .b = sizeof(fids) }
    };
    sret = 0xffffffff;
    if (tee_invoke(g_tee_fd, ipfm_id, 5, p_fids, 1, &sret) == 0) {
        printf("qwes-bridge: QTEE IPFM(119) self-check op=5 ret=%u fids_len=%llu\n",
               sret, (unsigned long long)p_fids[0].b);
    }

    /* 3. Register QRTR Service 1059 (0x423) */
    int sock = socket(AF_QIPCRTR, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (sock < 0) { perror("socket AF_QIPCRTR"); return 1; }

    struct sockaddr_qrtr local;
    socklen_t sl = sizeof(local);
    if (getsockname(sock, (struct sockaddr *)&local, &sl) < 0) { perror("getsockname"); return 1; }
    if (local.sq_port == 0) {
        if (bind(sock, (struct sockaddr *)&local, sizeof(local)) < 0) { perror("bind qrtr"); return 1; }
        sl = sizeof(local);
        if (getsockname(sock, (struct sockaddr *)&local, &sl) < 0) { perror("getsockname2"); return 1; }
    }

    struct sockaddr_qrtr ctrl = { .sq_family = AF_QIPCRTR, .sq_node = local.sq_node, .sq_port = QRTR_PORT_CTRL };
    struct qrtr_ctrl_pkt cpkt;
    memset(&cpkt, 0, sizeof(cpkt));
    cpkt.cmd = QRTR_TYPE_NEW_SERVER;
    cpkt.server.service = QMI_QWES_SERVICE_ID;
    cpkt.server.instance = QMI_QWES_VERSION | (QMI_QWES_INSTANCE << 8);
    cpkt.server.node = local.sq_node;
    cpkt.server.port = local.sq_port;
    if (sendto(sock, &cpkt, sizeof(cpkt), 0, (struct sockaddr *)&ctrl, sizeof(ctrl)) < 0) {
        perror("sendto NEW_SERVER"); return 1;
    }
    printf("qwes-bridge: registered QRTR service %u (0x%x) ver=%u inst=%u at node=%u port=%u\n",
           QMI_QWES_SERVICE_ID, QMI_QWES_SERVICE_ID, QMI_QWES_VERSION, QMI_QWES_INSTANCE,
           local.sq_node, local.sq_port);

    /* Also watch for Service 85 (sec_channel) for diagnostics */
    memset(&cpkt, 0, sizeof(cpkt));
    cpkt.cmd = QRTR_TYPE_NEW_LOOKUP;
    cpkt.server.service = QMI_SECCHAN_SVC_ID;
    sendto(sock, &cpkt, sizeof(cpkt), 0, (struct sockaddr *)&ctrl, sizeof(ctrl));

    uint16_t ind_txn = 1;
    uint8_t buf[4096];
    while (!g_stop) {
        struct sockaddr_qrtr peer;
        socklen_t plen = sizeof(peer);
        ssize_t n = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&peer, &plen);
        if (n < 0) {
            if (errno == EINTR) continue;
            perror("recvfrom");
            break;
        }
        if (peer.sq_port == QRTR_PORT_CTRL) {
            if (n >= (ssize_t)sizeof(struct qrtr_ctrl_pkt)) {
                struct qrtr_ctrl_pkt *c = (struct qrtr_ctrl_pkt *)buf;
                if (c->cmd == QRTR_TYPE_BYE || c->cmd == QRTR_TYPE_NEW_SERVER || c->cmd == QRTR_TYPE_DEL_SERVER) {
                    if (c->server.service != 0 || c->cmd == QRTR_TYPE_BYE)
                        printf("qwes-bridge: ctrl cmd=%u svc=%u inst=0x%x node=%u port=%u\n",
                               c->cmd, c->server.service, c->server.instance, c->server.node, c->server.port);
                }
            }
            continue;
        }
        if (n < 7) continue;

        uint8_t qtype = buf[0];
        uint16_t qtxn = buf[1] | ((uint16_t)buf[2] << 8);
        uint16_t qmsg = buf[3] | ((uint16_t)buf[4] << 8);
        uint16_t qlen = buf[5] | ((uint16_t)buf[6] << 8);
        printf("qwes-bridge: RX node=%u port=%u type=%u txn=%u msg=0x%04x len=%u\n",
               peer.sq_node, peer.sq_port, qtype, qtxn, qmsg, qlen);

        if (qtype != QMI_TYPE_REQ || (qmsg != 0x0020 && qmsg != 0x0021)) continue;

        uint32_t tlv1 = 0;
        uint16_t req_txn_id = 0;
        uint8_t is_secure = 0;
        const uint8_t *payload = NULL;
        uint16_t payload_len = 0;

        size_t pos = 7;
        size_t end = 7 + qlen;
        if (end > (size_t)n) end = (size_t)n;
        while (pos + 3 <= end) {
            uint8_t tag = buf[pos];
            uint16_t tlen = buf[pos+1] | ((uint16_t)buf[pos+2] << 8);
            pos += 3;
            if (pos + tlen > end) break;
            if (tag == 0x01 && tlen >= 4) memcpy(&tlv1, buf + pos, 4);
            else if (tag == 0x02 && tlen >= 2) req_txn_id = buf[pos] | ((uint16_t)buf[pos+1] << 8);
            else if (tag == 0x03 && tlen >= 1) is_secure = buf[pos];
            else if (tag == 0x04 && tlen >= 2) {
                uint16_t arr_len = buf[pos] | ((uint16_t)buf[pos+1] << 8);
                if (2u + arr_len <= tlen) { payload = buf + pos + 2; payload_len = arr_len; }
            }
            pos += tlen;
        }
        printf("  req: tlv1=%u req_txn_id=%u is_secure=%u payload_len=%u\n",
               tlv1, req_txn_id, is_secure, payload_len);

        uint8_t resp_tlvs[14] = {
            0x02, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x10, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00
        };
        send_qmi(sock, &peer, QMI_TYPE_RESP, qtxn, qmsg, resp_tlvs, sizeof(resp_tlvs));

        uint8_t out_buf[1024];
        memset(out_buf, 0, sizeof(out_buf));
        size_t out_len = 0;
        int svc_err = 0;
        int ok = 0;
        if (payload && payload_len > 0) {
            if (ipfm_check_license(g_tee_fd, ipfm_id, is_secure, payload, payload_len,
                                   out_buf, &out_len, &svc_err) == 0) {
                ok = 1;
            }
        }
        printf("  IPFM(op=%u): ok=%d svc_err=%d out_len=%zu\n",
               is_secure ? 4u : 7u, ok, svc_err, out_len);
        if (out_len > 0) hexdump("  ipfm_out", out_buf, out_len);

        uint8_t ind_tlvs[1100];
        size_t p = 0;
        uint32_t ind_status = ok ? 0 : 6;
        uint8_t ind_secure = (ok && is_secure) ? 1 : 0;
        uint16_t ind_olen = (out_len <= 1024) ? (uint16_t)out_len : 1024;

        ind_tlvs[p++] = 0x01; ind_tlvs[p++] = 4; ind_tlvs[p++] = 0;
        memcpy(ind_tlvs + p, &ind_status, 4); p += 4;
        ind_tlvs[p++] = 0x02; ind_tlvs[p++] = 2; ind_tlvs[p++] = 0;
        ind_tlvs[p++] = req_txn_id & 0xff; ind_tlvs[p++] = (req_txn_id >> 8) & 0xff;
        ind_tlvs[p++] = 0x03; ind_tlvs[p++] = 1; ind_tlvs[p++] = 0;
        ind_tlvs[p++] = ind_secure;
        uint16_t t4len = 2 + ind_olen;
        ind_tlvs[p++] = 0x04; ind_tlvs[p++] = t4len & 0xff; ind_tlvs[p++] = (t4len >> 8) & 0xff;
        ind_tlvs[p++] = ind_olen & 0xff; ind_tlvs[p++] = (ind_olen >> 8) & 0xff;
        if (ind_olen) { memcpy(ind_tlvs + p, out_buf, ind_olen); p += ind_olen; }

        if (send_qmi(sock, &peer, QMI_TYPE_IND, ind_txn++, 0x0020, ind_tlvs, (uint16_t)p) == 0) {
            printf("  sent IND 0x0020 (status=%u txn_id=%u is_secure=%u out_len=%u)\n",
                   ind_status, req_txn_id, ind_secure, ind_olen);
        }
    }

    close(sock);
    close(g_tee_fd);
    return 0;
}
