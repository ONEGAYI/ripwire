#pragma once
// POSIX 兼容层（实验性 Windows/MinGW 构建用）：
//   - rwMemstreamOpen/rwMemstreamClose：glibc 原生 open_memstream 的可移植替身。
//     MinGW CRT 没有 open_memstream，且 FILE* 的 fclose 无法挂钩，因此把调用方
//     统一的「fflush → fclose → 读 buf」序列收进 rwMemstreamClose：Windows 下
//     写入落 tmpfile（OS 缓存吸收，MB 级近内存速），close 时 fseek(END)+ftell
//     取大小（与 glibc memstream 的“最大写入偏移”语义一致，回退重写不截断旧尾）
//     再整段读回、malloc 拷贝交调用方 free，内存所有权与 glibc 相同。
//   - rwRealpath：MinGW 映射 _fullpath。差异：不解析符号链接（实验性构建的
//     可接受偏差）；resolved 为 nullptr 时自 malloc（glibc 扩展，quality.h 使用）。
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstddef>

#if defined( __GLIBC__ )

inline std::FILE* rwMemstreamOpen( char** buf, std::size_t* sz )
{
    return open_memstream( buf, sz );
}

inline bool rwMemstreamClose( std::FILE* m, char** /*buf*/, std::size_t* /*sz*/ )
{
    const bool flushed = std::fflush( m ) == 0;
    const bool closed  = std::fclose( m ) == 0;
    return flushed && closed;
}

inline char* rwRealpath( const char* path, char* resolved )
{
    return realpath( path, resolved );
}

#else  // MinGW / 无 glibc

#include <climits>
#include <vector>
#include <io.h>
#include <sys/types.h>
// winsock2 必须先于 windows.h；LEAN_AND_MEAN 收窄拖入面
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

// MinGW libstdc++ 的 std::print 终端直写在链接期缺 __open_terminal/__write_to_terminal
// 符号（GCC 15.2 winpthreads 构建已知缺口），强制 emit.h 走 std::format+fputs 路
#define EMIT_FORCE_FALLBACK 1

// windows.h 的 16 位时代段限定符宏会撞碎用户标识符（didyoumean.h 的局部变量 near）
#undef near
#undef far
#undef interface

// ── pathguard 安全层的旗标弱化 ──
// O_NOFOLLOW/O_NONBLOCK：Windows open 无对应旗标，置 0 后退化为普通 open，
// 符号链接防护（isSymlink/S_ISLNK）同步弱化为“恒非链接”——实验性构建的已声明缺口。
#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif
#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0
#endif
#ifndef O_NONBLOCK
#define O_NONBLOCK 0
#endif
#ifndef ELOOP
#define ELOOP 114  // 占位：O_NOFOLLOW=0 后永不产生
#endif
#include <sys/stat.h>
#ifndef S_ISLNK
#define S_ISLNK( m ) 0
#endif
inline int lstat( const char* path, struct stat* st ) { return stat( path, st ); }

// POSIX getline（malloc 增长缓冲的行读取；pathguard 低频读配置行，fgetc 实现足够）
inline ssize_t getline( char** lineptr, size_t* n, std::FILE* stream )
{
    if( lineptr == nullptr || n == nullptr || stream == nullptr ) { return -1; }
    if( *lineptr == nullptr || *n == 0 )
    {
        *n = 128;
        *lineptr = static_cast<char*>( std::realloc( nullptr, *n ) );
        if( *lineptr == nullptr ) { *n = 0; return -1; }
    }
    size_t used = 0;
    for( ;; )
    {
        const int c = std::fgetc( stream );
        if( c == EOF ) { break; }
        if( used + 2 > *n )
        {
            *n *= 2;
            char* grown = static_cast<char*>( std::realloc( *lineptr, *n ) );
            if( grown == nullptr ) { return -1; }
            *lineptr = grown;
        }
        ( *lineptr )[used++] = static_cast<char>( c );
        if( c == '\n' ) { break; }
    }
    if( used == 0 && std::feof( stream ) ) { return -1; }
    ( *lineptr )[used] = '\0';
    return static_cast<ssize_t>( used );
}

// ── BSD socket → winsock2 桥（mcpserver --listen 专用）──
// 调用方以 int 持有描述符并以 <0 判失败；SOCKET 无符号，故 rw_socket/rw_accept
// 在桥内完成 INVALID_SOCKET → -1 的转译。首次调用自动 WSAStartup。
#ifndef SO_NOSIGPIPE
#define SO_NOSIGPIPE 0  // Windows 无 SIGPIPE 概念，置 0 使 setsockopt 调用可编译
#endif
inline int rw_socket( int af, int type, int proto )
{
    static const bool wsaReady = [] { WSADATA d; return WSAStartup( MAKEWORD( 2, 2 ), &d ) == 0; }();
    if( !wsaReady ) { return -1; }
    const SOCKET s = socket( af, type, proto );
    return s == INVALID_SOCKET ? -1 : static_cast<int>( s );
}
inline int rw_accept( int fd, sockaddr* addr, int* len )
{
    const SOCKET s = accept( static_cast<SOCKET>( fd ), addr, len );
    return s == INVALID_SOCKET ? -1 : static_cast<int>( s );
}
inline int rw_setsockopt( int fd, int level, int optname, const void* optval, int optlen )
{
    return setsockopt( static_cast<SOCKET>( fd ), level, optname,
                       static_cast<const char*>( optval ), optlen );
}

// ── 杂项 POSIX 面 ──
// getuid：缓存目录属主校验用；Windows 单用户语义下固定 0，且 MinGW stat 的
// st_uid 亦为 0，目录属主比较恒成立。
inline unsigned int getuid() { return 0; }

// 双参 mkdir → _mkdir（权限位由文件系统默认 ACL 决定）
#include <direct.h>
inline int mkdir( const char* path, int /*mode*/ ) { return _mkdir( path ); }

// chmod：MinGW 提供 _chmod
inline int chmod( const char* path, int mode ) { return _chmod( path, mode ); }

// localtime_r（POSIX 参数序）→ localtime_s（CRT 参数序）
#include <ctime>
inline struct tm* localtime_r( const time_t* timep, struct tm* result )
{
    return localtime_s( result, timep ) == 0 ? result : nullptr;
}

// flock：quality.h 定价目录跨进程锁的功能性等价——LockFileEx 全文件范围。
// LOCKFILE_EXCLUSIVE_LOCK 需要显式重叠结构；区域大小 0xFFFFFFFF 两段（64 位满幅）。
#ifndef LOCK_EX
#define LOCK_EX 2
#endif
#ifndef LOCK_NB
#define LOCK_NB 4
#endif
#ifndef LOCK_UN
#define LOCK_UN 8
#endif
inline int flock( int fd, int operation )
{
    HANDLE h = reinterpret_cast<HANDLE>( _get_osfhandle( fd ) );
    if( h == INVALID_HANDLE_VALUE ) { return -1; }
    OVERLAPPED ov{};
    if( operation & LOCK_UN )
    {
        return UnlockFileEx( h, 0, 0xFFFFFFFF, 0xFFFFFFFF, &ov ) ? 0 : -1;
    }
    const DWORD flags = LOCKFILE_EXCLUSIVE_LOCK
                      | ( ( operation & LOCK_NB ) ? LOCKFILE_FAIL_IMMEDIATELY : 0 );
    return LockFileEx( h, flags, 0, 0xFFFFFFFF, 0xFFFFFFFF, &ov ) ? 0 : -1;
}

// POSIX 定位读的等价实现：同步句柄 + OVERLAPPED 偏移 = 阻塞定位读，
// 不移动文件句柄的当前指针，与 pread 语义一致。
inline ssize_t pread( int fd, void* buf, std::size_t n, off_t off )
{
    HANDLE h = reinterpret_cast<HANDLE>( _get_osfhandle( fd ) );
    if( h == INVALID_HANDLE_VALUE ) { return -1; }
    OVERLAPPED ov{};
    ov.Offset     = static_cast<DWORD>( static_cast<unsigned long long>( off ) & 0xFFFFFFFFull );
    ov.OffsetHigh = static_cast<DWORD>( static_cast<unsigned long long>( off ) >> 32 );
    DWORD got = 0;
    if( !ReadFile( h, buf, static_cast<DWORD>( n ), &got, &ov ) )
    {
        if( GetLastError() == ERROR_HANDLE_EOF ) { return 0; }
        return -1;
    }
    return static_cast<ssize_t>( got );
}

inline std::FILE* rwMemstreamOpen( char** buf, std::size_t* sz )
{
    *buf = nullptr;
    *sz  = 0;
    return std::tmpfile();
}

inline bool rwMemstreamClose( std::FILE* m, char** buf, std::size_t* sz )
{
    if( m == nullptr ) { return false; }
    const bool flushed = std::fflush( m ) == 0;
    // glibc 语义：size 是历史最大写入偏移，而非当前文件指针位置
    const bool seeked  = std::fseek( m, 0, SEEK_END ) == 0;
    const long  end    = seeked ? std::ftell( m ) : -1;
    std::vector<char> data;
    bool        copied = false;
    if( seeked && end >= 0 )
    {
        if( end > 0 )
        {
            std::rewind( m );
            data.resize( static_cast<std::size_t>( end ) );
            copied = std::fread( data.data(), 1, data.size(), m ) == data.size();
        }
        else
        {
            copied = true;  // 空流也是成功
        }
    }
    const bool closed = std::fclose( m ) == 0;
    if( seeked && end >= 0 && copied )
    {
        *sz  = static_cast<std::size_t>( end );
        *buf = static_cast<char*>( std::malloc( *sz + 1 ) );
        if( *buf == nullptr ) { *sz = 0; return false; }
        if( end > 0 ) { std::memcpy( *buf, data.data(), *sz ); }
        ( *buf )[*sz] = '\0';
        return flushed && closed;
    }
    return false;
}

inline char* rwRealpath( const char* path, char* resolved )
{
    if( resolved == nullptr )
    {
        char* heap = static_cast<char*>( std::malloc( PATH_MAX ) );
        if( heap == nullptr ) { return nullptr; }
        char* r = _fullpath( heap, path, PATH_MAX );
        if( r == nullptr ) { std::free( heap ); }
        return r;
    }
    return _fullpath( resolved, path, PATH_MAX );
}

#endif

#if !defined( __GLIBC__ )
// ── 第四批杂项（与上面的分支同为非 glibc 专属；追加时落在 #endif 之外曾破坏 Linux 构建）──
// setenv → _putenv_s（overwrite=0 时先查在值）
inline int setenv( const char* name, const char* value, int overwrite )
{
    if( overwrite == 0 && std::getenv( name ) != nullptr ) { return 0; }
    return _putenv_s( name, value ) == 0 ? 0 : -1;
}

// fchmod：fd 级权限位在 Windows 由 ACL 管，弱化为恒成功
inline int fchmod( int, int ) { return 0; }

// fsync → _commit（真等价：刷 CRT 缓冲并落盘）
inline int fsync( int fd ) { return _commit( fd ); }

// fcntl：scip.h 唯一用途是清除 O_NONBLOCK 后交给 stdio；Windows 描述符恒阻塞，
// F_GETFL 报 flags=0、F_SETFL 恒成功即语义正确。
#ifndef F_GETFL
#define F_GETFL 3
#endif
#ifndef F_SETFL
#define F_SETFL 4
#endif
inline int fcntl( int, int, ... ) { return 0; }
#endif  // !__GLIBC__

// rename 的原子覆盖替身：POSIX rename 语义是目标存在则原子替换，Windows CRT 的 rename
// 在目标存在时失败（缓存/快照的 tmp→final 替换因此每写必败）。
#if defined( _WIN32 )
inline int rwRenameReplace( const char* oldPath, const char* newPath )
{
    if( MoveFileExA( oldPath, newPath, MOVEFILE_REPLACE_EXISTING ) ) { return 0; }
    // Windows 特例：目标仍被本进程的缓存读句柄持有时，REPLACE 式 rename 因 sharing
    // violation 失败（POSIX rename 无此限制）。降级链：remove+rename；仍败则 CopyFile
    // 字节级覆写（非原子，但缓存带 checksum trailer，torn 在下一轮装载时自愈为全量重解析）。
    if( DeleteFileA( newPath ) && MoveFileExA( oldPath, newPath, 0 ) ) { return 0; }
    if( CopyFileA( oldPath, newPath, FALSE ) )
    {
        DeleteFileA( oldPath );
        return 0;
    }
    return -1;
}
#else
inline int rwRenameReplace( const char* oldPath, const char* newPath )
{
    return std::rename( oldPath, newPath );
}
#endif
