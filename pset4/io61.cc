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
    int fd = -1; // File descriptor
    int mode;

    // Size of the cache block
    static constexpr off_t bufsize = 65536;

    unsigned char cbuf[bufsize]; // Read buffer 
    // The following “tags” are addresses—file offsets—that describe the cache’s contents.
    // `tag`: File offset of first byte of cached data (0 when file is opened).
    // `end_tag`: File offset one past the last byte of cached data (0 when file is opened).
    // `pos_tag`: Cache position: file offset of the cache. In read caches, this is the file offset of the next character to be read.
    off_t tag, end_tag, pos_tag; // These are read tags

    unsigned char wbuf[bufsize]; // Write buffer
    size_t wcount = 0; // Number of valid byte sin wbuf
    off_t wtag = 0; // File offset of first byte in wbuf
    bool write_active = false; // Desnotes if wbuf currently holds data
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

static int io61_flush_write_cache(io61_file* f) {
    if (!f->write_active || f->wcount == 0) {
        return 0;
    }

    size_t done = 0;
    while (done < f->wcount) {
        ssize_t n = write(f->fd, f->wbuf + done, f->wcount - done);
        if (n > 0) {
            done += (size_t)n;
        }
        else if (n == 0 || errno == EINTR || errno == EAGAIN) { 
            continue;
        }
        else {
            if (n == 0 || errno == EINTR || errno == EAGAIN) {
                continue;        
            }
            f->wcount -= done;  
            return -1;
        }
    }

    f->wtag += (off_t)done;
    f->wcount = 0;
    return 0;
}

static int io61_refill_block_around(io61_file* f, off_t off) {
    // Ensure off is at the end of the cache
    off_t start = off + 1 - io61_file::bufsize;
    if (start < 0) {
        start = 0;
    }

    // Move file offset once and read one block.
    off_t r = lseek(f->fd, start, SEEK_SET);
    if (r == -1) return -1;

    // Fill the buffer once
    ssize_t n = read(f->fd, f->cbuf, (size_t)io61_file::bufsize);
    if (n < 0) { // Retry on failure (if possible)
        if (errno == EINTR || errno == EAGAIN) {
            return io61_refill_block_around(f, off);
        }
        return -1;
    }

    // Set range for cached bytes
    f->tag = start;
    f->end_tag = start + n;
    if (off >= f->end_tag) {
        // If request is beyond end of file, set to end of file
        f->pos_tag = f->end_tag;
    }
    else {
        f->pos_tag = off;
    }
    return 0;
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
        else if (fr < 0) { // Cannot recover from error
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
    unsigned char ch = static_cast<unsigned char>(c);

    // Enter writing mode
    if (!f->write_active) {
        off_t cur = lseek(f->fd, 0, SEEK_CUR);
        if (cur != -1) {
            f->wtag = cur;
        }
        f->write_active = true;
    }
    // Ensure there is room in the buffer
    if (f->wcount == static_cast<size_t>(io61_file::bufsize) && io61_flush_write_cache(f) < 0) {
        return -1;
    }
    // Append the byte to the write cache
    f->wbuf[f->wcount++] = ch;
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
    // Initialize write cache if not already initialized
    if (!f->write_active) {
        off_t cur = lseek(f->fd, 0, SEEK_CUR);
        if (cur != -1) {
            f->wtag = cur;
        }
        f->write_active = true;
    }
    while (total < sz) {
        // Find space left in cache
        size_t space = (size_t)(io61_file::bufsize - f->wcount);
        if (space == 0) {
            // If the cache is full, flush it
            if (io61_flush_write_cache(f) < 0) {
                // If some bytes were already consumed by cache, short write, otherwise error
                return (total > 0) ? (ssize_t)total : -1;
            }
            // Update space left in cache
            space = (size_t)(io61_file::bufsize - f->wcount);
        }
        // Find number of bytes to write 
        size_t want = sz - total;
        size_t ncopy = (want < space ? want : space);

        // Write the bytes into the cache
        memcpy(f->wbuf + f->wcount, buf + total, ncopy);
        f->wcount += ncopy;
        total += ncopy;
    }

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
    // If read-only
    if ((f->mode & O_ACCMODE) == O_RDONLY) {
        return 0;
    }
    // If write-only
    if (f->write_active && f->wcount > 0) {
        if (io61_flush_write_cache(f) < 0) {
            return -1;
        }
    }
    return 0;
}

// io61_seek(f, off)
//    Changes the file pointer for file `f` to `off` bytes into the file.
//    Returns 0 on success and -1 on failure.

int io61_seek(io61_file* f, off_t off) {
    // Flush the buffer before moving the offset
    if (f->write_active && f->wcount > 0) {
        if (io61_flush_write_cache(f) < 0) {
            return -1;
        }
    }

    int acc = (f->mode & O_ACCMODE);

    if (acc == O_WRONLY) {
        // If write only, do not read, just move kernel offset
        off_t r = lseek(f->fd, off, SEEK_SET);
        if (r == (off_t)-1) {
            return -1;
        }
        // Invalidate read cache
        f->tag = f->pos_tag = f->end_tag = off;
        // Reset write cache
        f->write_active = false;
        f->wcount = 0;
        f->wtag = off;
        return 0;
    }
    else { // acc == O_RDONLY
        // if off is in cache, just move pos to off
        if (f->tag <= off && off < f->end_tag) {
            f->pos_tag = off;
            return 0;
        }
        // If not, refill a block around the target
        return io61_refill_block_around(f, off);
    }
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
