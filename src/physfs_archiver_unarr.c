/*
 * RAR, TAR, ZIP and 7z support for PhysicsFS through libunarr.
 *
 * Please see the file LICENSE.txt in the source's root directory.
 */

#define __PHYSICSFS_INTERNAL__
#include "physfs_internal.h"

#if PHYSFS_SUPPORTS_ZIP || PHYSFS_SUPPORTS_7Z || PHYSFS_SUPPORTS_TAR || PHYSFS_SUPPORTS_RAR

#include <unarr.h>
#include <stdint.h>

typedef enum
{
    UNARR_FORMAT_ZIP,
    UNARR_FORMAT_7Z,
    UNARR_FORMAT_TAR,
    UNARR_FORMAT_RAR
} UnarrFormat;

typedef struct
{
    __PHYSFS_DirTreeEntry tree;
    off64_t offset;
    size_t size;
    PHYSFS_sint64 mtime;
} UnarrEntry;

typedef struct
{
    __PHYSFS_DirTree tree;
    PHYSFS_Io *io;
    UnarrFormat format;
} UnarrInfo;

typedef struct
{
    PHYSFS_Io *io;
    ar_stream *stream;
    ar_archive *archive;
    UnarrFormat format;
    off64_t entry_offset;
} UnarrFileInfo;

typedef struct
{
    PHYSFS_Io *io;
} UnarrIo;

static void unarrIoClose(void *opaque)
{
    UnarrIo *bridge = (UnarrIo *) opaque;
    bridge->io->destroy(bridge->io);
    allocator.Free(bridge);
}

static size_t unarrIoRead(void *opaque, void *buffer, size_t count)
{
    UnarrIo *bridge = (UnarrIo *) opaque;
    const PHYSFS_sint64 rc = bridge->io->read(bridge->io, buffer, (PHYSFS_uint64) count);
    return (rc > 0) ? (size_t) rc : 0;
}

static bool unarrIoSeek(void *opaque, off64_t offset, int origin)
{
    UnarrIo *bridge = (UnarrIo *) opaque;
    PHYSFS_sint64 base;
    PHYSFS_uint64 position;

    if (origin == SEEK_SET)
        base = 0;
    else if (origin == SEEK_CUR)
        base = bridge->io->tell(bridge->io);
    else if (origin == SEEK_END)
        base = bridge->io->length(bridge->io);
    else
        return false;

    if (base < 0 || (offset < 0 && base < -offset) || (offset > 0 && base > INT64_MAX - offset))
        return false;
    position = (PHYSFS_uint64) (base + offset);
    return bridge->io->seek(bridge->io, position) != 0;
}

static off64_t unarrIoTell(void *opaque)
{
    UnarrIo *bridge = (UnarrIo *) opaque;
    return (off64_t) bridge->io->tell(bridge->io);
}

static ar_stream *unarrOpenStream(PHYSFS_Io *io)
{
    UnarrIo *bridge = (UnarrIo *) allocator.Malloc(sizeof (UnarrIo));
    if (!bridge)
    {
        io->destroy(io);
        PHYSFS_setErrorCode(PHYSFS_ERR_OUT_OF_MEMORY);
        return NULL;
    }
    bridge->io = io;
    return ar_open_stream(bridge, unarrIoClose, unarrIoRead, unarrIoSeek, unarrIoTell);
}

static ar_archive *unarrOpenArchive(ar_stream *stream, UnarrFormat format)
{
    switch (format)
    {
        case UNARR_FORMAT_ZIP: return ar_open_zip_archive(stream, false);
        case UNARR_FORMAT_7Z: return ar_open_7z_archive(stream, 0);
        case UNARR_FORMAT_TAR: return ar_open_tar_archive(stream);
        case UNARR_FORMAT_RAR: return ar_open_rar_archive(stream);
        default: return NULL;
    }
}

static PHYSFS_sint64 unarrFileTime(time64_t value)
{
    const uint64_t epoch = UINT64_C(116444736000000000);
    const uint64_t ticks = (uint64_t) value;
    if (!value || ticks < epoch)
        return -1;
    return (PHYSFS_sint64) ((ticks - epoch) / UINT64_C(10000000));
}

static int unarrLoadEntries(UnarrInfo *info, ar_archive *archive)
{
    if (!__PHYSFS_DirTreeInit(&info->tree, sizeof (UnarrEntry), 1, 0))
        return 0;

    while (ar_parse_entry(archive))
    {
        const char *name = ar_entry_get_name(archive);
        const int isdir = ar_entry_is_directory(archive) ? 1 : 0;
        UnarrEntry *entry;
        char *mutable_name;

        if (!name)
            continue;
        mutable_name = __PHYSFS_strdup(name);
        if (!mutable_name)
            return 0;
        entry = (UnarrEntry *) __PHYSFS_DirTreeAdd(&info->tree, mutable_name, isdir);
        allocator.Free(mutable_name);
        if (!entry)
            return 0;
        entry->offset = ar_entry_get_offset(archive);
        entry->size = isdir ? 0 : ar_entry_get_size(archive);
        entry->mtime = unarrFileTime(ar_entry_get_filetime(archive));
    }

    return ar_at_eof(archive) ? 1 : 0;
}

static void unarrCloseArchive(void *opaque)
{
    UnarrInfo *info = (UnarrInfo *) opaque;
    if (!info)
        return;
    __PHYSFS_DirTreeDeinit(&info->tree);
    if (info->io)
        info->io->destroy(info->io);
    allocator.Free(info);
}

static void *unarrOpenArchiveCommon(PHYSFS_Io *io, int forWriting, int *claimed, UnarrFormat format)
{
    PHYSFS_Io *scanIo = NULL;
    ar_stream *stream = NULL;
    ar_archive *archive = NULL;
    UnarrInfo *info = NULL;

    BAIL_IF(forWriting, PHYSFS_ERR_READ_ONLY, NULL);
    *claimed = 0;

    scanIo = io->duplicate(io);
    BAIL_IF_ERRPASS(!scanIo, NULL);
    stream = unarrOpenStream(scanIo);
    if (!stream)
        return NULL;
    archive = unarrOpenArchive(stream, format);
    if (!archive)
    {
        ar_close(stream);
        return NULL;
    }
    *claimed = 1;

    info = (UnarrInfo *) allocator.Malloc(sizeof (UnarrInfo));
    GOTO_IF(!info, PHYSFS_ERR_OUT_OF_MEMORY, failed);
    memset(info, 0, sizeof (*info));
    info->io = io;
    info->format = format;

    GOTO_IF_ERRPASS(!unarrLoadEntries(info, archive), failed);

    ar_close_archive(archive);
    ar_close(stream);
    return info;

failed:
    ar_close_archive(archive);
    ar_close(stream);
    if (info)
    {
        info->io = NULL;
        unarrCloseArchive(info);
    }
    return NULL;
}

static PHYSFS_sint64 unarrRead(PHYSFS_Io *io, void *buffer, PHYSFS_uint64 len)
{
    UnarrFileInfo *finfo = (UnarrFileInfo *) io->opaque;
    size_t count = (len > (PHYSFS_uint64) SIZE_MAX) ? SIZE_MAX : (size_t) len;
    size_t rc = ar_entry_read(finfo->archive, buffer, count);
    if (rc == 0 && count != 0 && ar_entry_tell(finfo->archive) < (off64_t) ar_entry_size(finfo->archive))
        BAIL(PHYSFS_ERR_CORRUPT, -1);
    return (PHYSFS_sint64) rc;
}

static PHYSFS_sint64 unarrWrite(PHYSFS_Io *io, const void *buffer, PHYSFS_uint64 len)
{
    BAIL(PHYSFS_ERR_READ_ONLY, -1);
}

static int unarrSeek(PHYSFS_Io *io, PHYSFS_uint64 offset)
{
    UnarrFileInfo *finfo = (UnarrFileInfo *) io->opaque;
    BAIL_IF(!__PHYSFS_ui64FitsAddressSpace(offset), PHYSFS_ERR_PAST_EOF, 0);
    BAIL_IF(offset > (PHYSFS_uint64) ar_entry_get_size(finfo->archive), PHYSFS_ERR_PAST_EOF, 0);
    return ar_entry_seek(finfo->archive, (off64_t) offset, SEEK_SET) ? 1 : 0;
}

static PHYSFS_sint64 unarrTell(PHYSFS_Io *io)
{
    UnarrFileInfo *finfo = (UnarrFileInfo *) io->opaque;
    return (PHYSFS_sint64) ar_entry_tell(finfo->archive);
}

static PHYSFS_sint64 unarrLength(PHYSFS_Io *io)
{
    UnarrFileInfo *finfo = (UnarrFileInfo *) io->opaque;
    return (PHYSFS_sint64) ar_entry_get_size(finfo->archive);
}

static void unarrDestroyFile(PHYSFS_Io *io)
{
    UnarrFileInfo *finfo = (UnarrFileInfo *) io->opaque;
    ar_close_archive(finfo->archive);
    ar_close(finfo->stream);
    allocator.Free(finfo);
    allocator.Free(io);
}

static PHYSFS_Io *unarrDuplicate(PHYSFS_Io *io);
static int unarrFlush(PHYSFS_Io *io);

static PHYSFS_Io *unarrCreateFileIo(PHYSFS_Io *source, UnarrFormat format, off64_t entry_offset, size_t position)
{
    PHYSFS_Io *retval = NULL;
    UnarrFileInfo *finfo = NULL;
    ar_stream *stream = NULL;
    ar_archive *archive = NULL;

    stream = unarrOpenStream(source);
    if (!stream)
        return NULL;
    archive = unarrOpenArchive(stream, format);
    GOTO_IF(!archive, PHYSFS_ERR_CORRUPT, failed);
    GOTO_IF(!ar_parse_entry_at(archive, entry_offset), PHYSFS_ERR_CORRUPT, failed);
    GOTO_IF(!ar_entry_seek(archive, (off64_t) position, SEEK_SET), PHYSFS_ERR_IO, failed);

    retval = (PHYSFS_Io *) allocator.Malloc(sizeof (PHYSFS_Io));
    GOTO_IF(!retval, PHYSFS_ERR_OUT_OF_MEMORY, failed);
    finfo = (UnarrFileInfo *) allocator.Malloc(sizeof (UnarrFileInfo));
    GOTO_IF(!finfo, PHYSFS_ERR_OUT_OF_MEMORY, failed);

    finfo->io = source;
    finfo->stream = stream;
    finfo->archive = archive;
    finfo->format = format;
    finfo->entry_offset = entry_offset;

    retval->version = CURRENT_PHYSFS_IO_API_VERSION;
    retval->opaque = finfo;
    retval->read = unarrRead;
    retval->write = unarrWrite;
    retval->seek = unarrSeek;
    retval->tell = unarrTell;
    retval->length = unarrLength;
    retval->duplicate = unarrDuplicate;
    retval->flush = unarrFlush;
    retval->destroy = unarrDestroyFile;
    return retval;

failed:
    if (archive)
        ar_close_archive(archive);
    if (stream)
        ar_close(stream);
    if (finfo)
        allocator.Free(finfo);
    if (retval)
        allocator.Free(retval);
    return NULL;
}

static PHYSFS_Io *unarrDuplicate(PHYSFS_Io *io)
{
    UnarrFileInfo *finfo = (UnarrFileInfo *) io->opaque;
    PHYSFS_Io *source = finfo->io->duplicate(finfo->io);
    PHYSFS_Io *retval;
    BAIL_IF_ERRPASS(!source, NULL);
    retval = unarrCreateFileIo(source, finfo->format, finfo->entry_offset, ar_entry_tell(finfo->archive));
    return retval;
}

static int unarrFlush(PHYSFS_Io *io)
{
    return 1;
}

static PHYSFS_Io *unarrOpenRead(void *opaque, const char *path)
{
    UnarrInfo *info = (UnarrInfo *) opaque;
    UnarrEntry *entry = (UnarrEntry *) __PHYSFS_DirTreeFind(&info->tree, path);
    PHYSFS_Io *source;
    PHYSFS_Io *retval;

    BAIL_IF_ERRPASS(!entry, NULL);
    BAIL_IF(entry->tree.isdir, PHYSFS_ERR_NOT_A_FILE, NULL);

    source = info->io->duplicate(info->io);
    BAIL_IF_ERRPASS(!source, NULL);
    retval = unarrCreateFileIo(source, info->format, entry->offset, 0);
    return retval;
}

static PHYSFS_Io *unarrOpenWrite(void *opaque, const char *filename)
{
    BAIL(PHYSFS_ERR_READ_ONLY, NULL);
}

static PHYSFS_Io *unarrOpenAppend(void *opaque, const char *filename)
{
    BAIL(PHYSFS_ERR_READ_ONLY, NULL);
}

static int unarrRemove(void *opaque, const char *name)
{
    BAIL(PHYSFS_ERR_READ_ONLY, 0);
}

static int unarrMkdir(void *opaque, const char *name)
{
    BAIL(PHYSFS_ERR_READ_ONLY, 0);
}

static int unarrStat(void *opaque, const char *path, PHYSFS_Stat *stat)
{
    UnarrInfo *info = (UnarrInfo *) opaque;
    UnarrEntry *entry = (UnarrEntry *) __PHYSFS_DirTreeFind(&info->tree, path);
    BAIL_IF_ERRPASS(!entry, 0);

    stat->filesize = entry->tree.isdir ? -1 : (PHYSFS_sint64) entry->size;
    stat->filetype = entry->tree.isdir ? PHYSFS_FILETYPE_DIRECTORY : PHYSFS_FILETYPE_REGULAR;
    stat->modtime = entry->mtime;
    stat->createtime = -1;
    stat->accesstime = -1;
    stat->readonly = 1;
    return 1;
}

#define DEFINE_UNARR_ARCHIVER(symbol, extension, description, format) \
static void *unarrOpenArchive##symbol(PHYSFS_Io *io, const char *name, int forWriting, int *claimed) \
{ \
    return unarrOpenArchiveCommon(io, forWriting, claimed, format); \
} \
const PHYSFS_Archiver __PHYSFS_Archiver_##symbol = \
{ \
    CURRENT_PHYSFS_ARCHIVER_API_VERSION, \
    { extension, description, "libunarr / PhysicsFS", "https://github.com/StochasticEagle/psp-physfs", 0 }, \
    unarrOpenArchive##symbol, \
    __PHYSFS_DirTreeEnumerate, \
    unarrOpenRead, \
    unarrOpenWrite, \
    unarrOpenAppend, \
    unarrRemove, \
    unarrMkdir, \
    unarrStat, \
    unarrCloseArchive \
}

#if PHYSFS_SUPPORTS_ZIP
DEFINE_UNARR_ARCHIVER(ZIP, "ZIP", "ZIP archives through libunarr", UNARR_FORMAT_ZIP);
#endif
#if PHYSFS_SUPPORTS_7Z
DEFINE_UNARR_ARCHIVER(7Z, "7Z", "7zip archives through libunarr", UNARR_FORMAT_7Z);
#endif
#if PHYSFS_SUPPORTS_TAR
DEFINE_UNARR_ARCHIVER(TAR, "TAR", "TAR archives through libunarr", UNARR_FORMAT_TAR);
#endif
#if PHYSFS_SUPPORTS_RAR
DEFINE_UNARR_ARCHIVER(RAR, "RAR", "RAR archives through libunarr", UNARR_FORMAT_RAR);
#endif

#endif
