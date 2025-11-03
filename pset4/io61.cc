#include "io61.hh"
#include <sys/types.h>
#include <sys/stat.h>
#include <climits>
#include <cerrno>

// io61.cc
//    YOUR CODE HERE!

// io61_file
//    Data structure for io61 file wrappers. Add your own stuff.

struct io61_file {
    int fd = -1;     // file descriptor
    int mode;        // open mode (O_RDONLY or O_WRONLY)

    // `bufsiz` is the cache block size
    static constexpr off_t bufsize = 4096;
    // Cached data is stored in `cbuf`
    unsigned char cbuf[bufsize];

    // The following “tags” are addresses—file offsets—that describe the cache’s contents.
    // `tag`: File offset of first byte of cached data (0 when file is opened).
    off_t tag;
    // `end_tag`: File offset one past the last byte of cached data (0 when file is opened).
    off_t end_tag;
    // `pos_tag`: Cache position: file offset of the cache.
    // In read caches, this is the file offset of the next character to be read.
    off_t pos_tag;
};

ssize_t io61_fill(io61_file* f) {
    // Set the cache as empty
    f->tag = f->pos_tag = f->end_tag;

    while(true) {
        // Fill the buffer with new bytes.
        ssize_t n = read(f->fd, f->cbuf, (size_t)f->bufsize);

        if (n > 0) { // If success (partial or whole)
            // Update span of cache
            f->end_tag = f->tag + n;

            // Check invariants
            assert(f->tag <= f->pos_tag && f->pos_tag <= f->end_tag);
            assert((off_t)(f->end_tag - f->tag) <= f->bufsize);

            return n;
        }
        else if (n == 0) { // End of file
            // Leave cache empty
            errno = 0;
            return 0;
        }
        else { // error
            if (errno == EINTR || errno == EAGAIN) {
                // Retry the read
                continue;
            }
            // Cannot recover
            return -1;
        }
    }
}

// io61_fdopen(fd, mode)
//    Returns a new io61_file for file descriptor `fd`. `mode` is either
//    O_RDONLY for a read-only file or O_WRONLY for a write-only file.
//    You need not support read/write files.

io61_file* io61_fdopen(int fd, int mode) {
    assert(fd >= 0);
    io61_file* f = new io61_file;
    f->fd = fd;
    f->mode = mode;
    f->tag = f->pos_tag = f->end_tag = 0;
    return f;
}



// io61_close(f)
//    Closes the io61_file `f` and releases all its resources.

int io61_close(io61_file* f) {
    io61_flush(f);
    int r = close(f->fd);
    delete f;
    return r;
}


// io61_readc(f)
//    Reads a single (unsigned) byte from `f` and returns it. Returns EOF,
//    which equals -1, on end of file or error.

int io61_readc(io61_file* f) {
    if (f->pos_tag == f->end_tag) {
        ssize_t fr = io61_fill(f);
        if (fr == 0) { // End of file
            return -1;
        }
        else if (fr < 0) {    // hard error
            return -1;
        }
    }
    int ch = f->cbuf[f->pos_tag - f->tag];
    ++f->pos_tag;
    return ch;
}

// io61_read(f, buf, sz)
//    Reads up to `sz` bytes from `f` into `buf`. Returns the number of
//    bytes read on success. Returns 0 if end-of-file is encountered before
//    any bytes are read, and -1 if an error is encountered before any
//    bytes are read.
//
//    Note that the return value might be positive, but less than `sz`,
//    if end-of-file or error is encountered before all `sz` bytes are read.
//    This is called a “short read.”

ssize_t io61_read(io61_file* f, unsigned char* buf, size_t sz) {
    // Handle case where no read is required
    if (sz == 0) {
        return 0;
    }
    
    size_t copied = 0;
    while (copied < sz) {
        if (f->pos_tag == f->end_tag) { // If cache is empty 
            ssize_t fr = io61_fill(f);
            if (fr == 0) { // End of file
                return (copied > 0) ? (ssize_t)copied : 0;
            }
            else if (fr < 0) { // error
                return (copied > 0) ? (ssize_t)copied : -1;
            }
        }
        // Calculate number of bytes copied by finding minimum of wanted bytes and available bytes
        size_t avail = (size_t)(f->end_tag - f->pos_tag);
        size_t want = sz - copied;
        size_t copy = (avail < want ? avail : want);

        // Read the bytes from the cache (memcpy for speed)
        memcpy(buf + copied, f->cbuf + (f->pos_tag - f->tag), copy);
        f->pos_tag += copy;
        copied += copy;
    }

    return (ssize_t)copied;
}



// io61_writec(f)
//    Write a single character `c` to `f` (converted to unsigned char).
//    Returns 0 on success and -1 on error.

int io61_writec(io61_file* f, int c) {
    unsigned char ch = c;
    ssize_t nw = write(f->fd, &ch, 1);
    if (nw != 1) {
        assert(nw == 0 || nw == -1);
        return -1;
    }
    return 0;
}


// io61_write(f, buf, sz)
//    Writes `sz` characters from `buf` to `f`. Returns `sz` on success.
//    Can write fewer than `sz` characters when there is an error, such as
//    a drive running out of space. In this case io61_write returns the
//    number of characters written, or -1 if no characters were written
//    before the error occurred.

ssize_t io61_write(io61_file* f, const unsigned char* buf, size_t sz) {
    // Handle case where no write is required
    if (sz == 0) {
        return 0;
    }

    size_t total = 0;
    while (total < sz) {
        // Calculate and attempt to write remaining bytes wanted
        size_t want = sz - total;
        ssize_t n = write(f->fd, buf + total, want);
        if (n > 0) { // If success
            total += (size_t)n;
            continue; // Continue writing
        }
        else {
            // n == -1 => error
            if (n == 0 || errno == EINTR || errno == EAGAIN) {
                continue; // If error is recoverable, try again
            }
            if (total > 0) {
                return (ssize_t)total; // short read, cannot recover
            }
            else {
                return -1; // cannot recover
            }
        }
    }
    // return number of bytes successfully written
    return (ssize_t)total;
}

// io61_flush(f)
//    If `f` was opened write-only, `io61_flush(f)` forces a write of any
//    cached data written to `f`. Returns 0 on success; returns -1 if an error
//    is encountered before all cached data was written.
//
//    If `f` was opened read-only, `io61_flush(f)` returns 0. It may also
//    drop any data cached for reading.

int io61_flush(io61_file* f) {
    (void) f;
    return 0;
}


// io61_seek(f, off)
//    Changes the file pointer for file `f` to `off` bytes into the file.
//    Returns 0 on success and -1 on failure.

int io61_seek(io61_file* f, off_t off) {
    off_t r = lseek(f->fd, (off_t) off, SEEK_SET);
    // Ignore the returned offset unless it’s an error.
    if (r == -1) {
        return -1;
    }
    // Invalidate read cache at new position
    f->tag = f->pos_tag = f->end_tag = off;
    return 0;
}

// You shouldn't need to change these functions.

// io61_open_check(filename, mode)
//    Opens the file corresponding to `filename` and returns its io61_file.
//    If `!filename`, returns either the standard input or the
//    standard output, depending on `mode`. Exits with an error message if
//    `filename != nullptr` and the named file cannot be opened.

io61_file* io61_open_check(const char* filename, int mode) {
    int fd;
    if (filename) {
        fd = open(filename, mode, 0666);
    } else if ((mode & O_ACCMODE) == O_RDONLY) {
        fd = STDIN_FILENO;
    } else {
        fd = STDOUT_FILENO;
    }
    if (fd < 0) {
        fprintf(stderr, "%s: %s\n", filename, strerror(errno));
        exit(1);
    }
    return io61_fdopen(fd, mode & O_ACCMODE);
}


// io61_fileno(f)
//    Returns the file descriptor associated with `f`.

int io61_fileno(io61_file* f) {
    return f->fd;
}


// io61_filesize(f)
//    Returns the size of `f` in bytes. Returns -1 if `f` does not have a
//    well-defined size (for instance, if it is a pipe).

off_t io61_filesize(io61_file* f) {
    struct stat s;
    int r = fstat(f->fd, &s);
    if (r < 0 || !S_ISREG(s.st_mode)) {
        return -1;
    }
    return s.st_size;
}
