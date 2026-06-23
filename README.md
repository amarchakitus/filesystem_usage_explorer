# fue — Filesystem Usage Explorer

A fast, Lustre-friendly disk usage analyzer for Linux, written in C.

`fue` reports the apparent size, allocated blocks, and file-type breakdown of a
file or directory tree. It is designed to run efficiently on parallel
filesystems such as [Lustre](https://wiki.lustre.org/) by using the `statx()`
"lazy size on MDT" feature, which lets it query approximate sizes from the
metadata server without forcing a synchronous round-trip to every object
storage target. It works for ordinary, non-privileged users and caches its
results so repeated queries on the same path return instantly.

## Features

- **Lustre-aware** — uses `statx()` with the `AT_STATX_DONT_SYNC` flag to read
  cached (approximate) sizes from the Lustre MDT, avoiding expensive
  synchronization with OSTs. Use `--sync-size` when you need exact values.
- **Non-privileged** — runs as any user; permission-denied errors are counted
  and reported rather than aborting the traversal.
- **Iterative traversal** — directories are walked with an explicit heap stack,
  so arbitrarily deep trees won't overflow the call stack. Symlinks are not
  followed; `.` and `..` are skipped.
- **Result caching** — results are cached per canonical path in `$TMPDIR`
  (default `/tmp`) and timestamped. A cache hit reports how old the cached
  result is.
- **Human-readable or CSV output** — readable report by default, or
  machine-parsable CSV with `--csv`.
- **Live progress** — optional progress counter updated every 100 entries.

## Building

The build uses standard GNU Makefile conventions and depends only on a C11
compiler and a Linux kernel/glibc with `statx()` support (Linux 4.11+,
glibc 2.28+).

```sh
make            # build ./fue
make check      # build and run a smoke test against the current directory
make clean      # remove build artifacts
```

Build flags can be overridden on the command line, for example:

```sh
make CC=clang CFLAGS='-O3 -march=native'
```

### Installation

Installs to `/usr/local` by default, honoring `prefix` and `DESTDIR`:

```sh
sudo make install                 # -> /usr/local/bin/fue
make install prefix=$HOME/.local  # user-local install
make install-strip                # install with symbols stripped
sudo make uninstall
```

## Usage

```
fue [options] <path>
```

| Option         | Description                                                             |
| -------------- | ----------------------------------------------------------------------- |
| `--csv`        | Output in parsable CSV format.                                          |
| `--sync-size`  | Turn off `AT_STATX_DONT_SYNC` to get exact (synchronized) sizes.        |
| `--no-cache`   | Do not read an existing cache. The cache is still written afterwards.   |
| `--progress`   | Show a live counter of processed entries (updated every 100), on stderr.|
| `-h`, `--help` | Show usage and exit.                                                    |

Exactly one target path is required.

### Examples

```sh
# Analyze a directory tree (human-readable)
fue /scratch/project_2001234

# Force fresh results, ignoring any cache, with progress
fue --no-cache --progress /scratch/project_2001234

# Machine-parsable output for scripting
fue --csv /scratch/project_2001234 > usage.csv

# Exact sizes synchronized from the OSTs (slower on Lustre)
fue --sync-size /scratch/project_2001234
```

### Output

The default report looks like:

```
--------------------------------------------------
Disk Usage Analysis Report
--------------------------------------------------
Canonical Path:           /scratch/project_2001234
Total Apparent Size:      1.42 TiB (1561893203968 bytes)
Total Allocated Blocks:   3050573056 (allocated size: 1.42 TiB)
Regular Files Count:      284913
Directories Count:        15022
Symbolic Links Count:     431
Other Files Count:        7
Permission Denied Errors: 12
--------------------------------------------------
```

- **Total Apparent Size** is the sum of `st_size` over all entries (the logical
  file size).
- **Total Allocated Blocks** is the sum of `st_blocks` (512-byte units); the
  "allocated size" shown alongside is `blocks × 512`, reflecting actual space
  consumed.
- **Permission Denied Errors** counts entries that could not be read; the
  traversal continues past them.

In `--csv` mode the same data is emitted as a header row plus one data row, with
the canonical path quoted:

```
canonical_path,total_size_bytes,total_blocks,reg_files,directories,symlinks,other,perm_denied_errors
"/scratch/project_2001234",1561893203968,3050573056,284913,15022,431,7,12
```

When a cache is hit, the cache age is reported. In default mode this prints to
stdout (`Cache loaded. Cache age: ...`); in `--csv` mode it is written to stderr
(`Cache hit. Cache age: ...`) to keep stdout clean for parsing.

## Caching

On each run `fue` resolves the target to its canonical path (`realpath()`) and
derives a cache filename from a 64-bit FNV-1a hash of that path:

```
$TMPDIR/fue_cache_<16-hex-hash>.bin
```

(`$TMPDIR` defaults to `/tmp`.) Cache files are written with `0600`
permissions and opened with `O_NOFOLLOW` to avoid symlink attacks. The full
canonical path is stored in the cache and re-compared on read, so hash
collisions are harmless — a mismatch is simply treated as a cache miss.

- A successful run always writes the cache.
- A subsequent run reads it and returns immediately, reporting the cache age.
- `--no-cache` skips reading the cache but still refreshes it.

Caches do not expire automatically; delete the `fue_cache_*.bin` files (or use
`--no-cache`) to force a recomputation.

## Notes & limitations

- Symbolic links are reported and counted but never traversed.
- With the default lazy-size mode, sizes reflect the MDT's possibly-stale view;
  use `--sync-size` for authoritative numbers at the cost of performance.
- Individual paths longer than `PATH_MAX` (4096) are skipped with a warning.

## References

- [`statx(2)` manual page](https://man7.org/linux/man-pages/man2/statx.2.html)
- [Lazy Size on MDS (LUG 2018)](https://wiki.lustre.org/images/4/4f/LUG2018-Lazy_Size_on_MDS-Xi.pdf)
