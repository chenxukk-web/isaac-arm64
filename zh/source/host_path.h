#ifndef HOST_PATH_H
#define HOST_PATH_H

/* 把 Windows 风格的路径解析成磁盘上真实存在的路径（大小写不敏感）。
 * 返回的指针指向内部缓存，下次调用前有效；完全找不到时返回 NULL。 */
const char *host_path_resolve(const char *path);

#endif
